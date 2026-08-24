#include "telemetry_uploader.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <esp_system.h>

#include <algorithm>
#include <cstring>
#include <sys/time.h>

#include "ingest_ack.h"
#include "spool_name.h"
#include "telemetry_json.h"

namespace {

constexpr char kFirmwareVersion[] = "0.3.0";
constexpr char kSpoolDirectory[] = "/spool";
constexpr char kDeadDirectory[] = "/dead";
constexpr char kPendingPath[] = "/spool/.pending";
constexpr size_t kMaxFirmwareBatchSize = 30;
constexpr size_t kMaxSpoolFiles = 256;
constexpr size_t kMaxDeadFiles = 16;
constexpr uint64_t kFreezeIntervalMs = 30000;
constexpr uint64_t kWifiRetryMs = 30000;
constexpr uint64_t kMaximumBackoffMs = 5 * 60 * 1000;
constexpr size_t kMaximumAckBytes = 2048;
constexpr time_t kMinimumTrustedUtcSeconds = 1700000000;

RuntimeIdentity workerIdentity;
TelemetryUploaderSettings workerSettings;
TelemetryQueue* workerQueue = nullptr;
TaskHandle_t workerTaskHandle = nullptr;

class UploaderWorker {
 public:
  void run() {
    while (monotonicMillis() < workerSettings.startAfterUptimeMs) {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!initializeFilesystem()) {
      Serial.println("EVENT,uploader,filesystem_unavailable");
      vTaskDelete(nullptr);
      return;
    }

    spoolCount_ = countFiles(kSpoolDirectory, nullptr, 0);
    deadCount_ = countFiles(kDeadDirectory, nullptr, 0);
    Serial.printf("EVENT,uploader,spool_ready,%u,%u\n",
                  static_cast<unsigned>(spoolCount_),
                  static_cast<unsigned>(deadCount_));

    if (workerSettings.configured) {
      WiFi.persistent(false);
      WiFi.mode(WIFI_STA);
      WiFi.setAutoReconnect(true);
    } else {
      Serial.println("EVENT,uploader,not_provisioned");
    }

    while (true) {
      const uint64_t now = monotonicMillis();
      freezeQueueIfNeeded(now);
      if (workerSettings.configured && !operatorHalted_) {
        maintainWifi(now);
        captureClockAnchor();
        if (WiFi.status() == WL_CONNECTED && now >= nextUploadMs_) {
          uploadOneEnvelope(now);
        }
      }
      vTaskDelay(pdMS_TO_TICKS(250));
    }
  }

 private:
  static bool fileSink(void* context, const char* data, size_t size) {
    File* file = static_cast<File*>(context);
    return file->write(reinterpret_cast<const uint8_t*>(data), size) == size;
  }

  static bool setFilesystemInitializedFlag() {
    Preferences preferences;
    if (!preferences.begin("m5pres", false)) {
      return false;
    }
    const size_t written = preferences.putBool("fs_init", true);
    preferences.end();
    return written == sizeof(bool);
  }

  static bool readFilesystemInitializedFlag(bool* initialized) {
    if (initialized == nullptr) {
      return false;
    }
    Preferences preferences;
    // Open read-write so a genuinely new namespace can be distinguished from
    // an NVS access failure. Never interpret an access failure as first boot.
    if (!preferences.begin("m5pres", false)) {
      return false;
    }
    *initialized = preferences.getBool("fs_init", false);
    preferences.end();
    return true;
  }

  bool initializeFilesystem() {
    bool knownFilesystem = false;
    if (!readFilesystemInitializedFlag(&knownFilesystem)) {
      return false;
    }
    if (!LittleFS.begin(false)) {
      if (knownFilesystem || !LittleFS.format() || !LittleFS.begin(false)) {
        // Never auto-format a filesystem that previously held outage data.
        return false;
      }
    }
    if (!knownFilesystem && !setFilesystemInitializedFlag()) {
      return false;
    }
    if (!LittleFS.exists(kSpoolDirectory) &&
        !LittleFS.mkdir(kSpoolDirectory)) {
      return false;
    }
    if (!LittleFS.exists(kDeadDirectory) &&
        !LittleFS.mkdir(kDeadDirectory)) {
      return false;
    }
    // This name is only used while constructing a new immutable envelope.
    // A prior crash may leave it incomplete, so it is never replayed.
    if (LittleFS.exists(kPendingPath)) {
      LittleFS.remove(kPendingPath);
    }
    return true;
  }

