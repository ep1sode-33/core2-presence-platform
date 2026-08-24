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

#include "device_config_storage.h"
#include "feedback_bundle.h"
#include "feedback_protocol.h"
#include "feedback_spool_name.h"
#include "ingest_ack.h"
#include "spool_name.h"
#include "telemetry_json.h"

namespace {

constexpr char kFirmwareVersion[] = "0.4.0";
constexpr char kSpoolDirectory[] = "/spool";
constexpr char kDeadDirectory[] = "/dead";
constexpr char kPendingPath[] = "/spool/.pending";
constexpr char kFeedbackDirectory[] = "/feedback";
constexpr char kFeedbackWaitDirectory[] = "/feedback/wait";
constexpr char kFeedbackReadyDirectory[] = "/feedback/ready";
constexpr char kFeedbackPendingPath[] = "/feedback/.pending";
constexpr size_t kMaxFirmwareBatchSize = 30;
constexpr size_t kMaxSpoolFiles = 256;
constexpr size_t kMaxFeedbackSpoolFiles = 64;
constexpr size_t kMaxDeadFiles = 16;
constexpr uint64_t kFreezeIntervalMs = 30000;
constexpr uint64_t kWifiRetryMs = 30000;
constexpr uint64_t kConfigPollIntervalMs = 5 * 60 * 1000;
constexpr uint64_t kConfigRetryMs = 30000;
constexpr uint64_t kMaximumBackoffMs = 5 * 60 * 1000;
constexpr size_t kMaximumAckBytes = 2048;
constexpr size_t kMaximumConfigResponseBytes = 2048;
constexpr time_t kMinimumTrustedUtcSeconds = 1700000000;
constexpr int kHttpConflict = 409;
constexpr int kHttpUnprocessableEntity = 422;

RuntimeIdentity workerIdentity;
TelemetryUploaderSettings workerSettings;
TelemetryQueue* workerQueue = nullptr;
DeviceConfigMailbox* workerConfigMailbox = nullptr;
TouchFeedbackQueue* workerTouchFeedbackQueue = nullptr;
TaskHandle_t workerTaskHandle = nullptr;

enum class FeedbackBundleLoadResult : uint8_t {
  kOk,
  kOpenFailed,
  kSizeOutOfRange,
  kReadFailed,
  kInvalidFrame,
};

class UploaderWorker {
 public:
  void run() {
    activeConfig_ = workerSettings.initialConfig;
    if (workerConfigMailbox != nullptr) {
      workerConfigMailbox->acknowledgeAppliedRevision(activeConfig_.revision);
    }
    while (monotonicMillis() < workerSettings.startAfterUptimeMs) {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!initializeFilesystem()) {
      Serial.println("EVENT,uploader,filesystem_unavailable");
      vTaskDelete(nullptr);
      return;
    }

    spoolCount_ = countFiles(kSpoolDirectory, nullptr, 0);
    feedbackWaitCount_ = countFiles(kFeedbackWaitDirectory, nullptr, 0);
    feedbackReadyCount_ = countFiles(kFeedbackReadyDirectory, nullptr, 0);
    deadCount_ = countFiles(kDeadDirectory, nullptr, 0);
    Serial.printf("EVENT,uploader,spool_ready,%u,%u,%u,%u\n",
                  static_cast<unsigned>(spoolCount_),
                  static_cast<unsigned>(feedbackWaitCount_),
                  static_cast<unsigned>(feedbackReadyCount_),
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
      // Main may apply a mailbox revision even while Wi-Fi is down or uploads
      // are operator-halted. Observe that acknowledgement before choosing a
      // batch size or freezing records stamped with the new revision.
      synchronizeAppliedConfig(now);
      freezeQueueIfNeeded(now);
      freezeFeedbackQueueIfNeeded();
      if (workerSettings.configured && !operatorHalted_) {
        maintainWifi(now);
        captureClockAnchor();
        if (WiFi.status() == WL_CONNECTED &&
            now >= nextConfigPollMs_ && !configAwaitingApply_) {
          pollRemoteConfig(now);
        }
        if (!operatorHalted_ && WiFi.status() == WL_CONNECTED &&
            now >= nextUploadMs_ && !conflictAwaitingConfigValidation()) {
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

  struct FixedBufferSink {
    uint8_t* bytes = nullptr;
    size_t capacity = 0;
    size_t size = 0;
  };

  static bool bufferSink(void* context, const char* data, size_t size) {
    FixedBufferSink* sink = static_cast<FixedBufferSink*>(context);
    if (sink == nullptr || sink->bytes == nullptr || data == nullptr ||
        sink->size > sink->capacity || size > sink->capacity - sink->size) {
      return false;
    }
    std::memcpy(sink->bytes + sink->size, data, size);
    sink->size += size;
    return true;
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
    if (!LittleFS.exists(kFeedbackDirectory) &&
        !LittleFS.mkdir(kFeedbackDirectory)) {
      return false;
    }
    if (!LittleFS.exists(kFeedbackWaitDirectory) &&
        !LittleFS.mkdir(kFeedbackWaitDirectory)) {
      return false;
    }
    if (!LittleFS.exists(kFeedbackReadyDirectory) &&
        !LittleFS.mkdir(kFeedbackReadyDirectory)) {
      return false;
    }
    // This name is only used while constructing a new immutable envelope.
    // A prior crash may leave it incomplete, so it is never replayed.
    if (LittleFS.exists(kPendingPath) && !LittleFS.remove(kPendingPath)) {
      return false;
    }
    if (LittleFS.exists(kFeedbackPendingPath) &&
        !LittleFS.remove(kFeedbackPendingPath)) {
      return false;
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

  bool findNextFeedbackSpool(bool ready,
                             FeedbackSpoolFileMetadata& metadata) const {
    const char* directory =
        ready ? kFeedbackReadyDirectory : kFeedbackWaitDirectory;
    File folder = LittleFS.open(directory);
    if (!folder || !folder.isDirectory()) {
      return false;
    }

    bool found = false;
    FeedbackSpoolFileMetadata best = {};
    File entry = folder.openNextFile();
    while (entry) {
      if (!entry.isDirectory()) {
        FeedbackSpoolFileMetadata candidate = {};
        const char* entryPath = entry.path();
        if (parseFeedbackSpoolFileMetadata(entryPath, candidate)) {
          const char* canonicalPath = ready ? candidate.readyPath
                                            : candidate.path;
          if (!found || std::strcmp(canonicalPath,
                                    ready ? best.readyPath : best.path) < 0) {
            best = candidate;
            found = true;
          }
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

  static FeedbackBundleLoadResult loadFeedbackBundle(
      const char* path, uint8_t* bytes, size_t capacity, size_t& bundleSize,
      FeedbackBundleSlices& slices, FeedbackBundleError& frameError) {
    if (path == nullptr || bytes == nullptr ||
        capacity < kFeedbackBundleMaxEncodedBytes) {
      return FeedbackBundleLoadResult::kSizeOutOfRange;
    }
    File bundle = LittleFS.open(path, FILE_READ);
    if (!bundle) {
      return FeedbackBundleLoadResult::kOpenFailed;
    }
    const size_t fileSize = bundle.size();
    if (fileSize < kFeedbackBundleHeaderSize ||
        fileSize > kFeedbackBundleMaxEncodedBytes) {
      bundle.close();
      return FeedbackBundleLoadResult::kSizeOutOfRange;
    }
    const size_t bytesRead = bundle.read(bytes, fileSize);
    bundle.close();
    if (bytesRead != fileSize) {
      return FeedbackBundleLoadResult::kReadFailed;
    }
    FeedbackBundleSlices candidate = {};
    frameError = validateFeedbackBundle(bytes, fileSize, candidate);
    if (frameError != FeedbackBundleError::kNone) {
      return FeedbackBundleLoadResult::kInvalidFrame;
    }
    bundleSize = fileSize;
    slices = candidate;
    return FeedbackBundleLoadResult::kOk;
  }

  void freezeQueueIfNeeded(uint64_t now) {
    if (spoolCount_ >= kMaxSpoolFiles || workerQueue == nullptr) {
      return;
    }
    const size_t available = workerQueue->size();
    if (available == 0) {
      return;
    }

    TelemetryRecord records[kMaxFirmwareBatchSize] = {};
    size_t copiedCount = workerQueue->copyPrefix(
        records, std::min(available, kMaxFirmwareBatchSize));
    if (copiedCount == 0) {
      return;
    }
    if (records[0].appliedConfigRevision > activeConfig_.revision) {
      synchronizeAppliedConfig(now);
      if (records[0].appliedConfigRevision > activeConfig_.revision) {
        return;
      }
    }
    const size_t requestedBatchSize =
        std::max<size_t>(1, std::min<size_t>(activeConfig_.uploadBatchSize,
                                            kMaxFirmwareBatchSize));
    copiedCount = std::min(copiedCount, requestedBatchSize);

    const size_t recordCount =
        contiguousRevisionPrefix(records, copiedCount);
    const bool revisionBoundary = recordCount < copiedCount;
    if (recordCount < requestedBatchSize && !revisionBoundary &&
        now - lastFreezeMs_ < kFreezeIntervalMs) {
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
    context.appliedConfigRevision = records[0].appliedConfigRevision;
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

  void freezeFeedbackQueueIfNeeded() {
    if (workerTouchFeedbackQueue == nullptr ||
        workerTouchFeedbackQueue->size() == 0) {
      return;
    }

    TouchFeedbackEvidence evidence = {};
    if (workerTouchFeedbackQueue->copyPrefix(&evidence, 1) != 1 ||
        !touchFeedbackEvidenceIsValid(evidence)) {
      Serial.println("EVENT,feedback,queue_prefix_invalid");
      return;
    }

    FeedbackSpoolFileMetadata metadata = {};
    if (!buildFeedbackSpoolFileMetadata(evidence.feedback, metadata)) {
      Serial.println("EVENT,feedback,spool_metadata_invalid");
      return;
    }

    const char* existingPath = nullptr;
    if (LittleFS.exists(metadata.path)) {
      existingPath = metadata.path;
    } else if (LittleFS.exists(metadata.readyPath)) {
      existingPath = metadata.readyPath;
    }
    if (existingPath != nullptr) {
      uint8_t existingBytes[kFeedbackBundleMaxEncodedBytes] = {};
      size_t existingSize = 0;
      FeedbackBundleSlices existingSlices = {};
      FeedbackBundleError frameError = FeedbackBundleError::kNone;
      if (loadFeedbackBundle(existingPath, existingBytes,
                             sizeof(existingBytes), existingSize,
                             existingSlices, frameError) !=
          FeedbackBundleLoadResult::kOk) {
        Serial.printf("EVENT,feedback,existing_bundle_invalid,%s\n",
                      feedbackBundleErrorName(frameError));
        return;
      }
      if (!workerTouchFeedbackQueue->commitPrefix(&evidence, 1)) {
        Serial.println("EVENT,feedback,existing_prefix_mismatch");
      }
      return;
    }

    if (feedbackWaitCount_ + feedbackReadyCount_ >=
        kMaxFeedbackSpoolFiles) {
      return;
    }

    uint8_t telemetryJson[kFeedbackBundleMaxTelemetryJsonBytes] = {};
    FixedBufferSink telemetryBuffer{telemetryJson, sizeof(telemetryJson), 0};
    TelemetryBatchContext context = {};
    context.batchId = metadata.telemetryBatchId;
    context.bootId = evidence.feedback.bootId;
    context.firmwareVersion = kFirmwareVersion;
    context.appliedConfigRevision =
        evidence.preTouchSample.appliedConfigRevision;
    context.hasClockAnchor = hasClockAnchor_;
    context.anchorUtcMs = anchorUtcMs_;
    context.anchorUptimeMs = anchorUptimeMs_;
    context.anchorSource = ClockAnchorSource::kSntp;
    const TelemetryJsonSink telemetrySink{&telemetryBuffer, bufferSink};
    if (!writeTelemetryBatchJson(context, &evidence.preTouchSample, 1,
                                 telemetrySink)) {
      Serial.println("EVENT,feedback,telemetry_serialize_failed");
      return;
    }

    uint8_t feedbackJson[kFeedbackBundleMaxFeedbackJsonBytes] = {};
    FixedBufferSink feedbackBuffer{feedbackJson, sizeof(feedbackJson), 0};
    const FeedbackJsonSink feedbackSink{&feedbackBuffer, bufferSink};
    if (!writeTouchFeedbackJson(evidence.feedback, feedbackSink)) {
      Serial.println("EVENT,feedback,payload_serialize_failed");
      return;
    }

    uint8_t bundle[kFeedbackBundleMaxEncodedBytes] = {};
    size_t bundleSize = 0;
    const FeedbackBundleError encodeError = encodeFeedbackBundle(
        telemetryJson, telemetryBuffer.size, feedbackJson, feedbackBuffer.size,
        bundle, sizeof(bundle), bundleSize);
    if (encodeError != FeedbackBundleError::kNone) {
      Serial.printf("EVENT,feedback,bundle_encode_failed,%s\n",
                    feedbackBundleErrorName(encodeError));
      return;
    }

    if (LittleFS.exists(kFeedbackPendingPath) &&
        !LittleFS.remove(kFeedbackPendingPath)) {
      Serial.println("EVENT,feedback,pending_cleanup_failed");
      return;
    }
    File pending = LittleFS.open(kFeedbackPendingPath, FILE_WRITE);
    if (!pending) {
      Serial.println("EVENT,feedback,spool_open_failed");
      return;
    }
    const size_t bytesWritten = pending.write(bundle, bundleSize);
    pending.flush();
    const size_t durableSize = pending.size();
    pending.close();
    if (bytesWritten != bundleSize || durableSize != bundleSize) {
      LittleFS.remove(kFeedbackPendingPath);
      Serial.println("EVENT,feedback,spool_write_failed");
      return;
    }
    if (!LittleFS.rename(kFeedbackPendingPath, metadata.path)) {
      LittleFS.remove(kFeedbackPendingPath);
      Serial.println("EVENT,feedback,spool_commit_failed");
      return;
    }

    ++feedbackWaitCount_;
    if (!workerTouchFeedbackQueue->commitPrefix(&evidence, 1)) {
      Serial.println("EVENT,feedback,queue_commit_failed");
      return;
    }
    Serial.printf("EVENT,feedback,spooled,%s,%s\n", metadata.feedbackId,
                  metadata.telemetryBatchId);
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

  void synchronizeAppliedConfig(uint64_t now) {
    if (!configAwaitingApply_ || workerConfigMailbox == nullptr) {
      return;
    }
    const uint64_t acknowledged =
        workerConfigMailbox->acknowledgedAppliedRevision();
    if (acknowledged < pendingConfig_.revision) {
      return;
    }
    if (acknowledged == pendingConfig_.revision) {
      activeConfig_ = pendingConfig_;
      configAwaitingApply_ = false;
      Serial.printf("EVENT,config,applied,%llu\n",
                    static_cast<unsigned long long>(activeConfig_.revision));
      return;
    }

    // This worker is the only publisher, so an acknowledgement jumping past
    // its pending candidate indicates corrupted coordination state. Keep the
    // durable config untouched and refetch instead of guessing a payload.
    configAwaitingApply_ = false;
    nextConfigPollMs_ = now;
    Serial.println("EVENT,config,ack_revision_mismatch");
  }

  void pollRemoteConfig(uint64_t now) {
    if (workerConfigMailbox == nullptr) {
      nextConfigPollMs_ = now + kConfigRetryMs;
      return;
    }

    char url[224];
    const int urlLength =
        std::snprintf(url, sizeof(url), "%s/v1/devices/%s/config",
                      workerSettings.serverBaseUrl, workerIdentity.deviceId);
    if (urlLength <= 0 || static_cast<size_t>(urlLength) >= sizeof(url) ||
        std::strncmp(url, "http://", 7) != 0) {
      operatorHalted_ = true;
      Serial.println("EVENT,config,unsupported_server_url");
      return;
    }

    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(3000);
    http.setTimeout(5000);
    http.setReuse(false);
    http.useHTTP10(true);
    if (!http.begin(client, url)) {
      nextConfigPollMs_ = now + kConfigRetryMs;
      return;
    }
    http.setAuthorizationType("Bearer");
    http.setAuthorization(workerSettings.apiToken);
    const int status = http.GET();
    if (status == HTTP_CODE_UNAUTHORIZED || status == HTTP_CODE_NOT_FOUND) {
      http.end();
      operatorHalted_ = true;
      Serial.printf("EVENT,config,operator_halt,%d\n", status);
      return;
    }
    if (status != HTTP_CODE_OK) {
      http.end();
      nextConfigPollMs_ = now + kConfigRetryMs;
      Serial.printf("EVENT,config,retry,%d\n", status);
      return;
    }

    const int responseSize = http.getSize();
    if (responseSize < 0 ||
        responseSize > static_cast<int>(kMaximumConfigResponseBytes)) {
      http.end();
      nextConfigPollMs_ = now + kConfigRetryMs;
      Serial.println("EVENT,config,retry_bad_response,response_size");
      return;
    }
    const String response = http.getString();
    http.end();
    if (response.length() != static_cast<size_t>(responseSize)) {
      nextConfigPollMs_ = now + kConfigRetryMs;
      Serial.println("EVENT,config,retry_bad_response,truncated");
      return;
    }

    const DeviceConfigParseResult parsed = parseDeviceConfigResponse(
        response.c_str(), response.length(), workerIdentity.deviceId,
        std::strlen(workerIdentity.deviceId));
    if (!parsed.ok()) {
      nextConfigPollMs_ = now + kConfigRetryMs;
      Serial.printf("EVENT,config,retry_bad_response,%s\n",
                    deviceConfigParseErrorName(parsed.error));
      return;
    }
    if (parsed.config.revision < activeConfig_.revision) {
      Serial.printf("EVENT,config,server_rollback,%llu,%llu\n",
                    static_cast<unsigned long long>(activeConfig_.revision),
                    static_cast<unsigned long long>(parsed.config.revision));
      // The device has durably applied a revision that this server no longer
      // recognizes. Continuing would turn each valid queued batch into a 409
      // and eventually evict it from the bounded diagnostic queue.
      operatorHalted_ = true;
      return;
    }
    if (parsed.config.revision == activeConfig_.revision) {
      if (!deviceConfigsEqual(parsed.config, activeConfig_)) {
        Serial.printf("EVENT,config,same_revision_conflict,%llu\n",
                      static_cast<unsigned long long>(activeConfig_.revision));
        operatorHalted_ = true;
        return;
      }
      markConflictConfigValidated();
      nextConfigPollMs_ = now + kConfigPollIntervalMs;
      return;
    }

    // A newer server snapshot also proves that the currently applied revision
    // belongs to this server's monotonic history. Complete any 409 probe before
    // handing the newer snapshot to main.
    markConflictConfigValidated();

    const DeviceConfigStorageResult stored =
        saveStoredDeviceConfig(parsed.config);
    if (stored != DeviceConfigStorageResult::kOk &&
        stored != DeviceConfigStorageResult::kUnchanged) {
      nextConfigPollMs_ = now + kConfigRetryMs;
      Serial.printf("EVENT,config,storage_failed,%u\n",
                    static_cast<unsigned>(stored));
      return;
    }

    const DeviceConfigPublishResult published =
        workerConfigMailbox->publish(parsed.config);
    if (published != DeviceConfigPublishResult::kPublished &&
        published != DeviceConfigPublishResult::kReplacedPending) {
      nextConfigPollMs_ = now + kConfigRetryMs;
      Serial.printf("EVENT,config,publish_failed,%u\n",
                    static_cast<unsigned>(published));
      return;
    }
    pendingConfig_ = parsed.config;
    configAwaitingApply_ = true;
    nextConfigPollMs_ = now + kConfigPollIntervalMs;
    Serial.printf("EVENT,config,queued,%llu\n",
                  static_cast<unsigned long long>(parsed.config.revision));
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
    if (spoolCount_ > 0) {
      --spoolCount_;
    }
    ++deadCount_;
    return true;
  }

  bool moveFeedbackToDeadLetter(const FeedbackSpoolFileMetadata& metadata,
                                bool ready, int code) {
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

    const char* sourcePath = ready ? metadata.readyPath : metadata.path;
    const char* filename = std::strrchr(sourcePath, '/');
    filename = filename == nullptr ? sourcePath : filename + 1;
    char deadPath[128] = {};
    const int length = std::snprintf(deadPath, sizeof(deadPath),
                                     "/dead/fb_%c_e%d_%s",
                                     ready ? 'r' : 'w', code, filename);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(deadPath)) {
      return false;
    }
    if (LittleFS.exists(deadPath) && !LittleFS.remove(deadPath)) {
      return false;
    }
    if (!LittleFS.rename(sourcePath, deadPath)) {
      return false;
    }
    size_t& stageCount = ready ? feedbackReadyCount_ : feedbackWaitCount_;
    if (stageCount > 0) {
      --stageCount;
    }
    ++deadCount_;
    return true;
  }

  static bool readExactAckBody(HTTPClient& http, String& response) {
    const int responseSize = http.getSize();
    if (responseSize < 0 ||
        responseSize > static_cast<int>(kMaximumAckBytes)) {
      return false;
    }
    response = http.getString();
    return response.length() == static_cast<size_t>(responseSize) &&
           response.length() <= kMaximumAckBytes;
  }

  void noteDesiredConfigRevision(uint64_t now, uint64_t desiredRevision) {
    if (desiredRevision > activeConfig_.revision) {
      Serial.printf("EVENT,config,revision_available,%llu\n",
                    static_cast<unsigned long long>(desiredRevision));
      nextConfigPollMs_ = now;
    }
  }

  bool conflictAwaitingConfigValidation() const {
    return conflictProbeActive_ && !conflictProbeConfigValidated_;
  }

  void beginTelemetryConflictProbe(uint64_t now, const char* batchId) {
    if (batchId == nullptr) {
      return;
    }
    std::snprintf(conflictProbeBatchId_, sizeof(conflictProbeBatchId_), "%s",
                  batchId);
    conflictProbeActive_ = true;
    conflictProbeConfigValidated_ = false;
    nextConfigPollMs_ = now;
    scheduleBackoff(now);
    Serial.printf("EVENT,config,conflict_probe,%s\n", batchId);
  }

  void markConflictConfigValidated() {
    if (!conflictProbeActive_ || conflictProbeConfigValidated_) {
      return;
    }
    conflictProbeConfigValidated_ = true;
    Serial.printf("EVENT,config,conflict_probe_validated,%s\n",
                  conflictProbeBatchId_);
  }

  bool telemetryConflictIsConfirmed(const char* batchId) const {
    return batchId != nullptr && conflictProbeActive_ &&
           conflictProbeConfigValidated_ &&
           std::strcmp(batchId, conflictProbeBatchId_) == 0;
  }

  void clearTelemetryConflictProbe(const char* batchId) {
    if (batchId == nullptr || !conflictProbeActive_ ||
        std::strcmp(batchId, conflictProbeBatchId_) != 0) {
      return;
    }
    conflictProbeBatchId_[0] = '\0';
    conflictProbeActive_ = false;
    conflictProbeConfigValidated_ = false;
  }

  bool uploadOneTelemetryEnvelope(uint64_t now) {
    SpoolFileMetadata metadata = {};
    if (!findNextSpool(metadata)) {
      return false;
    }

    File envelope = LittleFS.open(metadata.path, FILE_READ);
    if (!envelope) {
      scheduleBackoff(now);
      return true;
    }

    char url[224] = {};
    const int urlLength =
        std::snprintf(url, sizeof(url), "%s/v1/devices/%s/batches",
                      workerSettings.serverBaseUrl, workerIdentity.deviceId);
    if (urlLength <= 0 || static_cast<size_t>(urlLength) >= sizeof(url) ||
        std::strncmp(url, "http://", 7) != 0) {
      envelope.close();
      operatorHalted_ = true;
      Serial.println("EVENT,uploader,unsupported_server_url");
      return true;
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
      return true;
    }
    http.addHeader("Content-Type", "application/json");
    http.setAuthorizationType("Bearer");
    http.setAuthorization(workerSettings.apiToken);
    const size_t envelopeSize = envelope.size();
    const int status = http.sendRequest("POST", &envelope, envelopeSize);
    envelope.close();

    if (status == HTTP_CODE_OK) {
      String response;
      if (!readExactAckBody(http, response)) {
        http.end();
        Serial.printf("EVENT,uploader,retry_bad_ack,%s,bounded_read\n",
                      metadata.batchId);
        scheduleBackoff(now);
        return true;
      }
      http.end();
      const IngestAckParseResult parsed = parseIngestAck(
          response.c_str(), response.length(), metadata.batchId,
          std::strlen(metadata.batchId), metadata.recordCount,
          metadata.maxSeq);
      if (!parsed.ok()) {
        Serial.printf("EVENT,uploader,retry_bad_ack,%s,%s\n",
                      metadata.batchId,
                      ingestAckParseErrorName(parsed.error));
        scheduleBackoff(now);
        return true;
      }
      if (!LittleFS.remove(metadata.path)) {
        scheduleBackoff(now);
        return true;
      }
      if (spoolCount_ > 0) {
        --spoolCount_;
      }
      clearTelemetryConflictProbe(metadata.batchId);
      Serial.printf("EVENT,uploader,acked,%s,%llu,%llu\n", metadata.batchId,
                    static_cast<unsigned long long>(parsed.ack.stored),
                    static_cast<unsigned long long>(parsed.ack.duplicates));
      noteDesiredConfigRevision(now, parsed.ack.desiredConfigRevision);
      resetBackoff(now);
      return true;
    }

    http.end();
    if (status == HTTP_CODE_UNAUTHORIZED || status == HTTP_CODE_NOT_FOUND) {
      Serial.printf("EVENT,uploader,operator_halt,%d,%s\n", status,
                    metadata.batchId);
      operatorHalted_ = true;
      return true;
    }
    if (status == kHttpConflict) {
      if (!telemetryConflictIsConfirmed(metadata.batchId)) {
        beginTelemetryConflictProbe(now, metadata.batchId);
        return true;
      }
      if (moveToDeadLetter(metadata, status)) {
        clearTelemetryConflictProbe(metadata.batchId);
        Serial.printf("EVENT,uploader,dead_letter,%d,%s\n", status,
                      metadata.batchId);
        resetBackoff(now);
      } else {
        Serial.printf("EVENT,uploader,dead_letter_failed,%d,%s\n", status,
                      metadata.batchId);
        scheduleBackoff(now);
      }
      return true;
    }
    if (status >= 400 && status < 500 && status != HTTP_CODE_REQUEST_TIMEOUT &&
        status != HTTP_CODE_TOO_MANY_REQUESTS) {
      if (moveToDeadLetter(metadata, status)) {
        clearTelemetryConflictProbe(metadata.batchId);
        Serial.printf("EVENT,uploader,dead_letter,%d,%s\n", status,
                      metadata.batchId);
        resetBackoff(now);
      } else {
        Serial.printf("EVENT,uploader,dead_letter_failed,%d,%s\n", status,
                      metadata.batchId);
        scheduleBackoff(now);
      }
      return true;
    }
    Serial.printf("EVENT,uploader,retry,%d,%s\n", status, metadata.batchId);
    scheduleBackoff(now);
    return true;
  }

  bool uploadOneFeedbackWait(uint64_t now) {
    FeedbackSpoolFileMetadata metadata = {};
    if (!findNextFeedbackSpool(false, metadata)) {
      return false;
    }

    uint8_t bundle[kFeedbackBundleMaxEncodedBytes] = {};
    size_t bundleSize = 0;
    FeedbackBundleSlices slices = {};
    FeedbackBundleError frameError = FeedbackBundleError::kNone;
    const FeedbackBundleLoadResult loaded = loadFeedbackBundle(
        metadata.path, bundle, sizeof(bundle), bundleSize, slices, frameError);
    if (loaded != FeedbackBundleLoadResult::kOk) {
      Serial.printf("EVENT,feedback,wait_bundle_invalid,%u,%s\n",
                    static_cast<unsigned>(loaded),
                    feedbackBundleErrorName(frameError));
      if ((loaded == FeedbackBundleLoadResult::kInvalidFrame ||
           loaded == FeedbackBundleLoadResult::kSizeOutOfRange) &&
          moveFeedbackToDeadLetter(metadata, false, 0)) {
        resetBackoff(now);
      } else {
        scheduleBackoff(now);
      }
      return true;
    }

    char url[224] = {};
    const int urlLength =
        std::snprintf(url, sizeof(url), "%s/v1/devices/%s/batches",
                      workerSettings.serverBaseUrl, workerIdentity.deviceId);
    if (urlLength <= 0 || static_cast<size_t>(urlLength) >= sizeof(url) ||
        std::strncmp(url, "http://", 7) != 0) {
      operatorHalted_ = true;
      Serial.println("EVENT,feedback,unsupported_server_url");
      return true;
    }

    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(3000);
    http.setTimeout(5000);
    http.setReuse(false);
    http.useHTTP10(true);
    if (!http.begin(client, url)) {
      scheduleBackoff(now);
      return true;
    }
    http.addHeader("Content-Type", "application/json");
    http.setAuthorizationType("Bearer");
    http.setAuthorization(workerSettings.apiToken);
    const int status = http.sendRequest(
        "POST", bundle + slices.telemetryOffset, slices.telemetryLength);

    if (status == HTTP_CODE_OK) {
      String response;
      if (!readExactAckBody(http, response)) {
        http.end();
        Serial.printf("EVENT,feedback,wait_retry_bad_ack,%s,bounded_read\n",
                      metadata.telemetryBatchId);
        scheduleBackoff(now);
        return true;
      }
      http.end();
      const IngestAckParseResult parsed = parseIngestAck(
          response.c_str(), response.length(), metadata.telemetryBatchId,
          std::strlen(metadata.telemetryBatchId), 1, metadata.seq);
      if (!parsed.ok()) {
        Serial.printf("EVENT,feedback,wait_retry_bad_ack,%s,%s\n",
                      metadata.telemetryBatchId,
                      ingestAckParseErrorName(parsed.error));
        scheduleBackoff(now);
        return true;
      }
      if (LittleFS.exists(metadata.readyPath) ||
          !LittleFS.rename(metadata.path, metadata.readyPath)) {
        Serial.printf("EVENT,feedback,ready_rename_failed,%s\n",
                      metadata.feedbackId);
        scheduleBackoff(now);
        return true;
      }
      if (feedbackWaitCount_ > 0) {
        --feedbackWaitCount_;
      }
      ++feedbackReadyCount_;
      clearTelemetryConflictProbe(metadata.telemetryBatchId);
      noteDesiredConfigRevision(now, parsed.ack.desiredConfigRevision);
      Serial.printf("EVENT,feedback,telemetry_acked,%s,%s\n",
                    metadata.feedbackId, metadata.telemetryBatchId);
      resetBackoff(now);
      return true;
    }

    http.end();
    if (status == HTTP_CODE_UNAUTHORIZED || status == HTTP_CODE_NOT_FOUND) {
      Serial.printf("EVENT,feedback,wait_operator_halt,%d,%s\n", status,
                    metadata.telemetryBatchId);
      operatorHalted_ = true;
      return true;
    }
    if (status == kHttpConflict) {
      if (!telemetryConflictIsConfirmed(metadata.telemetryBatchId)) {
        beginTelemetryConflictProbe(now, metadata.telemetryBatchId);
        return true;
      }
      if (moveFeedbackToDeadLetter(metadata, false, status)) {
        clearTelemetryConflictProbe(metadata.telemetryBatchId);
        Serial.printf("EVENT,feedback,wait_dead_letter,%d,%s\n", status,
                      metadata.telemetryBatchId);
        resetBackoff(now);
      } else {
        scheduleBackoff(now);
      }
      return true;
    }
    if (status >= 400 && status < 500 && status != HTTP_CODE_REQUEST_TIMEOUT &&
        status != HTTP_CODE_TOO_MANY_REQUESTS) {
      if (moveFeedbackToDeadLetter(metadata, false, status)) {
        clearTelemetryConflictProbe(metadata.telemetryBatchId);
        Serial.printf("EVENT,feedback,wait_dead_letter,%d,%s\n", status,
                      metadata.telemetryBatchId);
        resetBackoff(now);
      } else {
        scheduleBackoff(now);
      }
      return true;
    }
    Serial.printf("EVENT,feedback,wait_retry,%d,%s\n", status,
                  metadata.telemetryBatchId);
    scheduleBackoff(now);
    return true;
  }

  bool uploadOneFeedbackReady(uint64_t now) {
    FeedbackSpoolFileMetadata metadata = {};
    if (!findNextFeedbackSpool(true, metadata)) {
      return false;
    }

    FeedbackRecord expectedRecord = {};
    if (!feedbackRecordFromSpoolFileMetadata(metadata, expectedRecord)) {
      if (moveFeedbackToDeadLetter(metadata, true, 0)) {
        resetBackoff(now);
      } else {
        scheduleBackoff(now);
      }
      return true;
    }

    uint8_t bundle[kFeedbackBundleMaxEncodedBytes] = {};
    size_t bundleSize = 0;
    FeedbackBundleSlices slices = {};
    FeedbackBundleError frameError = FeedbackBundleError::kNone;
    const FeedbackBundleLoadResult loaded = loadFeedbackBundle(
        metadata.readyPath, bundle, sizeof(bundle), bundleSize, slices,
        frameError);
    if (loaded != FeedbackBundleLoadResult::kOk) {
      Serial.printf("EVENT,feedback,ready_bundle_invalid,%u,%s\n",
                    static_cast<unsigned>(loaded),
                    feedbackBundleErrorName(frameError));
      if ((loaded == FeedbackBundleLoadResult::kInvalidFrame ||
           loaded == FeedbackBundleLoadResult::kSizeOutOfRange) &&
          moveFeedbackToDeadLetter(metadata, true, 0)) {
        resetBackoff(now);
      } else {
        scheduleBackoff(now);
      }
      return true;
    }

    char url[224] = {};
    const int urlLength =
        std::snprintf(url, sizeof(url), "%s/v1/devices/%s/feedback",
                      workerSettings.serverBaseUrl, workerIdentity.deviceId);
    if (urlLength <= 0 || static_cast<size_t>(urlLength) >= sizeof(url) ||
        std::strncmp(url, "http://", 7) != 0) {
      operatorHalted_ = true;
      Serial.println("EVENT,feedback,unsupported_server_url");
      return true;
    }

    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(3000);
    http.setTimeout(5000);
    http.setReuse(false);
    http.useHTTP10(true);
    if (!http.begin(client, url)) {
      scheduleBackoff(now);
      return true;
    }
    http.addHeader("Content-Type", "application/json");
    http.setAuthorizationType("Bearer");
    http.setAuthorization(workerSettings.apiToken);
    const int status = http.sendRequest(
        "POST", bundle + slices.feedbackOffset, slices.feedbackLength);

    if (status == HTTP_CODE_OK) {
      String response;
      if (!readExactAckBody(http, response)) {
        http.end();
        Serial.printf("EVENT,feedback,ready_retry_bad_ack,%s,bounded_read\n",
                      metadata.feedbackId);
        scheduleBackoff(now);
        return true;
      }
      http.end();
      const FeedbackAckParseResult parsed = parseFeedbackAck(
          response.c_str(), response.length(), workerIdentity.deviceId,
          expectedRecord);
      if (!parsed.ok()) {
        Serial.printf("EVENT,feedback,ready_retry_bad_ack,%s,%s\n",
                      metadata.feedbackId,
                      feedbackAckParseErrorName(parsed.error));
        scheduleBackoff(now);
        return true;
      }
      if (!LittleFS.remove(metadata.readyPath)) {
        scheduleBackoff(now);
        return true;
      }
      if (feedbackReadyCount_ > 0) {
        --feedbackReadyCount_;
      }
      Serial.printf("EVENT,feedback,acked,%s,%u\n", metadata.feedbackId,
                    parsed.ack.duplicate ? 1U : 0U);
      resetBackoff(now);
      return true;
    }

    http.end();
    if (status == HTTP_CODE_UNAUTHORIZED) {
      Serial.printf("EVENT,feedback,ready_operator_halt,%d,%s\n", status,
                    metadata.feedbackId);
      operatorHalted_ = true;
      return true;
    }
    if (status == HTTP_CODE_NOT_FOUND || status == kHttpConflict ||
        status == kHttpUnprocessableEntity) {
      if (moveFeedbackToDeadLetter(metadata, true, status)) {
        Serial.printf("EVENT,feedback,ready_dead_letter,%d,%s\n", status,
                      metadata.feedbackId);
        resetBackoff(now);
      } else {
        scheduleBackoff(now);
      }
      return true;
    }
    Serial.printf("EVENT,feedback,ready_retry,%d,%s\n", status,
                  metadata.feedbackId);
    scheduleBackoff(now);
    return true;
  }

  void uploadOneEnvelope(uint64_t now) {
    // Complete already-acknowledged corrections first, then advance new
    // corrections to ready, and only then drain ordinary telemetry.
    if (uploadOneFeedbackReady(now)) {
      return;
    }
    if (uploadOneFeedbackWait(now)) {
      return;
    }
    if (uploadOneTelemetryEnvelope(now)) {
      return;
    }
    resetBackoff(now + 1000);
  }

  size_t spoolCount_ = 0;
  size_t feedbackWaitCount_ = 0;
  size_t feedbackReadyCount_ = 0;
  size_t deadCount_ = 0;
  uint64_t lastFreezeMs_ = 0;
  uint64_t lastWifiBeginMs_ = 0;
  uint64_t nextUploadMs_ = 0;
  uint64_t nextConfigPollMs_ = 0;
  uint64_t anchorUtcMs_ = 0;
  uint64_t anchorUptimeMs_ = 0;
  PresenceConfig activeConfig_ = defaultPresenceConfig();
  PresenceConfig pendingConfig_ = defaultPresenceConfig();
  char conflictProbeBatchId_[96] = {};
  uint8_t retryExponent_ = 0;
  bool wifiStarted_ = false;
  bool wifiWasConnected_ = false;
  bool sntpStarted_ = false;
  bool hasClockAnchor_ = false;
  bool configAwaitingApply_ = false;
  bool conflictProbeActive_ = false;
  bool conflictProbeConfigValidated_ = false;
  bool operatorHalted_ = false;
};

void uploaderTask(void*) {
  UploaderWorker worker;
  worker.run();
}

}  // namespace

bool startTelemetryUploader(const RuntimeIdentity& identity,
                            const TelemetryUploaderSettings& settings,
                            TelemetryQueue& queue,
                            DeviceConfigMailbox& configMailbox,
                            TouchFeedbackQueue& touchFeedbackQueue) {
  if (!identity.deviceIdValid || workerTaskHandle != nullptr) {
    return false;
  }
  workerIdentity = identity;
  workerSettings = settings;
  workerQueue = &queue;
  workerConfigMailbox = &configMailbox;
  workerTouchFeedbackQueue = &touchFeedbackQueue;
  const BaseType_t result = xTaskCreatePinnedToCore(
      uploaderTask, "presence_upload", 12288, nullptr, 1, &workerTaskHandle, 0);
  if (result != pdPASS) {
    workerTaskHandle = nullptr;
    workerQueue = nullptr;
    workerConfigMailbox = nullptr;
    workerTouchFeedbackQueue = nullptr;
    return false;
  }
  return true;
}