  static size_t countFiles(const char* directory, char* firstPath,
                           size_t firstPathSize) {
    if (firstPath != nullptr && firstPathSize > 0) {
      firstPath[0] = '\0';
    }
    File folder = LittleFS.open(directory);
    if (!folder || !folder.isDirectory()) {
      return 0;
    }

    size_t count = 0;
    File entry = folder.openNextFile();
    while (entry) {
      if (!entry.isDirectory()) {
        ++count;
        const char* path = entry.path();
        if (firstPath != nullptr && path != nullptr &&
            (firstPath[0] == '\0' || std::strcmp(path, firstPath) < 0)) {
          std::snprintf(firstPath, firstPathSize, "%s", path);
        }
      }
      entry.close();
      entry = folder.openNextFile();
    }
    folder.close();
    return count;
  }

  bool findNextSpool(SpoolFileMetadata& metadata) const {
    File folder = LittleFS.open(kSpoolDirectory);
    if (!folder || !folder.isDirectory()) {
      return false;
    }

    bool found = false;
    SpoolFileMetadata best;
    File entry = folder.openNextFile();
    while (entry) {
      if (!entry.isDirectory()) {
        SpoolFileMetadata candidate;
        if (parseSpoolFileMetadata(entry.path(), candidate) &&
            (!found || std::strcmp(candidate.path, best.path) < 0)) {
          best = candidate;
          found = true;
        }
      }
      entry.close();
      entry = folder.openNextFile();
    }
    folder.close();
    if (found) {
      metadata = best;
    }
    return found;
  }

  void freezeQueueIfNeeded(uint64_t now) {
    if (spoolCount_ >= kMaxSpoolFiles || workerQueue == nullptr) {
      return;
    }
    const size_t available = workerQueue->size();
    const size_t requestedBatchSize =
        std::max<size_t>(1, std::min<size_t>(workerSettings.uploadBatchSize,
                                            kMaxFirmwareBatchSize));
    if (available == 0 ||
        (available < requestedBatchSize &&
         now - lastFreezeMs_ < kFreezeIntervalMs)) {
      return;
    }

    TelemetryRecord records[kMaxFirmwareBatchSize] = {};
    const size_t requested = std::min(available, requestedBatchSize);
    const size_t recordCount = workerQueue->copyPrefix(records, requested);
    if (recordCount == 0) {
      return;
    }
    lastFreezeMs_ = now;

    SpoolFileMetadata metadata;
    if (!buildSpoolFileMetadata(workerIdentity.bootId, records[0].seq,
                                records[recordCount - 1].seq,
                                static_cast<uint16_t>(recordCount), metadata)) {
      Serial.println("EVENT,uploader,batch_metadata_invalid");
      return;
    }

    // A reset cannot preserve this RAM prefix. Within one run, however, a
    // completed rename followed by a failed prefix commit must reuse the exact
    // immutable file and never rewrite it with a later clock anchor.
    if (LittleFS.exists(metadata.path)) {
      if (!workerQueue->commitPrefix(records, recordCount)) {
        Serial.println("EVENT,uploader,existing_prefix_mismatch");
      }
      return;
    }

    File pending = LittleFS.open(kPendingPath, FILE_WRITE);
    if (!pending) {
      Serial.println("EVENT,uploader,spool_open_failed");
      return;
    }

    TelemetryBatchContext context;
    context.batchId = metadata.batchId;
    context.bootId = workerIdentity.bootId;
    context.firmwareVersion = kFirmwareVersion;
    context.appliedConfigRevision = workerSettings.appliedConfigRevision;
    context.hasClockAnchor = hasClockAnchor_;
    context.anchorUtcMs = anchorUtcMs_;
    context.anchorUptimeMs = anchorUptimeMs_;
    context.anchorSource = ClockAnchorSource::kSntp;
    const TelemetryJsonSink sink{&pending, fileSink};
    const bool serialized =
        writeTelemetryBatchJson(context, records, recordCount, sink);
    pending.flush();
    const size_t bytesWritten = pending.size();
    pending.close();
    if (!serialized || bytesWritten == 0) {
      LittleFS.remove(kPendingPath);
      Serial.println("EVENT,uploader,spool_serialize_failed");
      return;
    }
    if (!LittleFS.rename(kPendingPath, metadata.path)) {
      LittleFS.remove(kPendingPath);
      Serial.println("EVENT,uploader,spool_commit_failed");
      return;
    }

    ++spoolCount_;
    if (!workerQueue->commitPrefix(records, recordCount)) {
      Serial.println("EVENT,uploader,queue_commit_failed");
      return;
    }
    Serial.printf("EVENT,uploader,spooled,%s,%u\n", metadata.batchId,
                  static_cast<unsigned>(recordCount));
  }

  void maintainWifi(uint64_t now) {
    if (WiFi.status() == WL_CONNECTED) {
      if (!wifiWasConnected_) {
        wifiWasConnected_ = true;
        Serial.printf("EVENT,wifi,connected,%s\n",
                      WiFi.localIP().toString().c_str());
      }
      return;
    }

    if (wifiWasConnected_) {
      wifiWasConnected_ = false;
      Serial.println("EVENT,wifi,disconnected");
    }
    if (!wifiStarted_ || now - lastWifiBeginMs_ >= kWifiRetryMs) {
      lastWifiBeginMs_ = now;
      wifiStarted_ = true;
      WiFi.begin(workerSettings.wifiSsid, workerSettings.wifiPassword);
      Serial.println("EVENT,wifi,connecting");
    }
  }

  void captureClockAnchor() {
    if (hasClockAnchor_ || WiFi.status() != WL_CONNECTED) {
      return;
    }
    if (!sntpStarted_) {
      configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
      sntpStarted_ = true;
    }
    if (esp_sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
      return;
    }

    const uint64_t uptimeBeforeUtcRead = monotonicMillis();
    timeval currentTime = {};
    if (gettimeofday(&currentTime, nullptr) != 0 ||
        currentTime.tv_sec < kMinimumTrustedUtcSeconds) {
      return;
    }
    const uint64_t uptimeAfterUtcRead = monotonicMillis();
    anchorUptimeMs_ = uptimeBeforeUtcRead +
                      (uptimeAfterUtcRead - uptimeBeforeUtcRead) / 2;
    anchorUtcMs_ = static_cast<uint64_t>(currentTime.tv_sec) * 1000ULL +
                   static_cast<uint64_t>(currentTime.tv_usec) / 1000ULL;
    hasClockAnchor_ = true;
    Serial.printf("EVENT,clock,anchored,%llu\n",
                  static_cast<unsigned long long>(anchorUptimeMs_));
  }

  void scheduleBackoff(uint64_t now) {
    const uint64_t base =
        std::min<uint64_t>(1000ULL << std::min<uint8_t>(retryExponent_, 8),
                           kMaximumBackoffMs);
    const uint32_t jitterRange = static_cast<uint32_t>(base / 4 + 1);
    nextUploadMs_ = now + base + esp_random() % jitterRange;
    if (retryExponent_ < 8) {
      ++retryExponent_;
    }
  }

  void resetBackoff(uint64_t now) {
    retryExponent_ = 0;
    nextUploadMs_ = now;
  }

  bool moveToDeadLetter(const SpoolFileMetadata& metadata, int code) {
    while (deadCount_ >= kMaxDeadFiles) {
      char oldest[128] = {};
      const size_t count = countFiles(kDeadDirectory, oldest, sizeof(oldest));
      deadCount_ = count;
      if (count < kMaxDeadFiles) {
        break;
      }
      if (oldest[0] == '\0' || !LittleFS.remove(oldest)) {
        return false;
      }
      --deadCount_;
    }

    const char* filename = std::strrchr(metadata.path, '/');
    filename = filename == nullptr ? metadata.path : filename + 1;
    char deadPath[128];
    const int length = std::snprintf(deadPath, sizeof(deadPath),
                                     "/dead/e%d_%s", code, filename);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(deadPath)) {
      return false;
    }
    if (LittleFS.exists(deadPath) && !LittleFS.remove(deadPath)) {
      return false;
    }
    if (!LittleFS.rename(metadata.path, deadPath)) {
      return false;
    }
    --spoolCount_;
    ++deadCount_;
    return true;
  }

  void uploadOneEnvelope(uint64_t now) {
    SpoolFileMetadata metadata;
    if (!findNextSpool(metadata)) {
      resetBackoff(now + 1000);
      return;
    }

    File envelope = LittleFS.open(metadata.path, FILE_READ);
    if (!envelope) {
      scheduleBackoff(now);
      return;
    }

    char url[224];
    const int urlLength =
        std::snprintf(url, sizeof(url), "%s/v1/devices/%s/batches",
                      workerSettings.serverBaseUrl, workerIdentity.deviceId);
    if (urlLength <= 0 || static_cast<size_t>(urlLength) >= sizeof(url) ||
        std::strncmp(url, "http://", 7) != 0) {
      envelope.close();
      operatorHalted_ = true;
      Serial.println("EVENT,uploader,unsupported_server_url");
      return;
    }

    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(3000);
    http.setTimeout(5000);
    http.setReuse(false);
    http.useHTTP10(true);
    if (!http.begin(client, url)) {
      envelope.close();
      scheduleBackoff(now);
      return;
    }
    http.addHeader("Content-Type", "application/json");
    http.setAuthorizationType("Bearer");
    http.setAuthorization(workerSettings.apiToken);
    const size_t envelopeSize = envelope.size();
    const int status = http.sendRequest("POST", &envelope, envelopeSize);
    envelope.close();

    if (status == HTTP_CODE_OK) {
      const int responseSize = http.getSize();
      if (responseSize < 0 ||
          responseSize > static_cast<int>(kMaximumAckBytes)) {
        http.end();
        Serial.printf("EVENT,uploader,retry_bad_ack,%s,response_size\n",
                      metadata.batchId);
        scheduleBackoff(now);
        return;
      }
      const String response = http.getString();
      http.end();
      if (response.length() > kMaximumAckBytes ||
          (responseSize >= 0 &&
           response.length() != static_cast<size_t>(responseSize))) {
        Serial.printf("EVENT,uploader,retry_bad_ack,%s,truncated_or_large\n",
                      metadata.batchId);
        scheduleBackoff(now);
        return;
      }
      const IngestAckParseResult parsed = parseIngestAck(
          response.c_str(), response.length(), metadata.batchId,
          std::strlen(metadata.batchId), metadata.recordCount,
          metadata.maxSeq);
      if (!parsed.ok()) {
        Serial.printf("EVENT,uploader,retry_bad_ack,%s,%s\n",
                      metadata.batchId,
                      ingestAckParseErrorName(parsed.error));
        scheduleBackoff(now);
        return;
      }
      if (!LittleFS.remove(metadata.path)) {
        scheduleBackoff(now);
        return;
      }
      --spoolCount_;
      Serial.printf("EVENT,uploader,acked,%s,%llu,%llu\n", metadata.batchId,
                    static_cast<unsigned long long>(parsed.ack.stored),
                    static_cast<unsigned long long>(parsed.ack.duplicates));
      if (parsed.ack.desiredConfigRevision >
          workerSettings.appliedConfigRevision) {
        Serial.printf("EVENT,config,revision_available,%llu\n",
                      static_cast<unsigned long long>(
                          parsed.ack.desiredConfigRevision));
      }
      resetBackoff(now);
      return;
    }

    http.end();
    if (status == HTTP_CODE_UNAUTHORIZED || status == HTTP_CODE_NOT_FOUND) {
      // Credential and endpoint failures apply to every queued envelope. Keep
      // this exact file in the retry spool, halt the worker, and let USB
      // reprovisioning + restart retry it byte-for-byte with corrected settings.
      Serial.printf("EVENT,uploader,operator_halt,%d,%s\n", status,
                    metadata.batchId);
      operatorHalted_ = true;
      return;
    }
    if (status >= 400 && status < 500 && status != HTTP_CODE_REQUEST_TIMEOUT &&
        status != HTTP_CODE_TOO_MANY_REQUESTS) {
      if (moveToDeadLetter(metadata, status)) {
        Serial.printf("EVENT,uploader,dead_letter,%d,%s\n", status,
                      metadata.batchId);
        resetBackoff(now);
      } else {
        Serial.printf("EVENT,uploader,dead_letter_failed,%d,%s\n", status,
                      metadata.batchId);
        scheduleBackoff(now);
      }
      return;
    }

    Serial.printf("EVENT,uploader,retry,%d,%s\n", status, metadata.batchId);
    scheduleBackoff(now);
  }

  size_t spoolCount_ = 0;
  size_t deadCount_ = 0;
  uint64_t lastFreezeMs_ = 0;
  uint64_t lastWifiBeginMs_ = 0;
  uint64_t nextUploadMs_ = 0;
  uint64_t anchorUtcMs_ = 0;
  uint64_t anchorUptimeMs_ = 0;
  uint8_t retryExponent_ = 0;
  bool wifiStarted_ = false;
  bool wifiWasConnected_ = false;
  bool sntpStarted_ = false;
  bool hasClockAnchor_ = false;
  bool operatorHalted_ = false;
};

void uploaderTask(void*) {
  UploaderWorker worker;
  worker.run();
}

}  // namespace

bool startTelemetryUploader(const RuntimeIdentity& identity,
                            const TelemetryUploaderSettings& settings,
                            TelemetryQueue& queue) {
  if (!identity.deviceIdValid || workerTaskHandle != nullptr) {
    return false;
  }
  workerIdentity = identity;
  workerSettings = settings;
  workerQueue = &queue;
  const BaseType_t result = xTaskCreatePinnedToCore(
      uploaderTask, "presence_upload", 12288, nullptr, 1, &workerTaskHandle, 0);
  if (result != pdPASS) {
    workerTaskHandle = nullptr;
    workerQueue = nullptr;
    return false;
  }
  return true;
}
