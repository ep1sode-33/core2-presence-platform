#include "telemetry_uploader.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <esp_system.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <sys/time.h>

#include "backlog_policy.h"
#include "backend_transport_recovery.h"
#include "command_ack_protocol.h"
#include "command_journal.h"
#include "control_protocol.h"
#include "control_retry_policy.h"
#include "core_dump_ack.h"
#include "core_dump_json.h"
#include "core_dump_upload.h"
#include "dashboard_time.h"
#include "device_config_storage.h"
#include "feedback_bundle.h"
#include "feedback_protocol.h"
#include "feedback_spool_name.h"
#include "firmware_info.h"
#include "health_json.h"
#include "ingest_ack.h"
#include "operational_log_ack.h"
#include "operational_log_json.h"
#include "ota_control_validation.h"
#include "ota_boot_policy.h"
#include "ota_dev_window.h"
#include "ota_install_state.h"
#include "ota_release.h"
#include "ota_release_status.h"
#include "ota_release_status_ack.h"
#include "ota_trust_store.h"
#include "ota_update.h"
#include "spool_name.h"
#include "telemetry_json.h"
#include "uploader_watchdog.h"

namespace {

constexpr char kSpoolDirectory[] = "/spool";
constexpr char kDeadDirectory[] = "/dead";
constexpr char kPendingPath[] = "/spool/.pending";
constexpr char kFeedbackDirectory[] = "/feedback";
constexpr char kFeedbackWaitDirectory[] = "/feedback/wait";
constexpr char kFeedbackReadyDirectory[] = "/feedback/ready";
constexpr char kFeedbackPendingPath[] = "/feedback/.pending";
constexpr size_t kMaxFirmwareBatchSize = 30;
constexpr size_t kMaxFeedbackSpoolFiles = 64;
constexpr size_t kMaxDeadFiles = 16;
constexpr uint64_t kFreezeIntervalMs = 30000;
constexpr uint64_t kWifiRetryMs = 30000;
constexpr uint64_t kConfigPollIntervalMs = 5 * 60 * 1000;
constexpr uint64_t kConfigRetryMs = 30000;
constexpr uint64_t kMaximumBackoffMs = 5 * 60 * 1000;
constexpr uint64_t kFilesystemRetryMs = 30 * 1000;
constexpr uint64_t kHealthIntervalMs = 60 * 1000;
constexpr uint64_t kHealthRetryMs = 30 * 1000;
constexpr uint64_t kOperationalLogIntervalMs = 5 * 1000;
constexpr uint64_t kOperationalLogRetryMaximumMs = 5 * 60 * 1000;
constexpr uint64_t kCommandAckRetryMaximumMs = 5 * 60 * 1000;
constexpr uint64_t kCoreDumpRetryMaximumMs = 5 * 60 * 1000;
constexpr uint64_t kOtaReleaseRetryMs = 60 * 1000;
constexpr uint64_t kOtaImageInactivityTimeoutMs = 10 * 1000;
constexpr uint64_t kSpoolAuditIntervalMs = 60 * 1000;
constexpr uint64_t kMinimumEnvironmentPollIntervalMs = 30 * 1000;
constexpr uint64_t kMinimumWeatherPollIntervalMs = 15 * 60 * 1000;
constexpr uint64_t kEnvironmentRetryMs = 30 * 1000;
constexpr uint64_t kWeatherRetryMs = 60 * 1000;
constexpr uint64_t kMaximumDashboardDeferralMs = 30 * 1000;
constexpr size_t kMaximumAckBytes = 2048;
constexpr size_t kMaximumConfigResponseBytes = 2048;
constexpr size_t kMaximumEnvironmentResponseBytes = 1024;
constexpr size_t kMaximumWeatherResponseBytes = 4096;
constexpr size_t kMaximumControlResponseBytes = 8192;
constexpr size_t kMaximumOperationalLogPayloadBytes = 8192;
constexpr size_t kMaximumCommandAckPayloadBytes = 1024;
constexpr size_t kMaximumReleaseStatusPayloadBytes = 2048;
constexpr size_t kMaximumManifestBundleBytes =
    kOtaManifestMaximumSize + kOtaP256SignatureSize;
constexpr size_t kOperationalLogBatchSize = 24;
constexpr time_t kMinimumTrustedUtcSeconds = 1700000000;
constexpr int kHttpConflict = 409;
constexpr int kHttpUnprocessableEntity = 422;

RuntimeIdentity workerIdentity;
TelemetryUploaderSettings workerSettings;
TelemetryQueue* workerQueue = nullptr;
DeviceConfigMailbox* workerConfigMailbox = nullptr;
TouchFeedbackQueue* workerTouchFeedbackQueue = nullptr;
DashboardMailbox* workerDashboardMailbox = nullptr;
DeviceHealthMailbox* workerHealthMailbox = nullptr;
ControlMailbox* workerControlMailbox = nullptr;
OperationalLogRing* workerOperationalLog = nullptr;
OtaRuntimeMailbox* workerOtaRuntimeMailbox = nullptr;
TaskHandle_t workerTaskHandle = nullptr;

// Both dashboard sources are fetched serially by the sole uploader worker, so
// they can safely share one fixed response buffer without consuming task stack
// or fragmenting the heap. The extra byte is reserved for the parser's NUL.
char dashboardResponseBytes[kMaximumWeatherResponseBytes + 1] = {};
uint8_t healthPayloadBytes[4096] = {};
uint8_t controlResponseBytes[kMaximumControlResponseBytes + 1] = {};
uint8_t commandAckPayloadBytes[kMaximumCommandAckPayloadBytes] = {};
uint8_t operationalLogPayloadBytes[kMaximumOperationalLogPayloadBytes] = {};
uint8_t releaseStatusPayloadBytes[kMaximumReleaseStatusPayloadBytes] = {};
uint8_t manifestBundleBytes[kMaximumManifestBundleBytes] = {};
uint8_t otaImageChunk[kOtaMaximumWriteChunkSize] = {};

enum class FeedbackBundleLoadResult : uint8_t {
  kOk,
  kOpenFailed,
  kSizeOutOfRange,
  kReadFailed,
  kInvalidFrame,
};

enum class DashboardFetchResult : uint8_t {
  kOk,
  kInvalidUrl,
  kBeginFailed,
  kHttpStatus,
  kTooLarge,
  kTransferFailed,
  kTruncated,
};

struct DashboardHttpResponse {
  DashboardFetchResult result = DashboardFetchResult::kTransferFailed;
  int httpStatus = 0;
  int transferResult = 0;
  size_t bodySize = 0;
};

enum class CommandStep : uint8_t {
  kIdle = 0,
  kAckAccepted,
  kAckRunning,
  kWaitingMain,
  kAckFinal,
};

enum class ReleaseReportPhase : uint8_t {
  kDownloading,
  kVerifying,
  kRebootPending,
  kValidating,
  kRunning,
  kFailed,
  kRejected,
  kRolledBack,
};

class FixedCapacityStream final : public Stream {
 public:
  FixedCapacityStream(uint8_t* bytes, size_t capacity)
      : bytes_(bytes), capacity_(capacity) {}

  using Print::write;

  size_t write(uint8_t value) override {
    return write(&value, 1);
  }

  size_t write(const uint8_t* bytes, size_t size) override {
    if (bytes_ == nullptr || bytes == nullptr || size > capacity_ - size_) {
      overflowed_ = true;
      return 0;
    }
    std::memcpy(bytes_ + size_, bytes, size);
    size_ += size;
    return size;
  }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  int availableForWrite() override {
    const size_t remaining = capacity_ - size_;
    return remaining > static_cast<size_t>(INT_MAX)
               ? INT_MAX
               : static_cast<int>(remaining);
  }

  size_t size() const { return size_; }
  bool overflowed() const { return overflowed_; }

 private:
  uint8_t* bytes_ = nullptr;
  size_t capacity_ = 0;
  size_t size_ = 0;
  bool overflowed_ = false;
};

// HTTPClient pulls a streamed request body in fixed chunks. Feed between those
// chunks so a slow but progressing core-dump upload is not mistaken for a hung
// worker; a blocked socket write still remains bounded by the 30-second TWDT.
class WatchdogFeedingStream final : public Stream {
 public:
  explicit WatchdogFeedingStream(Stream& source) : source_(source) {}

  int available() override {
    feedUploaderTaskWatchdog();
    return source_.available();
  }
  int read() override {
    feedUploaderTaskWatchdog();
    const int value = source_.read();
    feedUploaderTaskWatchdog();
    return value;
  }
  int peek() override {
    feedUploaderTaskWatchdog();
    return source_.peek();
  }
  size_t readBytes(uint8_t* buffer, size_t length) override {
    feedUploaderTaskWatchdog();
    const size_t read = source_.readBytes(buffer, length);
    feedUploaderTaskWatchdog();
    return read;
  }
  size_t write(uint8_t) override { return 0; }

 private:
  Stream& source_;
};

// HTTPClient keeps its input Stream alive while it writes each body chunk. A
// directly-open LittleFS File made the ESP32 socket return EAGAIN before the
// first telemetry body byte. Reopen only for each bounded flash read, then
// close before HTTPClient writes that RAM chunk to the network.
class ReopeningLittleFsReadStream final : public Stream {
 public:
  ReopeningLittleFsReadStream(const char* path, size_t size)
      : path_(path), size_(size) {}

  int available() override {
    if (failed_) {
      return -1;
    }
    const size_t remaining = size_ - offset_;
    return remaining > static_cast<size_t>(INT_MAX)
               ? INT_MAX
               : static_cast<int>(remaining);
  }

  int read() override {
    uint8_t value = 0;
    return readBytes(&value, 1) == 1 ? value : -1;
  }

  int peek() override { return -1; }

  size_t readBytes(uint8_t* buffer, size_t length) override {
    feedUploaderTaskWatchdog();
    if (length == 0) {
      return 0;
    }
    if (failed_ || path_ == nullptr || buffer == nullptr || offset_ > size_) {
      failed_ = true;
      return 0;
    }
    const size_t requested = std::min(length, size_ - offset_);
    File file = LittleFS.open(path_, FILE_READ);
    if (!file || file.size() != size_ || !file.seek(offset_)) {
      file.close();
      failed_ = true;
      return 0;
    }
    const size_t read = file.read(buffer, requested);
    file.close();
    if (read != requested) {
      failed_ = true;
      return read;
    }
    offset_ += read;
    feedUploaderTaskWatchdog();
    delay(0);
    return read;
  }

  size_t write(uint8_t) override { return 0; }

  bool complete() const { return !failed_ && offset_ == size_; }

 private:
  const char* path_ = nullptr;
  size_t size_ = 0;
  size_t offset_ = 0;
  bool failed_ = false;
};

class UploaderWorker {
 public:
  void run() {
    const UploaderWatchdogStartResult watchdogResult =
        startUploaderTaskWatchdog();
    watchdogActive_ =
        watchdogResult == UploaderWatchdogStartResult::kStarted ||
        watchdogResult == UploaderWatchdogStartResult::kAlreadySubscribed;
    Serial.printf("EVENT,uploader,watchdog,%s,%us\n",
                  uploaderWatchdogStartResultName(watchdogResult),
                  kUploaderWatchdogPolicy.timeoutSeconds);
    activeConfig_ = workerSettings.initialConfig;
    desiredConfigRevision_ = activeConfig_.revision;
    if (workerConfigMailbox != nullptr) {
      workerConfigMailbox->acknowledgeAppliedRevision(activeConfig_.revision);
    }
    while (monotonicMillis() < workerSettings.startAfterUptimeMs) {
      feedWatchdog();
      vTaskDelay(pdMS_TO_TICKS(100));
    }
    initializeCommandJournal();
    feedWatchdog();
    initializeOtaRuntime(monotonicMillis());
    feedWatchdog();
    initializeCoreDump();
    feedWatchdog();
    filesystemReady_ = initializeFilesystem();
    feedWatchdog();
    if (filesystemReady_) {
      loadFilesystemCounts();
    } else {
      nextFilesystemRetryMs_ = monotonicMillis() + kFilesystemRetryMs;
      Serial.println("EVENT,uploader,filesystem_unavailable,retrying");
    }

    const bool networkRequested =
        workerSettings.configured || workerSettings.environmentEnabled ||
        workerSettings.weatherEnabled;
    wifiAvailable_ =
        networkRequested && workerSettings.wifiSsid[0] != '\0' &&
        std::memchr(workerSettings.wifiSsid, '\0',
                    sizeof(workerSettings.wifiSsid)) != nullptr;
    if (wifiAvailable_) {
      WiFi.persistent(false);
      WiFi.mode(WIFI_STA);
      WiFi.setAutoReconnect(true);
    } else {
      Serial.println("EVENT,uploader,not_provisioned");
    }

    while (true) {
      feedWatchdog();
      const uint64_t now = monotonicMillis();
      serviceOtaRuntime(now);
      feedWatchdog();
      serviceMainControlResult(now);
      expireDetailedLogSession(now);
      if (!filesystemReady_ && now >= nextFilesystemRetryMs_) {
        filesystemReady_ = initializeFilesystem();
        if (filesystemReady_) {
          loadFilesystemCounts();
          Serial.println("EVENT,uploader,filesystem_recovered");
        } else {
          nextFilesystemRetryMs_ = now + kFilesystemRetryMs;
          Serial.println("EVENT,uploader,filesystem_retry_failed");
        }
      }
      // Main may apply a mailbox revision even while Wi-Fi is down or uploads
      // are operator-halted. Observe that acknowledgement before choosing a
      // batch size or freezing records stamped with the new revision.
      synchronizeAppliedConfig(now);
      if (filesystemReady_) {
        if (now >= nextSpoolAuditMs_) {
          const bool quarantined = quarantineOneInvalidTelemetrySpool();
          nextSpoolAuditMs_ =
              now + (quarantined ? 1000 : kSpoolAuditIntervalMs);
        }
        freezeQueueIfNeeded(now);
        freezeFeedbackQueueIfNeeded();
        feedWatchdog();
      }
      if (wifiAvailable_) {
        maintainWifi(now);
        feedWatchdog();
        captureClockAnchor();
        if (WiFi.status() == WL_CONNECTED) {
          serviceDevelopmentOta(now);
          feedWatchdog();
          publishWorkerHealth(now);
          uploadHealthIfNeeded(now);
          feedWatchdog();
          serviceCommandAcknowledgement(now);
          feedWatchdog();
          if (workerSettings.configured && !controlOperatorHalted_ &&
              now >= nextControlPollMs_ && !productionOtaActive_) {
            pollRemoteControl(now);
            feedWatchdog();
          }
          uploadCoreDumpIfNeeded(now);
          feedWatchdog();
          uploadOperationalLogsIfNeeded(now);
          feedWatchdog();
          bool presenceRequestAttempted = false;
          const bool dashboardMustPreempt =
              dashboardSourceMustPreempt(now);
          if (workerSettings.configured && !operatorHalted_) {
            if (now >= nextConfigPollMs_ && !configAwaitingApply_) {
              pollRemoteConfig(now);
              feedWatchdog();
            }
            if (filesystemReady_ && !dashboardMustPreempt &&
                !operatorHalted_ &&
                now >= nextUploadMs_ &&
                !conflictAwaitingConfigValidation()) {
              presenceRequestAttempted = uploadOneEnvelope(now);
              feedWatchdog();
            }
          }
          // Presence traffic normally wins. Each dashboard source still gets
          // one prompt first attempt, and once due can be deferred by backlog
          // for at most 30 seconds before it preempts one upload iteration.
          if (dashboardMustPreempt || !presenceRequestAttempted) {
            fetchOneDashboardSource(now);
            feedWatchdog();
          }
        }
      }
      publishRuntimeActivity(now);
      publishWorkerHealth(now);
      feedWatchdog();
      vTaskDelay(pdMS_TO_TICKS(250));
    }
  }

 private:
  void feedWatchdog() {
    if (!watchdogActive_) {
      return;
    }
    if (!feedUploaderTaskWatchdog()) {
      watchdogActive_ = false;
      Serial.println("EVENT,uploader,watchdog_feed_failed");
    }
  }

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

  static uint64_t boundedRetryDelay(uint8_t exponent, uint64_t maximumMs,
                                    uint32_t entropy) {
    const uint64_t base = std::min<uint64_t>(
        1000ULL << std::min<uint8_t>(exponent, 8), maximumMs);
    const uint64_t jitterRange = base / 4U + 1U;
    return std::min<uint64_t>(base + entropy % jitterRange, maximumMs);
  }

  bool beginBackendRequest(const char* url, uint16_t timeoutMs) {
    // The control endpoint is polled every five seconds. Opening a fresh
    // HTTP/1.0 connection for every control/log/health operation can fill the
    // ESP32's small TCP PCB pool with TIME_WAIT sockets. Keep one serialized
    // HTTP/1.1 connection for bounded authenticated backend operations.
    backendHttp_.setConnectTimeout(3000);
    backendHttp_.setTimeout(timeoutMs);
    backendHttp_.useHTTP10(false);
    // A failed/partial response deliberately disables reuse before closing.
    // Restore the normal mode for the next clean request attempt.
    backendHttp_.setReuse(true);
    if (backendHttp_.begin(backendClient_, url)) {
      return true;
    }
    backendHttp_.setReuse(false);
    backendHttp_.end();
    backendClient_.stop();
    return false;
  }

  bool beginTelemetryRequest(const char* url, uint16_t timeoutMs) {
    // Streamed LittleFS bodies use their own persistent connection so a send
    // stall cannot contaminate the bounded control/health/log transport. This
    // session is serialized by the same worker and reuses only its own socket.
    telemetryHttp_.setConnectTimeout(3000);
    telemetryHttp_.setTimeout(timeoutMs);
    telemetryHttp_.useHTTP10(false);
    // useHTTP10(false) currently enables reuse as a side effect; keep this
    // explicit and ordered afterwards so the intended lifecycle is stable.
    telemetryHttp_.setReuse(true);
    if (telemetryHttp_.begin(telemetryClient_, url)) {
      return true;
    }
    telemetryHttp_.setReuse(false);
    telemetryHttp_.end();
    telemetryClient_.stop();
    return false;
  }

  void recycleBackendTransport(int status, uint8_t failureCount) {
    // A confirmed path-level failure invalidates both independent sessions.
    // The ordinary telemetry retry path closes only telemetryClient_ and does
    // not call this global Wi-Fi recovery routine.
    telemetryHttp_.setReuse(false);
    telemetryHttp_.end();
    telemetryClient_.stop();
    backendHttp_.setReuse(false);
    backendHttp_.end();
    backendClient_.stop();
    const bool reconnectStarted = WiFi.reconnect();
    if (!reconnectStarted) {
      WiFi.disconnect(false, false);
      wifiStarted_ = false;
      lastWifiBeginMs_ = 0;
    }
    wifiWasConnected_ = false;
    if (wifiReconnectCount_ != UINT32_MAX) {
      ++wifiReconnectCount_;
    }
    Serial.printf("EVENT,wifi,transport_recycle,%d,%u\n", status,
                  static_cast<unsigned>(failureCount));
  }

  void endBackendRequest(bool responseFullyConsumed,
                         int status = HTTP_CODE_OK) {
    if (!responseFullyConsumed) {
      // An unread or partial response must never contaminate the next request
      // on the persistent connection.
      backendHttp_.setReuse(false);
    }
    backendHttp_.end();
    if (!responseFullyConsumed) {
      backendClient_.stop();
    }
    // Receiving any HTTP status proves that the TCP path is alive, even when
    // the application rejects the request. Only transport-level failures feed
    // the Wi-Fi circuit breaker.
    if (responseFullyConsumed || status >= HTTP_CODE_CONTINUE) {
      backendTransportRecovery_.recordSuccess();
      return;
    }
    const BackendTransportRecoveryDecision decision =
        backendTransportRecovery_.recordFailure(monotonicMillis(), status);
    if (decision.shouldRecycle) {
      recycleBackendTransport(status, decision.failureCount);
    }
  }

  void endTelemetryRequest(bool responseFullyConsumed,
                           int status = HTTP_CODE_OK) {
    if (!responseFullyConsumed) {
      // Retry the immutable envelope on a clean telemetry socket without
      // disturbing the separate bounded-operation connection.
      telemetryHttp_.setReuse(false);
    }
    telemetryHttp_.end();
    if (!responseFullyConsumed) {
      telemetryClient_.stop();
    }
    // Any complete response, or even a received HTTP status whose body we
    // reject, proves that the backend TCP path is alive and clears stale
    // shared-session failure history. A negative telemetry transport error is
    // isolated here; shared operations retain responsibility for Wi-Fi recycle.
    if (responseFullyConsumed || status >= HTTP_CODE_CONTINUE) {
      backendTransportRecovery_.recordSuccess();
    }
  }

  static bool readHttpBodyToBuffer(HTTPClient& http, uint8_t* output,
                                   size_t capacity, size_t* outputSize) {
    if (output == nullptr || capacity == 0 || outputSize == nullptr) {
      return false;
    }
    const int declaredSize = http.getSize();
    if (declaredSize < 0 || declaredSize > static_cast<int>(capacity)) {
      return false;
    }
    FixedCapacityStream sink(output, capacity);
    const int transferred = http.writeToStream(&sink);
    const int completedSize = http.getSize();
    if (sink.overflowed() || transferred < 0 ||
        sink.size() != static_cast<size_t>(transferred) ||
        transferred != declaredSize ||
        (completedSize >= 0 && transferred != completedSize)) {
      return false;
    }
    *outputSize = sink.size();
    return true;
  }

  bool buildDeviceUrl(const char* relativePath, char* output,
                      size_t outputCapacity) const {
    if (relativePath == nullptr || output == nullptr || outputCapacity == 0 ||
        relativePath[0] != '/' || relativePath[1] == '/' ||
        std::strstr(relativePath, "..") != nullptr ||
        !isExplicitHttpUrl(workerSettings.serverBaseUrl,
                           sizeof(workerSettings.serverBaseUrl))) {
      return false;
    }
    const int length = std::snprintf(output, outputCapacity, "%s%s",
                                     workerSettings.serverBaseUrl,
                                     relativePath);
    return length > 0 && static_cast<size_t>(length) < outputCapacity;
  }

  void initializeCommandJournal() {
    CommandJournalRecord stored = {};
    const CommandJournalStorageResult result =
        loadCommandJournalRecord(&stored);
    if (result != CommandJournalStorageResult::kOk) {
      if (result != CommandJournalStorageResult::kNotStored) {
        Serial.printf("EVENT,control,journal_load_failed,%u\n",
                      static_cast<unsigned>(result));
        controlStorageHealthy_ = false;
        controlOperatorHalted_ = true;
      }
      return;
    }
    if (!commandJournal_.restore(stored)) {
      controlStorageHealthy_ = false;
      controlOperatorHalted_ = true;
      Serial.println("EVENT,control,journal_restore_failed");
      return;
    }
    const CommandJournalRecoveryAction recovery =
        commandJournalRecoveryAction(stored);
    if (recovery == CommandJournalRecoveryAction::kInvalid) {
      controlStorageHealthy_ = false;
      controlOperatorHalted_ = true;
      Serial.println("EVENT,control,journal_recovery_invalid");
      return;
    }
    if (recovery ==
        CommandJournalRecoveryAction::kFailInterruptedThenAck) {
      // A side effect interrupted by reset is never repeated blindly. The
      // terminal failure uses the stored lease and deterministic ACK id.
      if (!commandJournal_.transition(CommandExecutionStatus::kFailed) ||
          saveCommandJournalRecord(commandJournal_.record()) !=
              CommandJournalStorageResult::kOk) {
        controlStorageHealthy_ = false;
        controlOperatorHalted_ = true;
        Serial.println("EVENT,control,journal_recovery_failed");
        return;
      }
      Serial.printf("EVENT,control,interrupted,%s\n",
                    stored.commandId);
    }
    // This also covers the power-loss point after a terminal journal write but
    // before its ACK. ACK ids are deterministic and the backend endpoint is
    // idempotent, so replay is always safe.
    commandStep_ = CommandStep::kAckFinal;
    nextCommandAckMs_ = 0;
  }

  void initializeCoreDump() {
    const CoreDumpStorageBackend backend = coreDumpEsp32StorageBackend();
    if (!workerSettings.coreDumpAttributionAvailable) {
      CoreDumpImageDescriptor descriptor = {};
      const CoreDumpProbeStatus probe =
          backend.probe == nullptr
              ? CoreDumpProbeStatus::kIoError
              : backend.probe(backend.context, &descriptor);
      if (probe == CoreDumpProbeStatus::kNotFound) {
        clearPinnedCrashDumpAttribution();
      } else if (probe == CoreDumpProbeStatus::kPresent) {
        // Preserve the dump indefinitely rather than assigning the current or
        // an ambiguous boot/build identity.
        Serial.println("EVENT,coredump,attribution_unavailable,preserved");
      } else {
        Serial.println("EVENT,coredump,probe_failed,preserved");
      }
      return;
    }
    const CoreDumpReportContext context{
        workerIdentity.deviceId,
        workerSettings.coreDumpAttribution.bootId,
        workerSettings.coreDumpAttribution.buildId,
        workerSettings.coreDumpAttribution.resetReason};
    const CoreDumpPrepareResult result =
        pendingCoreDump_.prepare(backend, context);
    if (result == CoreDumpPrepareResult::kReady) {
      coreDumpReady_ = true;
      Serial.printf("EVENT,coredump,ready,%s,%u\n",
                    pendingCoreDump_.metadata().crashId,
                    pendingCoreDump_.metadata().dumpSize);
    } else if (result == CoreDumpPrepareResult::kNoDump) {
      clearPinnedCrashDumpAttribution();
    } else {
      Serial.printf("EVENT,coredump,prepare_failed,%s\n",
                    coreDumpPrepareResultName(result));
    }
  }

  void scheduleCoreDumpRetry(uint64_t now) {
    nextCoreDumpAttemptMs_ =
        now + boundedRetryDelay(coreDumpRetryExponent_,
                                kCoreDumpRetryMaximumMs, esp_random());
    if (coreDumpRetryExponent_ < 8) {
      ++coreDumpRetryExponent_;
    }
  }

  void uploadCoreDumpIfNeeded(uint64_t now) {
    if (!workerSettings.configured || !coreDumpReady_ ||
        !pendingCoreDump_.ready() || now < nextCoreDumpAttemptMs_ ||
        productionOtaActive_) {
      return;
    }
    CoreDumpArduinoJsonStream body(pendingCoreDump_);
    if (!body.valid() || body.contentLength() == 0) {
      scheduleCoreDumpRetry(now);
      return;
    }
    char relative[128] = {};
    std::snprintf(relative, sizeof(relative), "/v1/devices/%s/coredumps",
                  workerIdentity.deviceId);
    char url[256] = {};
    if (!buildDeviceUrl(relative, url, sizeof(url))) {
      scheduleCoreDumpRetry(now);
      return;
    }
    if (!beginBackendRequest(url, 15000)) {
      scheduleCoreDumpRetry(now);
      return;
    }
    HTTPClient& http = backendHttp_;
    http.addHeader("Content-Type", "application/json");
    http.setAuthorizationType("Bearer");
    http.setAuthorization(workerSettings.apiToken);
    WatchdogFeedingStream watchedBody(body);
    const int status =
        http.sendRequest("POST", &watchedBody, body.contentLength());
    String response;
    const bool responseOk = status == HTTP_CODE_OK && body.complete() &&
                            !body.failed() &&
                            readExactAckBody(http, response);
    endBackendRequest(responseOk, status);
    if (!responseOk) {
      scheduleCoreDumpRetry(now);
      Serial.printf("EVENT,coredump,retry,%d\n", status);
      return;
    }
    const CoreDumpAckResult parsed = parseCoreDumpAck(
        response.c_str(), response.length(),
        pendingCoreDump_.metadata().crashId);
    if (!parsed.ok()) {
      scheduleCoreDumpRetry(now);
      Serial.printf("EVENT,coredump,bad_ack,%s\n",
                    coreDumpAckErrorName(parsed.error));
      return;
    }
    char crashId[CoreDumpReportMetadata::kCrashIdCapacity] = {};
    std::snprintf(crashId, sizeof(crashId), "%s",
                  pendingCoreDump_.metadata().crashId);
    const CoreDumpAcknowledgeResult acknowledged =
        pendingCoreDump_.acknowledgeDurable(crashId, parsed.durable);
    if (acknowledged != CoreDumpAcknowledgeResult::kErased) {
      scheduleCoreDumpRetry(now);
      Serial.printf("EVENT,coredump,erase_retry,%s\n",
                    coreDumpAcknowledgeResultName(acknowledged));
      return;
    }
    coreDumpReady_ = false;
    coreDumpRetryExponent_ = 0;
    const CrashDumpAttributionStorageResult pinClear =
        clearPinnedCrashDumpAttribution();
    if (pinClear != CrashDumpAttributionStorageResult::kOk) {
      Serial.printf("EVENT,coredump,pin_clear_failed,%u\n",
                    static_cast<unsigned>(pinClear));
    }
    Serial.printf("EVENT,coredump,acked_erased,%s\n", crashId);
  }

  bool persistCommandJournal() {
    const CommandJournalStorageResult result =
        saveCommandJournalRecord(commandJournal_.record());
    if (result == CommandJournalStorageResult::kOk) {
      return true;
    }
    controlStorageHealthy_ = false;
    controlOperatorHalted_ = true;
    Serial.printf("EVENT,control,journal_save_failed,%u\n",
                  static_cast<unsigned>(result));
    return false;
  }

  uint64_t estimatedServerUtcMs(uint64_t now) const {
    if (!hasControlClockAnchor_) {
      return 0;
    }
    return controlAnchorUtcMs_ +
           (now >= controlAnchorUptimeMs_ ? now - controlAnchorUptimeMs_ : 0);
  }

  void scheduleCommandAckRetry(uint64_t now) {
    nextCommandAckMs_ =
        now + boundedRetryDelay(commandAckRetryExponent_,
                                kCommandAckRetryMaximumMs, esp_random());
    if (commandAckRetryExponent_ < 8) {
      ++commandAckRetryExponent_;
    }
  }

  void setCommandTerminal(CommandExecutionStatus status, uint64_t now) {
    if (!commandJournal_.transition(status) || !persistCommandJournal()) {
      commandStep_ = CommandStep::kIdle;
      hasActiveCommand_ = false;
      return;
    }
    commandStep_ = CommandStep::kAckFinal;
    commandAckRetryExponent_ = 0;
    nextCommandAckMs_ = now;
  }

  bool openDevelopmentWindow(uint64_t now) {
    if (!developmentOtaConfigured_ || !installStateHealthy_ ||
        installState_.pending || installState_.developmentPending ||
        productionOtaActive_ || pendingImageRunning_) {
      otaRuntimeError_ = OtaRuntimeError::kDevelopmentSecretMissing;
      return false;
    }
    if (WiFi.status() != WL_CONNECTED) {
      otaRuntimeError_ = OtaRuntimeError::kNetwork;
      return false;
    }
    const IPAddress ip = WiFi.localIP();
    const uint8_t address[4] = {ip[0], ip[1], ip[2], ip[3]};
    if (!otaIpv4IsTrustedLan(address) ||
        !developmentWindow_.openAfterPhysicalConfirmation(now)) {
      otaRuntimeError_ = OtaRuntimeError::kDevelopmentServiceFailed;
      return false;
    }
    if (!prepareCriticalQueuesForOta(now)) {
      developmentWindow_.close();
      otaRuntimeError_ = OtaRuntimeError::kImageRejected;
      return false;
    }
    resetUpdateSafetyMetrics();
    developmentUploadJournalStaged_ = false;
    developmentRebootPending_ = false;
    if (!otaStartArduinoDevelopmentService(
            &developmentWindow_, developmentHostname_,
            developmentSafetyAbortCheck, developmentCompletionCheck,
            developmentActivityCallback, this)) {
      developmentWindow_.noteUploadFailed(
          OtaDevelopmentWindowError::kPlatformUnavailable);
      otaRuntimeError_ = OtaRuntimeError::kDevelopmentServiceFailed;
      return false;
    }
    developmentServiceStarted_ = true;
    otaRuntimeError_ = OtaRuntimeError::kNone;
    Serial.printf("EVENT,ota,development_open,%s,%s\n",
                  ip.toString().c_str(), developmentHostname_);
    return true;
  }

  void executeCurrentCommand(uint64_t now) {
    if (!hasActiveCommand_) {
      setCommandTerminal(CommandExecutionStatus::kFailed, now);
      return;
    }
    switch (activeCommand_.action) {
      case RemoteCommandAction::kDiagnosticSnapshot:
        nextHealthAttemptMs_ = 0;
        publishWorkerHealth(now);
        setCommandTerminal(CommandExecutionStatus::kSucceeded, now);
        return;
      case RemoteCommandAction::kSetLogLevel:
        if (activeCommand_.detailedLog) {
          if (!remoteLogSession_.beginDetailed(
                  now, static_cast<uint64_t>(activeCommand_.durationSeconds) *
                           1000ULL)) {
            setCommandTerminal(CommandExecutionStatus::kFailed, now);
            return;
          }
        } else {
          remoteLogSession_.stop();
        }
        setCommandTerminal(CommandExecutionStatus::kSucceeded, now);
        return;
      case RemoteCommandAction::kRetryUpload:
        operatorHalted_ = false;
        resetBackoff(now);
        nextConfigPollMs_ = now;
        nextHealthAttemptMs_ = now;
        nextOperationalLogMs_ = now;
        setCommandTerminal(CommandExecutionStatus::kSucceeded, now);
        return;
      case RemoteCommandAction::kReboot:
        restartAfterFinalCommandAck_ = true;
        setCommandTerminal(CommandExecutionStatus::kSucceeded, now);
        return;
      case RemoteCommandAction::kRecalibrateMicrophone:
      case RemoteCommandAction::kOpenDevOta:
        break;
    }

    if (workerControlMailbox == nullptr) {
      setCommandTerminal(CommandExecutionStatus::kFailed, now);
      return;
    }
    MainControlRequest request = {};
    std::memcpy(request.commandId, activeCommand_.commandId,
                sizeof(request.commandId));
    request.action = activeCommand_.action;
    request.durationSeconds = activeCommand_.durationSeconds;
    request.detailedLog = activeCommand_.detailedLog;
    request.requiresLocalConfirmation =
        activeCommand_.action == RemoteCommandAction::kOpenDevOta;
    const uint64_t serverNow = estimatedServerUtcMs(now);
    const uint64_t remaining =
        serverNow != 0 && activeCommand_.expiresAtMs > serverNow
            ? activeCommand_.expiresAtMs - serverNow
            : 0;
    request.expiresAtMs = now + remaining;
    if (remaining == 0 || !workerControlMailbox->publishRequest(request)) {
      setCommandTerminal(remaining == 0 ? CommandExecutionStatus::kExpired
                                       : CommandExecutionStatus::kFailed,
                         now);
      return;
    }
    commandStep_ = CommandStep::kWaitingMain;
  }

  void serviceMainControlResult(uint64_t now) {
    if (commandStep_ != CommandStep::kWaitingMain ||
        workerControlMailbox == nullptr) {
      return;
    }
    MainControlResult result = {};
    if (!workerControlMailbox->takeResult(&result)) {
      const uint64_t serverNow = estimatedServerUtcMs(now);
      if (serverNow != 0 && serverNow >= commandJournal_.record().expiresAtMs) {
        setCommandTerminal(CommandExecutionStatus::kExpired, now);
      }
      return;
    }
    if (std::strcmp(result.commandId, commandJournal_.record().commandId) != 0) {
      setCommandTerminal(CommandExecutionStatus::kFailed, now);
      return;
    }
    if (result.code == MainControlResultCode::kSucceeded &&
        activeCommand_.action == RemoteCommandAction::kOpenDevOta &&
        !openDevelopmentWindow(now)) {
      setCommandTerminal(CommandExecutionStatus::kFailed, now);
      return;
    }
    switch (result.code) {
      case MainControlResultCode::kSucceeded:
        setCommandTerminal(CommandExecutionStatus::kSucceeded, now);
        break;
      case MainControlResultCode::kFailed:
        setCommandTerminal(CommandExecutionStatus::kFailed, now);
        break;
      case MainControlResultCode::kRejected:
        otaRuntimeError_ = OtaRuntimeError::kLocalConfirmationRejected;
        setCommandTerminal(CommandExecutionStatus::kRejected, now);
        break;
      case MainControlResultCode::kExpired:
        setCommandTerminal(CommandExecutionStatus::kExpired, now);
        break;
    }
  }

  void serviceCommandAcknowledgement(uint64_t now) {
    if ((commandStep_ != CommandStep::kAckAccepted &&
         commandStep_ != CommandStep::kAckRunning &&
         commandStep_ != CommandStep::kAckFinal) ||
        now < nextCommandAckMs_ || !commandJournal_.hasRecord()) {
      return;
    }

    FixedBufferSink payload{commandAckPayloadBytes,
                            sizeof(commandAckPayloadBytes), 0};
    const CommandAckJsonSink sink{&payload, bufferSink};
    if (!writeCommandAckJson(commandJournal_.record(), sink)) {
      controlOperatorHalted_ = true;
      Serial.println("EVENT,control,ack_serialize_failed");
      return;
    }

    char relative[128] = {};
    std::snprintf(relative, sizeof(relative), "/v1/devices/%s/control/acks",
                  workerIdentity.deviceId);
    char url[256] = {};
    if (!buildDeviceUrl(relative, url, sizeof(url))) {
      controlOperatorHalted_ = true;
      return;
    }
    if (!beginBackendRequest(url, 5000)) {
      scheduleCommandAckRetry(now);
      return;
    }
    HTTPClient& http = backendHttp_;
    http.addHeader("Content-Type", "application/json");
    http.setAuthorizationType("Bearer");
    http.setAuthorization(workerSettings.apiToken);
    const int status = http.sendRequest("POST", commandAckPayloadBytes,
                                        payload.size);
    String response;
    const bool bodyOk = status == HTTP_CODE_OK &&
                        readExactAckBody(http, response);
    endBackendRequest(bodyOk, status);
    if (!bodyOk) {
      if (status == HTTP_CODE_UNAUTHORIZED) {
        controlOperatorHalted_ = true;
      }
      scheduleCommandAckRetry(now);
      Serial.printf("EVENT,control,ack_retry,%d\n", status);
      return;
    }
    const CommandAckResponseResult parsed = parseCommandAckResponse(
        response.c_str(), response.length(), commandJournal_.record());
    if (!parsed.ok()) {
      scheduleCommandAckRetry(now);
      Serial.printf("EVENT,control,ack_bad_response,%s\n",
                    commandAckResponseErrorName(parsed.error));
      return;
    }

    commandAckRetryExponent_ = 0;
    nextCommandAckMs_ = 0;
    if (commandStep_ == CommandStep::kAckAccepted) {
      if (!commandJournal_.transition(CommandExecutionStatus::kRunning) ||
          !persistCommandJournal()) {
        return;
      }
      commandStep_ = CommandStep::kAckRunning;
      nextCommandAckMs_ = now;
      return;
    }
    if (commandStep_ == CommandStep::kAckRunning) {
      executeCurrentCommand(now);
      return;
    }

    Serial.printf("EVENT,control,command_done,%s,%s\n",
                  commandJournal_.record().commandId,
                  commandExecutionStatusWireName(
                      commandJournal_.record().status));
    commandStep_ = CommandStep::kIdle;
    hasActiveCommand_ = false;
    if (restartAfterFinalCommandAck_) {
      restartAfterFinalCommandAck_ = false;
      delay(100);
      ESP.restart();
    }
  }

  void handlePolledCommand(const RemoteCommandEnvelope& command,
                           uint64_t serverUtcMs, uint64_t now) {
    const CommandAcceptance acceptance =
        commandJournal_.consider(command, serverUtcMs);
    switch (acceptance) {
      case CommandAcceptance::kAcceptedNew:
        if (!persistCommandJournal()) {
          return;
        }
        activeCommand_ = command;
        hasActiveCommand_ = true;
        commandStep_ = CommandStep::kAckAccepted;
        commandAckRetryExponent_ = 0;
        nextCommandAckMs_ = now;
        Serial.printf("EVENT,control,accepted,%s\n", command.commandId);
        return;
      case CommandAcceptance::kReplayExisting:
        // consider() refreshed the lease. Persist it before emitting an ACK
        // derived from that lease, even for a terminal replay.
        if (!persistCommandJournal()) {
          return;
        }
        if (commandExecutionStatusIsTerminal(commandJournal_.record().status)) {
          commandStep_ = CommandStep::kAckFinal;
          nextCommandAckMs_ = now;
        } else if (commandStep_ == CommandStep::kAckAccepted ||
                   commandStep_ == CommandStep::kAckRunning) {
          commandAckRetryExponent_ = 0;
          nextCommandAckMs_ = now;
        }
        return;
      case CommandAcceptance::kExpired:
        Serial.printf("EVENT,control,command_expired,%s\n", command.commandId);
        return;
      case CommandAcceptance::kLeaseExpired:
        Serial.printf("EVENT,control,lease_expired,%s\n", command.commandId);
        return;
      case CommandAcceptance::kBusy:
        Serial.printf("EVENT,control,busy,%s\n", command.commandId);
        return;
      case CommandAcceptance::kInvalid:
        Serial.println("EVENT,control,invalid_command");
        return;
    }
  }

  void scheduleControlRetry(uint64_t now) {
    const ControlRetrySchedule retry = scheduleControlPoll(
        now, false, controlRetryExponent_, esp_random());
    nextControlPollMs_ = retry.nextAttemptMs;
    controlRetryExponent_ = retry.nextFailureExponent;
  }

  void pollRemoteControl(uint64_t now) {
    char relative[128] = {};
    std::snprintf(relative, sizeof(relative), "/v1/devices/%s/control",
                  workerIdentity.deviceId);
    char url[256] = {};
    if (!buildDeviceUrl(relative, url, sizeof(url))) {
      controlOperatorHalted_ = true;
      otaRuntimeError_ = OtaRuntimeError::kControlProtocol;
      return;
    }

    if (!beginBackendRequest(url, 5000)) {
      scheduleControlRetry(now);
      return;
    }
    HTTPClient& http = backendHttp_;
    http.addHeader("Accept", "application/json");
    http.setAuthorizationType("Bearer");
    http.setAuthorization(workerSettings.apiToken);
    const int status = http.GET();
    if (status == HTTP_CODE_UNAUTHORIZED) {
      endBackendRequest(false, status);
      controlOperatorHalted_ = true;
      Serial.println("EVENT,control,operator_halt,401");
      return;
    }
    if (status != HTTP_CODE_OK) {
      endBackendRequest(false, status);
      scheduleControlRetry(now);
      Serial.printf("EVENT,control,poll_retry,%d\n", status);
      return;
    }
    size_t responseSize = 0;
    const bool bodyOk = readHttpBodyToBuffer(
        http, controlResponseBytes, kMaximumControlResponseBytes,
        &responseSize);
    endBackendRequest(bodyOk, status);
    if (!bodyOk) {
      scheduleControlRetry(now);
      Serial.println("EVENT,control,poll_bad_body");
      return;
    }
    controlResponseBytes[responseSize] = '\0';
    const ControlPollParseResult parsed = parseControlPollResponse(
        reinterpret_cast<const char*>(controlResponseBytes), responseSize);
    if (!parsed.ok()) {
      scheduleControlRetry(now);
      otaRuntimeError_ = OtaRuntimeError::kControlProtocol;
      Serial.printf("EVENT,control,poll_bad_response,%s\n",
                    controlPollParseErrorName(parsed.error));
      return;
    }

    const uint64_t completedMs = monotonicMillis();
    controlAnchorUtcMs_ = parsed.value.serverUtcMs;
    controlAnchorUptimeMs_ = completedMs;
    hasControlClockAnchor_ = true;
    const ControlRetrySchedule next =
        scheduleControlPoll(completedMs, true, controlRetryExponent_, 0);
    nextControlPollMs_ = next.nextAttemptMs;
    controlRetryExponent_ = next.nextFailureExponent;
    if (parsed.value.hasCommand) {
      handlePolledCommand(parsed.value.command, parsed.value.serverUtcMs,
                          completedMs);
      return;
    }
    if (parsed.value.hasDesiredRelease && commandStep_ == CommandStep::kIdle &&
        completedMs >= nextOtaReleaseAttemptMs_) {
      installDesiredRelease(parsed.value.desiredRelease, completedMs);
    }
  }

  void expireDetailedLogSession(uint64_t now) {
    const bool active = remoteLogSession_.mode(now) == RemoteLogMode::kDetailed;
    if (active != detailedLogWasActive_) {
      detailedLogWasActive_ = active;
      OperationalLogEvent event = {};
      event.sequence = nextWorkerLogSequence_++;
      event.uptimeMs = now;
      event.level = OperationalLogLevel::kInfo;
      event.code = OperationalLogCode::kDebugSessionChanged;
      event.value0 = active ? 1 : 0;
      if (workerOperationalLog != nullptr) {
        workerOperationalLog->push(event);
      }
    }
  }

  void scheduleOperationalLogRetry(uint64_t now) {
    nextOperationalLogMs_ =
        now + boundedRetryDelay(operationalLogRetryExponent_,
                                kOperationalLogRetryMaximumMs, esp_random());
    if (operationalLogRetryExponent_ < 8) {
      ++operationalLogRetryExponent_;
    }
  }

  void uploadOperationalLogsIfNeeded(uint64_t now) {
    if (!workerSettings.configured || workerOperationalLog == nullptr ||
        now < nextOperationalLogMs_ || workerOperationalLog->size() == 0 ||
        productionOtaActive_) {
      return;
    }
    OperationalLogEvent events[kOperationalLogBatchSize] = {};
    const size_t count =
        workerOperationalLog->copyPrefix(events, kOperationalLogBatchSize);
    if (count == 0) {
      nextOperationalLogMs_ = now + 1000;
      return;
    }
    char batchId[97] = {};
    const int batchIdLength = std::snprintf(
        batchId, sizeof(batchId), "log-%s-%llu-%llu", workerIdentity.bootId,
        static_cast<unsigned long long>(events[0].sequence),
        static_cast<unsigned long long>(events[count - 1].sequence));
    if (batchIdLength <= 0 ||
        static_cast<size_t>(batchIdLength) >= sizeof(batchId)) {
      scheduleOperationalLogRetry(now);
      return;
    }
    FixedBufferSink payload{operationalLogPayloadBytes,
                            sizeof(operationalLogPayloadBytes), 0};
    const OperationalLogBatchContext context{batchId, workerIdentity.bootId,
                                             kM5goBuildId};
    const OperationalLogJsonSink sink{&payload, bufferSink};
    if (!writeOperationalLogBatchJson(context, events, count, sink)) {
      scheduleOperationalLogRetry(now);
      Serial.println("EVENT,logs,serialize_failed");
      return;
    }

    char relative[128] = {};
    std::snprintf(relative, sizeof(relative),
                  "/v1/devices/%s/logs/batches", workerIdentity.deviceId);
    char url[256] = {};
    if (!buildDeviceUrl(relative, url, sizeof(url))) {
      scheduleOperationalLogRetry(now);
      return;
    }
    if (!beginBackendRequest(url, 5000)) {
      scheduleOperationalLogRetry(now);
      return;
    }
    HTTPClient& http = backendHttp_;
    http.addHeader("Content-Type", "application/json");
    http.setAuthorizationType("Bearer");
    http.setAuthorization(workerSettings.apiToken);
    const int status = http.sendRequest("POST", operationalLogPayloadBytes,
                                        payload.size);
    String response;
    const bool bodyOk = status == HTTP_CODE_OK &&
                        readExactAckBody(http, response);
    endBackendRequest(bodyOk, status);
    if (!bodyOk) {
      scheduleOperationalLogRetry(now);
      Serial.printf("EVENT,logs,retry,%d\n", status);
      return;
    }
    const OperationalLogAckResult parsed = parseOperationalLogAck(
        response.c_str(), response.length(), batchId, count);
    if (!parsed.ok() || !workerOperationalLog->commitPrefix(events, count)) {
      scheduleOperationalLogRetry(now);
      Serial.printf("EVENT,logs,bad_ack,%s\n",
                    parsed.ok() ? "prefix_changed"
                                : operationalLogAckErrorName(parsed.error));
      return;
    }
    operationalLogRetryExponent_ = 0;
    nextOperationalLogMs_ = monotonicMillis() + kOperationalLogIntervalMs;
    Serial.printf("EVENT,logs,acked,%s,%u\n", batchId,
                  static_cast<unsigned>(count));
  }

  static bool developmentSafetyAbortCheck(void* context) {
    UploaderWorker* worker = static_cast<UploaderWorker*>(context);
    if (worker == nullptr) {
      return false;
    }
    worker->feedWatchdog();
    if (!worker->criticalQueueGuardActive_ &&
        !worker->prepareCriticalQueuesForOta(monotonicMillis())) {
      worker->safetyAcceptanceFailed_ = true;
      return true;
    }
    if (!worker->developmentUploadJournalStaged_) {
      const uint32_t targetAddress =
          otaInactiveApplicationPartitionAddress();
      OtaInstallState staged = worker->installState_;
      if (!otaStagePendingDevelopmentImage(&staged, targetAddress) ||
          saveOtaInstallState(staged) !=
              OtaInstallStateStorageResult::kOk) {
        worker->installStateHealthy_ = false;
        worker->otaRuntimeError_ = OtaRuntimeError::kStorage;
        Serial.println("EVENT,ota,development_journal_stage_failed");
        return true;
      }
      worker->installState_ = staged;
      worker->developmentUploadJournalStaged_ = true;
    }
    if (!worker->criticalQueueGuardActive_) {
      worker->armCriticalQueueGuard();
    }
    return worker->consumeSafetyAbort() ||
           !worker->criticalQueuesSafeForOta();
  }

  static bool developmentCompletionCheck(void* context) {
    UploaderWorker* worker = static_cast<UploaderWorker*>(context);
    if (worker == nullptr) {
      return false;
    }
    worker->feedWatchdog();
    // ArduinoOTA calls onEnd only after Update.end(). Leave the transfer phase
    // active long enough for main to publish measurements that include the
    // final flash/partition-selection stall.
    vTaskDelay(pdMS_TO_TICKS(50));
    worker->feedWatchdog();
    // Close the tiny interval after the final progress callback as well: a
    // hard abort posted there must not be reduced to the aggregate <1% gate.
    bool accepted = !worker->consumeSafetyAbort() &&
                    worker->criticalQueuesSafeForOta() &&
                    worker->updateSafetyMetricsAcceptable();
    OtaApplicationImageIdentity candidate = {};
    if (accepted &&
        (!worker->developmentUploadJournalStaged_ ||
         !otaReadBootApplicationIdentity(&candidate) ||
         candidate.partitionAddress !=
             worker->installState_.pendingPartitionAddress)) {
      accepted = false;
    }
    if (accepted) {
      OtaInstallState acceptedState = worker->installState_;
      accepted = otaMarkPendingImageAccepted(
                     &acceptedState, candidate.partitionAddress,
                     candidate.sha256) &&
                 saveOtaInstallState(acceptedState) ==
                     OtaInstallStateStorageResult::kOk;
      if (accepted) {
        worker->installState_ = acceptedState;
        worker->developmentRebootPending_ = true;
      }
    }
    if (!accepted) {
      worker->endCriticalQueueGuard();
      worker->safetyAcceptanceFailed_ = true;
      worker->otaRuntimeError_ = OtaRuntimeError::kImageRejected;
      // Update.end() has already selected the rejected candidate. Preserve the
      // durable staged/unaccepted transaction and do not re-select the running
      // slot: esp_ota_set_boot_partition(running) would put the known-good slot
      // back into NEW/PENDING_VERIFY and make the rejected candidate its
      // reverse-rollback target. On the next boot the exact policy rejects the
      // candidate's PENDING_VERIFY image and rolls back to the untouched slot.
      Serial.println("EVENT,ota,development_acceptance_rejected");
    }
    return accepted;
  }

  static void developmentActivityCallback(void* context) {
    UploaderWorker* worker = static_cast<UploaderWorker*>(context);
    if (worker == nullptr) {
      return;
    }
    worker->refreshUpdateSafetyMetrics();
    worker->feedWatchdog();
    worker->publishRuntimeActivity(monotonicMillis(), true);
  }

  void refreshUpdateSafetyMetrics() {
    if (workerOtaRuntimeMailbox == nullptr) {
      return;
    }
    OtaSafetyAbortRequest metrics = {};
    if (workerOtaRuntimeMailbox->takeSafetyMetrics(&metrics)) {
      lastSafetyMetrics_ = metrics;
    }
  }

  void resetUpdateSafetyMetrics() {
    lastSafetyMetrics_ = {};
    safetyAbortLatched_ = false;
    safetyAcceptanceFailed_ = false;
    endCriticalQueueGuard();
    guardedTelemetryCriticalDrops_ =
        workerQueue == nullptr ? 0 : workerQueue->droppedCritical();
    guardedFeedbackDrops_ = workerTouchFeedbackQueue == nullptr
                                ? 0
                                : workerTouchFeedbackQueue->droppedFull();
    if (workerOtaRuntimeMailbox == nullptr) {
      return;
    }
    OtaSafetyAbortRequest discarded = {};
    while (workerOtaRuntimeMailbox->takeSafetyMetrics(&discarded)) {
    }
    while (workerOtaRuntimeMailbox->takeSafetyAbort(&discarded)) {
    }
  }

  bool updateSafetyMetricsAcceptable() {
    refreshUpdateSafetyMetrics();
    if (lastSafetyMetrics_.maximumMainLoopGapMs >= 250) {
      return false;
    }
    if (lastSafetyMetrics_.totalMicrophoneWindows == 0) {
      return false;
    }
    return static_cast<uint64_t>(lastSafetyMetrics_.invalidMicrophoneWindows) *
               100ULL <
           lastSafetyMetrics_.totalMicrophoneWindows;
  }

  bool consumeSafetyAbort() {
    if (workerOtaRuntimeMailbox == nullptr) {
      return false;
    }
    OtaSafetyAbortRequest request = {};
    if (!workerOtaRuntimeMailbox->takeSafetyAbort(&request)) {
      return false;
    }
    lastSafetyMetrics_ = request;
    safetyAbortLatched_ = true;
    otaRuntimeError_ = OtaRuntimeError::kImageRejected;
    Serial.printf("EVENT,ota,safety_abort,%u,%u,%u,%u\n",
                  request.maximumMainLoopGapMs,
                  request.invalidMicrophoneWindows,
                  request.totalMicrophoneWindows,
                  request.consecutiveInvalidMicrophoneWindows);
    return true;
  }

  void armCriticalQueueGuard() {
    if (workerQueue != nullptr) {
      workerQueue->setCriticalOnly(true);
    }
    guardedTelemetryCriticalDrops_ =
        workerQueue == nullptr ? 0 : workerQueue->droppedCritical();
    guardedFeedbackDrops_ = workerTouchFeedbackQueue == nullptr
                                ? 0
                                : workerTouchFeedbackQueue->droppedFull();
    criticalQueueAbortLatched_ = false;
    criticalQueueGuardActive_ = true;
  }

  void endCriticalQueueGuard() {
    if (workerQueue != nullptr) {
      workerQueue->setCriticalOnly(false);
    }
    criticalQueueGuardActive_ = false;
  }

  bool criticalQueuesSafeForOta() {
    const bool safe =
        criticalQueueGuardActive_ && workerQueue != nullptr &&
        workerTouchFeedbackQueue != nullptr &&
        workerQueue->droppedCritical() == guardedTelemetryCriticalDrops_ &&
        workerTouchFeedbackQueue->droppedFull() == guardedFeedbackDrops_ &&
        !workerQueue->hasCritical() && workerTouchFeedbackQueue->size() == 0;
    if (!safe && !criticalQueueAbortLatched_) {
      criticalQueueAbortLatched_ = true;
      safetyAcceptanceFailed_ = true;
      otaRuntimeError_ = OtaRuntimeError::kImageRejected;
      Serial.println("EVENT,ota,critical_queue_backpressure");
    }
    return safe;
  }

  bool prepareCriticalQueuesForOta(uint64_t now) {
    if (!filesystemReady_ || workerQueue == nullptr ||
        workerTouchFeedbackQueue == nullptr) {
      return false;
    }
    for (size_t attempt = 0;
         workerQueue->size() != 0 && attempt < TelemetryQueue::kCapacity;
         ++attempt) {
      const size_t before = workerQueue->size();
      freezeQueueIfNeeded(now, true);
      if (workerQueue->size() >= before) {
        return false;
      }
    }
    for (size_t attempt = 0;
         workerTouchFeedbackQueue->size() != 0 &&
         attempt < TouchFeedbackQueue::kCapacity;
         ++attempt) {
      const size_t before = workerTouchFeedbackQueue->size();
      freezeFeedbackQueueIfNeeded();
      if (workerTouchFeedbackQueue->size() >= before) {
        return false;
      }
    }
    return workerQueue->size() == 0 &&
           workerTouchFeedbackQueue->size() == 0;
  }

  void initializeOtaRuntime(uint64_t now) {
    const void* secretEnd = std::memchr(
        workerSettings.otaDevelopmentSecret, '\0',
        sizeof(workerSettings.otaDevelopmentSecret));
    const size_t secretLength =
        secretEnd == nullptr
            ? sizeof(workerSettings.otaDevelopmentSecret)
            : static_cast<const char*>(secretEnd) -
                  workerSettings.otaDevelopmentSecret;
    developmentOtaConfigured_ =
        secretLength == kOtaDevelopmentSecretLength &&
        developmentWindow_.configureSecret(
            workerSettings.otaDevelopmentSecret, secretLength);
    const char* suffix = std::strrchr(workerIdentity.deviceId, '-');
    suffix = suffix == nullptr ? workerIdentity.deviceId : suffix + 1;
    const int hostnameLength = std::snprintf(
        developmentHostname_, sizeof(developmentHostname_), "m5go-%s", suffix);
    if (hostnameLength <= 0 ||
        static_cast<size_t>(hostnameLength) >= sizeof(developmentHostname_)) {
      developmentOtaConfigured_ = false;
      developmentHostname_[0] = '\0';
    }

    const OtaInstallStateStorageResult stateResult =
        loadOtaInstallState(&installState_);
    installStateKnown_ = true;
    const bool stateNotStored =
        stateResult == OtaInstallStateStorageResult::kNotStored;
    installStateHealthy_ =
        stateResult == OtaInstallStateStorageResult::kOk || stateNotStored;
    if (stateNotStored) {
      installState_ = {};
    }

    OtaBootRecoveryAction recovery = OtaBootRecoveryAction::kHalt;
    if (installStateHealthy_) {
      // Every startup, including an empty journal, goes through the same exact
      // state/identity policy. In particular, a missing journal cannot turn an
      // untracked PENDING_VERIFY image or an unreadable boot state into an
      // ordinary boot.
      recovery = decideOtaBootRecovery(
          installState_, workerSettings.bootImageInfo, kM5goFirmwareVersion,
          kM5goBuildId);
    }
    if (stateNotStored && recovery == OtaBootRecoveryAction::kOrdinary) {
      // Establish a real empty journal only after the boot policy proves this
      // is an ordinary VALID/UNDEFINED image. A later pending boot can then
      // distinguish an accepted development upload from absent state.
      installStateHealthy_ =
          saveOtaInstallState(installState_) ==
          OtaInstallStateStorageResult::kOk;
    }
    if (!installStateHealthy_) {
      otaRuntimeError_ = workerSettings.bootValidationPending
                             ? OtaRuntimeError::kBootValidationFailed
                             : OtaRuntimeError::kStorage;
      otaPhase_ = OtaRuntimePhase::kFailed;
      Serial.printf("EVENT,ota,state_load_failed,%u\n",
                    static_cast<unsigned>(stateResult));
    } else {
      const bool production = installState_.pending;
      if (production) {
        std::snprintf(activeReleaseId_, sizeof(activeReleaseId_), "%s",
                      installState_.pendingReleaseId);
      }
      if (recovery == OtaBootRecoveryAction::kValidatePendingImage) {
        otaPhase_ = OtaRuntimePhase::kValidating;
        pendingImageRunning_ = true;
        pendingValidatingReport_ = production;
      } else if (recovery == OtaBootRecoveryAction::kPromoteProduction ||
                 recovery == OtaBootRecoveryAction::kClearDevelopment) {
        // Power may have failed after bootloader confirmation but before the
        // NVS transaction was promoted/cleared. Exact VALID state and exact
        // partition digest are both required to finish it.
        OtaInstallState promoted = installState_;
        const bool exactConfirmedDevelopment =
            !production &&
            workerSettings.bootImageInfo.state ==
                OtaRunningImageState::kValid &&
            otaPendingImageIdentityMatches(
                installState_,
                workerSettings.bootImageInfo.identity.partitionAddress,
                workerSettings.bootImageInfo.identity.sha256);
        bool promotedOk = false;
        if (production) {
          promotedOk = otaConfirmPendingRelease(&promoted, kM5goBuildId);
        } else if (exactConfirmedDevelopment) {
          promotedOk = otaConfirmPendingDevelopmentImage(&promoted);
        } else {
          otaCancelPendingDevelopmentImage(&promoted);
          promotedOk = otaInstallStateIsValid(promoted);
        }
        if (!promotedOk ||
            saveOtaInstallState(promoted) !=
                OtaInstallStateStorageResult::kOk) {
          installStateHealthy_ = false;
          otaRuntimeError_ = OtaRuntimeError::kStorage;
          otaPhase_ = OtaRuntimePhase::kFailed;
        } else {
          installState_ = promoted;
          if (production) {
            pendingRunningReport_ = isCurrentConfirmedProduction();
            std::snprintf(activeReleaseId_, sizeof(activeReleaseId_), "%s",
                          installState_.confirmedReleaseId);
          } else {
            activeReleaseId_[0] = '\0';
          }
          otaPhase_ = isCurrentConfirmedProduction() ||
                              installState_.runningDevelopmentImage
                          ? OtaRuntimePhase::kRunning
                          : OtaRuntimePhase::kInactive;
        }
      } else if (recovery ==
                 OtaBootRecoveryAction::kReportProductionRollback) {
        // The candidate was never selected or the bootloader already rolled
        // it back. Report the durable pending release before clearing it.
        otaPhase_ = OtaRuntimePhase::kFailed;
        pendingRollbackReport_ = true;
      } else if (recovery == OtaBootRecoveryAction::kOrdinary) {
        if (isCurrentConfirmedProduction()) {
          std::snprintf(activeReleaseId_, sizeof(activeReleaseId_), "%s",
                        installState_.confirmedReleaseId);
          otaPhase_ = OtaRuntimePhase::kRunning;
          pendingRunningReport_ = true;
        } else {
          // A persisted confirmed release is a rollback baseline after a
          // development upload, not evidence that it is the running image.
          activeReleaseId_[0] = '\0';
          otaPhase_ = installState_.runningDevelopmentImage
                          ? OtaRuntimePhase::kRunning
                          : OtaRuntimePhase::kInactive;
        }
      } else if (recovery ==
                 OtaBootRecoveryAction::kRollbackPendingImage) {
        // The main-loop validator already owns the destructive rollback API.
        // Publishing an unhealthy install state makes its existing hard-fail
        // mailbox path invalidate and reboot this exact PENDING image.
        installStateHealthy_ = false;
        otaRuntimeError_ = OtaRuntimeError::kBootValidationFailed;
        otaPhase_ = OtaRuntimePhase::kFailed;
      } else {
        // Unknown/error boot states and impossible stable transaction matrices
        // halt OTA without confirming an image or reporting a release running.
        installStateHealthy_ = false;
        otaRuntimeError_ = OtaRuntimeError::kBootValidationFailed;
        otaPhase_ = OtaRuntimePhase::kFailed;
        Serial.println("EVENT,ota,boot_policy_halt");
      }
    }
    if (!developmentOtaConfigured_ && otaRuntimeError_ == OtaRuntimeError::kNone) {
      // Development OTA being unprovisioned is visible but does not degrade
      // ordinary presence operation or production OTA.
      Serial.println("EVENT,ota,development_unconfigured");
    }
    nextRuntimePublishMs_ = now;
  }

  void serviceOtaRuntime(uint64_t now) {
    if (workerOtaRuntimeMailbox == nullptr) {
      return;
    }
    if (acceptedRebootPending_) {
      if (prepareCriticalQueuesForOta(now)) {
        Serial.println("EVENT,ota,accepted_reboot_now");
        ESP.restart();
      }
      return;
    }
    OtaSafetyAbortRequest metrics = {};
    if (workerOtaRuntimeMailbox->takeSafetyMetrics(&metrics)) {
      lastSafetyMetrics_ = metrics;
    }

    if (workerOtaRuntimeMailbox->takePhysicallyConfirmedDevelopmentOpen()) {
      if (!openDevelopmentWindow(now)) {
        otaPhase_ = OtaRuntimePhase::kFailed;
      }
    }

    const OtaBootValidationNotice notice =
        workerOtaRuntimeMailbox->takeBootValidationNotice();
    if (notice == OtaBootValidationNotice::kPrepareConfirmation &&
        (installState_.pending || installState_.developmentPending) &&
        pendingImageRunning_) {
      OtaInstallState prepared = installState_;
      if ((!prepared.pendingValidated &&
           !otaMarkPendingValidated(&prepared, kM5goBuildId)) ||
          saveOtaInstallState(prepared) !=
              OtaInstallStateStorageResult::kOk) {
        installStateHealthy_ = false;
        otaRuntimeError_ = OtaRuntimeError::kStorage;
        otaPhase_ = OtaRuntimePhase::kFailed;
      } else {
        installState_ = prepared;
        publishRuntimeActivity(now, true);
      }
    } else if (notice == OtaBootValidationNotice::kConfirmed &&
               (installState_.pending || installState_.developmentPending) &&
               pendingImageRunning_) {
      const bool production = installState_.pending;
      OtaInstallState promoted = installState_;
      const bool promotedOk =
          production
              ? otaConfirmPendingRelease(&promoted, kM5goBuildId)
              : otaConfirmPendingDevelopmentImage(&promoted);
      if (!promotedOk ||
          saveOtaInstallState(promoted) !=
              OtaInstallStateStorageResult::kOk) {
        installStateHealthy_ = false;
        otaRuntimeError_ = OtaRuntimeError::kStorage;
        otaPhase_ = OtaRuntimePhase::kFailed;
      } else {
        installState_ = promoted;
        pendingImageRunning_ = false;
        pendingValidatingReport_ = false;
        pendingRunningReport_ = production && isCurrentConfirmedProduction();
        if (production) {
          std::snprintf(activeReleaseId_, sizeof(activeReleaseId_), "%s",
                        installState_.confirmedReleaseId);
        } else {
          activeReleaseId_[0] = '\0';
        }
        otaPhase_ = isCurrentConfirmedProduction() ||
                            installState_.runningDevelopmentImage
                        ? OtaRuntimePhase::kRunning
                        : OtaRuntimePhase::kInactive;
        otaRuntimeError_ = OtaRuntimeError::kNone;
      }
    } else if (notice == OtaBootValidationNotice::kFailed &&
               (installState_.pending || installState_.developmentPending)) {
      pendingValidatingReport_ = false;
      otaRuntimeError_ = OtaRuntimeError::kBootValidationFailed;
      otaPhase_ = OtaRuntimePhase::kFailed;
    }

    if (consumeSafetyAbort()) {
      if (developmentServiceStarted_) {
        developmentWindow_.noteUploadFailed(
            OtaDevelopmentWindowError::kSafetyAbort);
        otaStopArduinoDevelopmentService(&developmentWindow_);
        developmentServiceStarted_ = false;
        endCriticalQueueGuard();
      }
      otaPhase_ = OtaRuntimePhase::kFailed;
    }

    if (developmentServiceStarted_ && WiFi.status() != WL_CONNECTED) {
      otaStopArduinoDevelopmentService(&developmentWindow_);
      developmentWindow_.close();
      developmentServiceStarted_ = false;
      endCriticalQueueGuard();
      otaRuntimeError_ = OtaRuntimeError::kNetwork;
      otaPhase_ = OtaRuntimePhase::kFailed;
    }

    if (WiFi.status() == WL_CONNECTED && pendingRunningReport_ &&
        now >= nextOtaReleaseAttemptMs_ &&
        reportReleaseStatus(installState_.confirmedReleaseId,
                            ReleaseReportPhase::kRunning, 100, nullptr,
                            installState_.previousReleaseId,
                            currentRunningProductionReleaseId(),
                            installState_.confirmedReleaseId)) {
      pendingRunningReport_ = false;
    }
    if (WiFi.status() == WL_CONNECTED && pendingValidatingReport_ &&
        now >= nextOtaReleaseAttemptMs_ &&
        reportReleaseStatus(installState_.pendingReleaseId,
                            ReleaseReportPhase::kValidating, 100, nullptr,
                            installState_.confirmedReleaseId,
                            installState_.pendingReleaseId,
                            installState_.confirmedReleaseId)) {
      pendingValidatingReport_ = false;
    }
    if (WiFi.status() == WL_CONNECTED && pendingRollbackReport_ &&
        now >= nextOtaReleaseAttemptMs_ &&
        reportReleaseStatus(installState_.pendingReleaseId,
                            ReleaseReportPhase::kRolledBack, 0,
                            "boot_validation_failed",
                            installState_.confirmedReleaseId,
                            currentRunningProductionReleaseId(),
                            installState_.confirmedReleaseId)) {
      otaCancelPendingRelease(&installState_);
      if (saveOtaInstallState(installState_) ==
          OtaInstallStateStorageResult::kOk) {
        pendingRollbackReport_ = false;
        otaPhase_ = isCurrentConfirmedProduction() ||
                            installState_.runningDevelopmentImage
                        ? OtaRuntimePhase::kRunning
                        : OtaRuntimePhase::kInactive;
      } else {
        installStateHealthy_ = false;
        otaRuntimeError_ = OtaRuntimeError::kStorage;
      }
    }
  }

  void serviceDevelopmentOta(uint64_t now) {
    if (!developmentServiceStarted_) {
      return;
    }
    otaServiceArduinoDevelopmentWindow(&developmentWindow_, now);
    const OtaDevelopmentWindowPhase phase = developmentWindow_.phase();
    if (phase == OtaDevelopmentWindowPhase::kFailed) {
      otaRuntimeError_ = developmentWindow_.error() ==
                                 OtaDevelopmentWindowError::kSafetyAbort
                             ? OtaRuntimeError::kImageRejected
                             : OtaRuntimeError::kDevelopmentServiceFailed;
      otaPhase_ = OtaRuntimePhase::kFailed;
      if (developmentUploadJournalStaged_) {
        OtaApplicationImageIdentity boot = {};
        OtaApplicationImageIdentity running = {};
        if (otaReadBootApplicationIdentity(&boot) &&
            otaReadRunningApplicationIdentity(&running) &&
            otaApplicationImageIdentityEquals(boot, running)) {
          OtaInstallState cleared = installState_;
          otaCancelPendingDevelopmentImage(&cleared);
          if (saveOtaInstallState(cleared) ==
              OtaInstallStateStorageResult::kOk) {
            installState_ = cleared;
          } else {
            installStateHealthy_ = false;
            otaRuntimeError_ = OtaRuntimeError::kStorage;
          }
        }
      }
      otaStopArduinoDevelopmentService(&developmentWindow_);
      developmentServiceStarted_ = false;
      endCriticalQueueGuard();
    } else if (phase == OtaDevelopmentWindowPhase::kSucceeded) {
      otaStopArduinoDevelopmentService(&developmentWindow_);
      developmentServiceStarted_ = false;
      if (developmentRebootPending_) {
        otaPhase_ = OtaRuntimePhase::kRebootPending;
        publishRuntimeActivity(monotonicMillis(), true);
        Serial.println("EVENT,ota,development_reboot_pending");
        acceptedRebootPending_ = true;
        if (prepareCriticalQueuesForOta(monotonicMillis())) {
          Serial.println("EVENT,ota,accepted_reboot_now");
          ESP.restart();
        }
      }
    } else if (phase == OtaDevelopmentWindowPhase::kClosed) {
      otaStopArduinoDevelopmentService(&developmentWindow_);
      developmentServiceStarted_ = false;
      endCriticalQueueGuard();
    }
  }

  bool isCurrentConfirmedProduction() const {
    return otaConfirmedProductionMatchesRunningImage(
        installState_, kM5goFirmwareVersion, kM5goBuildId);
  }

  const char* currentRunningProductionReleaseId() const {
    return isCurrentConfirmedProduction()
               ? installState_.confirmedReleaseId
               : nullptr;
  }

  OtaRuntimePhase effectiveOtaPhase() const {
    if (productionOtaActive_ || otaPhase_ == OtaRuntimePhase::kRebootPending ||
        otaPhase_ == OtaRuntimePhase::kValidating ||
        otaPhase_ == OtaRuntimePhase::kRunning) {
      return otaPhase_;
    }
    if (commandStep_ == CommandStep::kWaitingMain && hasActiveCommand_ &&
        activeCommand_.action == RemoteCommandAction::kOpenDevOta) {
      return OtaRuntimePhase::kAwaitingLocalConfirmation;
    }
    switch (developmentWindow_.phase()) {
      case OtaDevelopmentWindowPhase::kOpen:
        return OtaRuntimePhase::kDevelopmentWindowOpen;
      case OtaDevelopmentWindowPhase::kUploading:
        return OtaRuntimePhase::kDevelopmentUploading;
      case OtaDevelopmentWindowPhase::kFailed:
        return OtaRuntimePhase::kFailed;
      case OtaDevelopmentWindowPhase::kUnconfigured:
      case OtaDevelopmentWindowPhase::kClosed:
      case OtaDevelopmentWindowPhase::kSucceeded:
        return otaPhase_;
    }
    return otaPhase_;
  }

  void publishRuntimeActivity(uint64_t now, bool force = false) {
    if (workerOtaRuntimeMailbox == nullptr ||
        (!force && now < nextRuntimePublishMs_)) {
      return;
    }
    nextRuntimePublishMs_ = now + 1000;
    const OtaRuntimePhase phase = effectiveOtaPhase();
    OtaRuntimeSnapshot snapshot = {};
    snapshot.updatedAtMs = now;
    snapshot.confirmedReleaseCounter = installState_.confirmedReleaseCounter;
    snapshot.phase = phase;
    snapshot.error = otaRuntimeError_;
    snapshot.developmentConfigured = developmentOtaConfigured_;
    snapshot.productionTrusted = kCompiledOtaTrustKeyCount != 0;
    snapshot.installStateKnown = installStateKnown_;
    snapshot.installStateHealthy = installStateHealthy_;
    snapshot.productionPending = installState_.pending;
    snapshot.developmentPending = installState_.developmentPending;
    snapshot.confirmationPrepared =
        installStateHealthy_ && pendingImageRunning_ &&
        installState_.pendingValidated;
    snapshot.maximumMainLoopGapMs = lastSafetyMetrics_.maximumMainLoopGapMs;
    snapshot.invalidMicrophoneWindows =
        lastSafetyMetrics_.invalidMicrophoneWindows;
    snapshot.totalMicrophoneWindows = lastSafetyMetrics_.totalMicrophoneWindows;
    snapshot.consecutiveInvalidMicrophoneWindows =
        lastSafetyMetrics_.consecutiveInvalidMicrophoneWindows;
    snapshot.completedBytes = productionBytesWritten_;
    snapshot.totalBytes = productionImageSize_;
    if (developmentWindow_.phase() == OtaDevelopmentWindowPhase::kOpen) {
      snapshot.remainingMs = developmentWindow_.remainingMs(now);
    }
    if (developmentWindow_.phase() ==
        OtaDevelopmentWindowPhase::kUploading) {
      snapshot.completedBytes = developmentWindow_.completedBytes();
      snapshot.totalBytes = developmentWindow_.totalBytes();
    }
    if (WiFi.status() == WL_CONNECTED) {
      const String ip = WiFi.localIP().toString();
      std::snprintf(snapshot.localIp, sizeof(snapshot.localIp), "%s",
                    ip.c_str());
    }
    std::snprintf(snapshot.hostname, sizeof(snapshot.hostname), "%s",
                  developmentHostname_);
    std::snprintf(snapshot.releaseId, sizeof(snapshot.releaseId), "%s",
                  activeReleaseId_);
    workerOtaRuntimeMailbox->publishSnapshot(snapshot);

    const bool otaActive =
        phase == OtaRuntimePhase::kAwaitingLocalConfirmation ||
        phase == OtaRuntimePhase::kDevelopmentWindowOpen ||
        phase == OtaRuntimePhase::kDevelopmentUploading ||
        phase == OtaRuntimePhase::kProductionDownloading ||
        phase == OtaRuntimePhase::kProductionVerifying ||
        phase == OtaRuntimePhase::kRebootPending ||
        phase == OtaRuntimePhase::kValidating;
    const bool debugActive =
        remoteLogSession_.mode(now) == RemoteLogMode::kDetailed;
    if (workerHealthMailbox != nullptr) {
      workerHealthMailbox->publishRuntimeActivity(
          otaActive, otaRuntimePhaseName(phase), debugActive,
          debugActive ? "detailed" : "inactive");
    }
  }

  static const char* releaseReportPhaseName(ReleaseReportPhase phase) {
    switch (phase) {
      case ReleaseReportPhase::kDownloading:
        return "downloading";
      case ReleaseReportPhase::kVerifying:
        return "verifying";
      case ReleaseReportPhase::kRebootPending:
        return "reboot_pending";
      case ReleaseReportPhase::kValidating:
        return "validating";
      case ReleaseReportPhase::kRunning:
        return "running";
      case ReleaseReportPhase::kFailed:
        return "failed";
      case ReleaseReportPhase::kRejected:
        return "rejected";
      case ReleaseReportPhase::kRolledBack:
        return "rolled_back";
    }
    return "failed";
  }

  static const char* rollbackOutcomeName(ReleaseReportPhase phase) {
    switch (phase) {
      case ReleaseReportPhase::kRunning:
        return "not_needed";
      case ReleaseReportPhase::kRolledBack:
        return "succeeded";
      default:
        return "none";
    }
  }

  static bool nullableJsonString(const char* value, char* output,
                                 size_t outputCapacity) {
    if (output == nullptr || outputCapacity < 5) {
      return false;
    }
    if (value == nullptr || value[0] == '\0') {
      std::memcpy(output, "null", 5);
      return true;
    }
    for (const char* cursor = value; *cursor != '\0'; ++cursor) {
      const unsigned char byte = static_cast<unsigned char>(*cursor);
      if (byte < 0x20 || *cursor == '"' || *cursor == '\\') {
        return false;
      }
    }
    const int length = std::snprintf(output, outputCapacity, "\"%s\"", value);
    return length > 0 && static_cast<size_t>(length) < outputCapacity;
  }

  bool reportReleaseStatus(const char* desiredReleaseId,
                           ReleaseReportPhase phase, int progressPercent,
                           const char* lastError,
                           const char* previousReleaseId,
                           const char* runningReleaseId,
                           const char* lastKnownGoodReleaseId) {
    if (desiredReleaseId == nullptr || desiredReleaseId[0] == '\0' ||
        (progressPercent < -1 || progressPercent > 100)) {
      return false;
    }
    char desired[64] = {};
    char running[64] = {};
    char previous[64] = {};
    char lastGood[64] = {};
    char error[520] = {};
    const char* const wireRunningReleaseId =
        installState_.runningDevelopmentImage ? nullptr : runningReleaseId;
    if (!nullableJsonString(desiredReleaseId, desired, sizeof(desired)) ||
        !nullableJsonString(wireRunningReleaseId, running, sizeof(running)) ||
        !nullableJsonString(previousReleaseId, previous, sizeof(previous)) ||
        !nullableJsonString(lastKnownGoodReleaseId, lastGood,
                            sizeof(lastGood)) ||
        !nullableJsonString(lastError, error, sizeof(error))) {
      return false;
    }
    char progress[16] = {};
    if (progressPercent < 0) {
      std::memcpy(progress, "null", 5);
    } else {
      std::snprintf(progress, sizeof(progress), "%d", progressPercent);
    }
    const OtaReleaseStatusIdentity statusIdentity{
        workerIdentity.deviceId,
        desiredReleaseId,
        wireRunningReleaseId,
        previousReleaseId,
        lastKnownGoodReleaseId,
        releaseReportPhaseName(phase),
        progressPercent,
        lastError,
        rollbackOutcomeName(phase),
        kM5goFirmwareVersion,
        kM5goBuildId,
    };
    const uint64_t hash = otaReleaseStatusIdentityHash(statusIdentity);
    char statusId[40] = {};
    std::snprintf(statusId, sizeof(statusId), "status-%016llx",
                  static_cast<unsigned long long>(hash));
    const int payloadLength = std::snprintf(
        reinterpret_cast<char*>(releaseStatusPayloadBytes),
        sizeof(releaseStatusPayloadBytes),
        "{\"schema_version\":1,\"status_id\":\"%s\","
        "\"desired_release_id\":%s,\"running_release_id\":%s,"
        "\"previous_release_id\":%s,\"last_known_good_release_id\":%s,"
        "\"phase\":\"%s\",\"progress_percent\":%s,"
        "\"last_error\":%s,\"rollback_outcome\":\"%s\","
        "\"firmware_version\":\"%s\",\"build_id\":\"%s\"}",
        statusId, desired, running, previous, lastGood,
        releaseReportPhaseName(phase), progress, error,
        rollbackOutcomeName(phase), kM5goFirmwareVersion, kM5goBuildId);
    if (payloadLength <= 0 ||
        static_cast<size_t>(payloadLength) >= sizeof(releaseStatusPayloadBytes)) {
      return false;
    }

    char relative[160] = {};
    std::snprintf(relative, sizeof(relative),
                  "/v1/devices/%s/control/release-status",
                  workerIdentity.deviceId);
    char url[288] = {};
    if (!buildDeviceUrl(relative, url, sizeof(url))) {
      return false;
    }
    if (!beginBackendRequest(url, 5000)) {
      nextOtaReleaseAttemptMs_ = monotonicMillis() + kOtaReleaseRetryMs;
      return false;
    }
    HTTPClient& http = backendHttp_;
    http.addHeader("Content-Type", "application/json");
    http.setAuthorizationType("Bearer");
    http.setAuthorization(workerSettings.apiToken);
    const int status = http.sendRequest(
        "POST", releaseStatusPayloadBytes, static_cast<size_t>(payloadLength));
    String response;
    const bool bodyOk = status == HTTP_CODE_OK &&
                        readExactAckBody(http, response);
    endBackendRequest(bodyOk, status);
    if (!bodyOk) {
      nextOtaReleaseAttemptMs_ = monotonicMillis() + kOtaReleaseRetryMs;
      return false;
    }
    const OtaReleaseStatusAckResult parsed = parseOtaReleaseStatusAck(
        response.c_str(), response.length(), statusId);
    if (!parsed.ok()) {
      nextOtaReleaseAttemptMs_ = monotonicMillis() + kOtaReleaseRetryMs;
      Serial.printf("EVENT,ota,status_bad_ack,%s\n",
                    otaReleaseStatusAckErrorName(parsed.error));
      return false;
    }
    nextOtaReleaseAttemptMs_ = 0;
    return true;
  }

  bool downloadAuthenticatedBinary(const char* relativePath, uint8_t* output,
                                   size_t capacity, size_t* outputSize) {
    char url[384] = {};
    if (!buildDeviceUrl(relativePath, url, sizeof(url))) {
      return false;
    }
    if (!beginBackendRequest(url, 10000)) {
      return false;
    }
    HTTPClient& http = backendHttp_;
    http.addHeader("Accept", "application/octet-stream");
    http.setAuthorizationType("Bearer");
    http.setAuthorization(workerSettings.apiToken);
    const int status = http.GET();
    feedWatchdog();
    const bool ok = status == HTTP_CODE_OK &&
                    readHttpBodyToBuffer(http, output, capacity, outputSize);
    endBackendRequest(ok, status);
    return ok;
  }

  void failProductionRelease(const DesiredFirmwareRelease& desired,
                             const char* error,
                             OtaRuntimeError runtimeError,
                             bool rejected, bool staged) {
    endCriticalQueueGuard();
    productionOtaActive_ = false;
    productionBytesWritten_ = 0;
    productionImageSize_ = 0;
    otaRuntimeError_ = runtimeError;
    otaPhase_ = OtaRuntimePhase::kFailed;
    if (staged) {
      otaCancelPendingRelease(&installState_);
      if (saveOtaInstallState(installState_) !=
          OtaInstallStateStorageResult::kOk) {
        installStateHealthy_ = false;
        otaRuntimeError_ = OtaRuntimeError::kStorage;
      }
    }
    reportReleaseStatus(
        desired.releaseId,
        rejected ? ReleaseReportPhase::kRejected
                 : ReleaseReportPhase::kFailed,
        0, error, installState_.confirmedReleaseId,
        currentRunningProductionReleaseId(),
        installState_.confirmedReleaseId);
    nextOtaReleaseAttemptMs_ = monotonicMillis() + kOtaReleaseRetryMs;
    publishRuntimeActivity(monotonicMillis(), true);
    Serial.printf("EVENT,ota,production_failed,%s,%s\n", desired.releaseId,
                  error == nullptr ? "unknown" : error);
  }

  bool streamProductionImage(const DesiredFirmwareRelease& desired,
                             const OtaVerifiedRelease& release,
                             OtaApplicationImageIdentity* acceptedIdentity) {
    if (acceptedIdentity == nullptr) {
      return false;
    }
    *acceptedIdentity = {};
    char url[384] = {};
    if (!buildDeviceUrl(desired.imageUrl, url, sizeof(url))) {
      return false;
    }
    if (!beginBackendRequest(url, 10000)) {
      return false;
    }
    HTTPClient& http = backendHttp_;
    http.addHeader("Accept", "application/octet-stream");
    http.setAuthorizationType("Bearer");
    http.setAuthorization(workerSettings.apiToken);
    const int status = http.GET();
    feedWatchdog();
    if (!criticalQueuesSafeForOta() || status != HTTP_CODE_OK ||
        http.getSize() != static_cast<int>(release.manifest().firmwareSize)) {
      endBackendRequest(false, status);
      return false;
    }
    OtaStreamUpdater updater;
    if (!updater.begin(release, otaEsp32ApplicationUpdateBackend())) {
      endBackendRequest(false, status);
      return false;
    }
    productionImageSize_ = release.manifest().firmwareSize;
    productionBytesWritten_ = 0;
    WiFiClient* stream = http.getStreamPtr();
    if (stream == nullptr) {
      updater.abort();
      endBackendRequest(false, status);
      return false;
    }
    stream->setTimeout(1000);
    uint64_t lastProgressMs = monotonicMillis();
    uint16_t lastReportedPercent = 0;
    while (updater.bytesWritten() < updater.imageSize()) {
      feedWatchdog();
      if (consumeSafetyAbort() || !criticalQueuesSafeForOta()) {
        updater.abort();
        endBackendRequest(false, status);
        return false;
      }
      const int available = stream->available();
      if (available > 0) {
        const size_t remaining = updater.imageSize() - updater.bytesWritten();
        const size_t requested = otaSectorBoundedChunkSize(
            updater.bytesWritten(), static_cast<size_t>(available), remaining);
        const int read = stream->read(otaImageChunk, requested);
        if (read <= 0) {
          endBackendRequest(false, HTTPC_ERROR_CONNECTION_LOST);
          return false;
        }
        if (!updater.write(otaImageChunk, static_cast<size_t>(read))) {
          // The HTTP stream is still healthy; this is a local flash/update
          // failure and must not feed the Wi-Fi circuit breaker.
          endBackendRequest(false, status);
          return false;
        }
        lastProgressMs = monotonicMillis();
        productionBytesWritten_ = updater.bytesWritten();
        feedWatchdog();
        refreshUpdateSafetyMetrics();
        const uint16_t percent =
            static_cast<uint16_t>(updater.progressPermille() / 10U);
        if (percent >= lastReportedPercent + 10U || percent == 100U) {
          lastReportedPercent = percent;
          publishRuntimeActivity(lastProgressMs, true);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
        continue;
      }
      const uint64_t current = monotonicMillis();
      if (!http.connected() ||
          current - lastProgressMs >= kOtaImageInactivityTimeoutMs) {
        updater.abort();
        endBackendRequest(false, HTTPC_ERROR_READ_TIMEOUT);
        return false;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    endBackendRequest(true, status);
    if (consumeSafetyAbort() || !criticalQueuesSafeForOta()) {
      updater.abort();
      return false;
    }
    if (!updateSafetyMetricsAcceptable()) {
      safetyAcceptanceFailed_ = true;
      updater.abort();
      Serial.println("EVENT,ota,production_acceptance_rejected");
      return false;
    }
    feedWatchdog();
    if (!updater.finish()) {
      return false;
    }
    // esp_ota_end() validates but deliberately does not select the candidate.
    // Observe the final flash/validation stall before making it bootable.
    vTaskDelay(pdMS_TO_TICKS(50));
    feedWatchdog();
    const bool finalAccepted = !consumeSafetyAbort() &&
                               criticalQueuesSafeForOta() &&
                               updateSafetyMetricsAcceptable();
    if (!finalAccepted) {
      safetyAcceptanceFailed_ = true;
      Serial.println("EVENT,ota,production_finalize_rejected");
      return false;
    }
    if (!otaReadInactiveApplicationIdentity(acceptedIdentity) ||
        acceptedIdentity->partitionAddress !=
            installState_.pendingPartitionAddress) {
      Serial.println("EVENT,ota,production_identity_read_failed");
      return false;
    }
    return true;
  }

  void installDesiredRelease(const DesiredFirmwareRelease& desired,
                             uint64_t now) {
    if (!installStateHealthy_ || productionOtaActive_ ||
        pendingImageRunning_ || installState_.pending ||
        installState_.developmentPending || developmentServiceStarted_) {
      return;
    }
    if (installState_.confirmedReleaseCounter != 0 &&
        std::strcmp(desired.releaseId, installState_.confirmedReleaseId) == 0 &&
        isCurrentConfirmedProduction()) {
      std::snprintf(activeReleaseId_, sizeof(activeReleaseId_), "%s",
                    desired.releaseId);
      reportReleaseStatus(
          desired.releaseId, ReleaseReportPhase::kRunning, 100, nullptr,
          installState_.previousReleaseId,
          currentRunningProductionReleaseId(),
          installState_.confirmedReleaseId);
      return;
    }
    if (kCompiledOtaTrustKeyCount == 0) {
      failProductionRelease(desired, "production_trust_unavailable",
                            OtaRuntimeError::kProductionTrustUnavailable, true,
                            false);
      return;
    }
    if (desired.releaseCounter <= installState_.confirmedReleaseCounter) {
      failProductionRelease(desired, "release_counter_not_newer",
                            OtaRuntimeError::kManifestRejected, true, false);
      return;
    }
    std::snprintf(activeReleaseId_, sizeof(activeReleaseId_), "%s",
                  desired.releaseId);

    if (!prepareCriticalQueuesForOta(now)) {
      failProductionRelease(desired, "critical_queue_backpressure",
                            OtaRuntimeError::kImageRejected, false, false);
      return;
    }

    resetUpdateSafetyMetrics();
    armCriticalQueueGuard();

    productionOtaActive_ = true;
    otaPhase_ = OtaRuntimePhase::kProductionDownloading;
    otaRuntimeError_ = OtaRuntimeError::kNone;
    productionBytesWritten_ = 0;
    productionImageSize_ = desired.imageSize;
    publishRuntimeActivity(now, true);
    if (!reportReleaseStatus(
            desired.releaseId, ReleaseReportPhase::kDownloading, 0, nullptr,
            installState_.confirmedReleaseId,
            currentRunningProductionReleaseId(),
            installState_.confirmedReleaseId)) {
      endCriticalQueueGuard();
      productionOtaActive_ = false;
      otaRuntimeError_ = OtaRuntimeError::kNetwork;
      otaPhase_ = OtaRuntimePhase::kFailed;
      return;
    }
    feedWatchdog();
    if (!criticalQueuesSafeForOta()) {
      failProductionRelease(desired, "critical_queue_backpressure",
                            OtaRuntimeError::kImageRejected, false, false);
      return;
    }

    size_t manifestBundleSize = 0;
    if (!downloadAuthenticatedBinary(
            desired.manifestUrl, manifestBundleBytes,
            sizeof(manifestBundleBytes), &manifestBundleSize) ||
        manifestBundleSize <= kOtaP256SignatureSize) {
      failProductionRelease(desired, "manifest_download_failed",
                            OtaRuntimeError::kNetwork, false, false);
      return;
    }
    if (!criticalQueuesSafeForOta()) {
      failProductionRelease(desired, "critical_queue_backpressure",
                            OtaRuntimeError::kImageRejected, false, false);
      return;
    }
    feedWatchdog();
    const size_t manifestSize =
        manifestBundleSize - kOtaP256SignatureSize;
    const uint32_t partitionSize = otaInactiveApplicationPartitionSize();
    const uint32_t partitionAddress =
        otaInactiveApplicationPartitionAddress();
    const OtaReleaseValidationResult validated = validateOtaRelease(
        manifestBundleBytes, manifestSize,
        manifestBundleBytes + manifestSize, kOtaP256SignatureSize,
        kCompiledOtaTrustKeys, kCompiledOtaTrustKeyCount,
        "m5go-classic-esp32-16m", installState_.confirmedReleaseCounter,
        partitionSize);
    if (!validated.ok()) {
      failProductionRelease(desired,
                            otaReleaseValidationErrorName(validated.error),
                            OtaRuntimeError::kManifestRejected, true, false);
      return;
    }
    const OtaControlValidationError claims =
        validateOtaControlClaims(desired, validated.release.manifest());
    if (claims != OtaControlValidationError::kNone) {
      failProductionRelease(desired,
                            otaControlValidationErrorName(claims),
                            OtaRuntimeError::kManifestRejected, true, false);
      return;
    }

    otaPhase_ = OtaRuntimePhase::kProductionVerifying;
    publishRuntimeActivity(monotonicMillis(), true);
    if (!reportReleaseStatus(
            desired.releaseId, ReleaseReportPhase::kVerifying, 0, nullptr,
            installState_.confirmedReleaseId,
            currentRunningProductionReleaseId(),
            installState_.confirmedReleaseId)) {
      endCriticalQueueGuard();
      productionOtaActive_ = false;
      otaRuntimeError_ = OtaRuntimeError::kNetwork;
      otaPhase_ = OtaRuntimePhase::kFailed;
      return;
    }
    feedWatchdog();
    if (!otaStagePendingRelease(&installState_, desired.releaseId,
                                validated.release.manifest(),
                                partitionAddress)) {
      installStateHealthy_ = false;
      failProductionRelease(desired, "install_state_write_failed",
                            OtaRuntimeError::kStorage, false, false);
      return;
    }
    if (saveOtaInstallState(installState_) !=
        OtaInstallStateStorageResult::kOk) {
      // The staged record was never made durable, so clear the in-memory copy
      // before refusing further production updates for this boot.
      otaCancelPendingRelease(&installState_);
      installStateHealthy_ = false;
      failProductionRelease(desired, "install_state_write_failed",
                            OtaRuntimeError::kStorage, false, false);
      return;
    }
    feedWatchdog();

    // Requests above are bounded but can still enqueue transitions while the
    // sole worker is occupied. Durably drain them before the long image stream
    // and take a fresh no-drop baseline.
    if (!prepareCriticalQueuesForOta(monotonicMillis())) {
      failProductionRelease(desired, "critical_queue_backpressure",
                            OtaRuntimeError::kImageRejected, false, true);
      return;
    }
    armCriticalQueueGuard();

    otaPhase_ = OtaRuntimePhase::kProductionDownloading;
    publishRuntimeActivity(monotonicMillis(), true);
    OtaApplicationImageIdentity acceptedIdentity = {};
    if (!streamProductionImage(desired, validated.release,
                               &acceptedIdentity)) {
      failProductionRelease(
          desired,
          safetyAbortLatched_
              ? "safety_abort"
              : safetyAcceptanceFailed_ ? "acceptance_metrics_rejected"
                                        : "image_rejected",
          OtaRuntimeError::kImageRejected, false, true);
      safetyAbortLatched_ = false;
      return;
    }

    OtaInstallState acceptedState = installState_;
    if (!otaMarkPendingImageAccepted(
            &acceptedState, acceptedIdentity.partitionAddress,
            acceptedIdentity.sha256) ||
        saveOtaInstallState(acceptedState) !=
            OtaInstallStateStorageResult::kOk) {
      // The raw production backend has not selected the candidate yet. Keep
      // the durable unaccepted transaction so a reset can reconcile it, and
      // never clear the record on an ambiguous storage failure.
      installStateHealthy_ = false;
      failProductionRelease(desired, "install_state_write_failed",
                            OtaRuntimeError::kStorage, false, false);
      return;
    }
    installState_ = acceptedState;

    // Include the accepted-journal NVS commit in the measured safety
    // contract while the known-good running slot is still selected. Give Core
    // 1 time to publish the resulting loop-gap/microphone window before the
    // final boot-selection handoff.
    vTaskDelay(pdMS_TO_TICKS(50));
    feedWatchdog();
    const bool terminalAccepted =
        !consumeSafetyAbort() && criticalQueuesSafeForOta() &&
        updateSafetyMetricsAcceptable();
    if (!terminalAccepted) {
      safetyAcceptanceFailed_ = true;
      failProductionRelease(desired, "acceptance_metrics_rejected",
                            OtaRuntimeError::kImageRejected, false, true);
      safetyAbortLatched_ = false;
      return;
    }

    // This is deliberately the final flash mutation. Re-selecting the running
    // slot after a rejected handoff would mark that known-good application NEW
    // under ESP-IDF rollback semantics. All rejectable safety gates therefore
    // complete before selecting the exact accepted candidate.
    if (!otaSelectAcceptedApplicationBootPartition(acceptedIdentity)) {
      // accepted=true is already durable. Preserve it even if selection or
      // readback is ambiguous; the next boot compares exact running identity.
      installStateHealthy_ = false;
      failProductionRelease(desired, "boot_partition_select_failed",
                            OtaRuntimeError::kStorage, false, false);
      return;
    }

    productionBytesWritten_ = productionImageSize_;
    otaPhase_ = OtaRuntimePhase::kRebootPending;
    otaRuntimeError_ = OtaRuntimeError::kNone;
    publishRuntimeActivity(monotonicMillis(), true);
    reportReleaseStatus(
        desired.releaseId, ReleaseReportPhase::kRebootPending, 100, nullptr,
        installState_.confirmedReleaseId, currentRunningProductionReleaseId(),
        installState_.confirmedReleaseId);
    Serial.printf("EVENT,ota,reboot_pending,%s\n", desired.releaseId);
    acceptedRebootPending_ = true;
    if (prepareCriticalQueuesForOta(monotonicMillis())) {
      Serial.println("EVENT,ota,accepted_reboot_now");
      ESP.restart();
    }
  }

  static uint64_t ageSince(uint64_t now, uint64_t timestamp) {
    return timestamp == 0 ? UINT64_MAX
                          : (now >= timestamp ? now - timestamp : 0);
  }

  void loadFilesystemCounts() {
    spoolCount_ = countFiles(kSpoolDirectory, nullptr, 0);
    feedbackWaitCount_ = countFiles(kFeedbackWaitDirectory, nullptr, 0);
    feedbackReadyCount_ = countFiles(kFeedbackReadyDirectory, nullptr, 0);
    deadCount_ = countFiles(kDeadDirectory, nullptr, 0);
    Serial.printf("EVENT,uploader,spool_ready,%u,%u,%u,%u\n",
                  static_cast<unsigned>(spoolCount_),
                  static_cast<unsigned>(feedbackWaitCount_),
                  static_cast<unsigned>(feedbackReadyCount_),
                  static_cast<unsigned>(deadCount_));
  }

  void publishWorkerHealth(uint64_t now) {
    if (workerHealthMailbox == nullptr) {
      return;
    }
    const bool hasBacklog = spoolCount_ != 0 || feedbackWaitCount_ != 0 ||
                            feedbackReadyCount_ != 0;
    if (hasBacklog && backlogSinceMs_ == 0) {
      backlogSinceMs_ = now;
    } else if (!hasBacklog) {
      backlogSinceMs_ = 0;
    }

    DeviceHealthWorkerUpdate update = {};
    update.uptimeMs = now;
    update.desiredConfigRevision = desiredConfigRevision_;
    update.lastTelemetryAckAgeMs = ageSince(now, lastTelemetryAckMs_);
    update.lastConfigAttemptAgeMs = ageSince(now, lastConfigAttemptMs_);
    update.lastRoomFetchAgeMs = ageSince(now, lastEnvironmentAttemptMs_);
    update.lastWeatherFetchAgeMs = ageSince(now, lastWeatherAttemptMs_);
    update.uploaderHeartbeatAgeMs = 0;
    update.oldestBacklogAgeMs =
        backlogSinceMs_ == 0 ? 0 : ageSince(now, backlogSinceMs_);
    update.wifiReconnectCount = wifiReconnectCount_;
    update.wifiConnected = WiFi.status() == WL_CONNECTED;
    if (update.wifiConnected) {
      const String ip = WiFi.localIP().toString();
      std::snprintf(update.localIp, sizeof(update.localIp), "%s", ip.c_str());
      update.wifiRssiDbm =
          std::max<int32_t>(-127, std::min<int32_t>(0, WiFi.RSSI()));
    }
    update.clockSynchronized = hasClockAnchor_;
    update.uploaderStackHighWaterBytes =
        static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr)) *
        sizeof(StackType_t);
    update.spoolFiles = spoolCount_;
    update.feedbackWaitFiles = feedbackWaitCount_;
    update.feedbackReadyFiles = feedbackReadyCount_;
    update.deadFiles = deadCount_;
    update.filesystemReady = filesystemReady_;
    if (filesystemReady_) {
      update.littlefsTotalBytes = LittleFS.totalBytes();
      update.littlefsUsedBytes = LittleFS.usedBytes();
    }
    update.uploaderStatus =
        operatorHalted_
            ? UploaderHealthStatus::kOperatorHalted
            : !filesystemReady_
                  ? UploaderHealthStatus::kFilesystemUnavailable
                  : retryExponent_ != 0
                        ? UploaderHealthStatus::kRetrying
                        : UploaderHealthStatus::kReady;
    update.telemetryAckResult = telemetryAckResult_;
    update.configResult = configResult_;
    update.roomFetchResult = environmentFetchResult_;
    update.weatherFetchResult = weatherFetchResult_;
    workerHealthMailbox->publishWorker(update);
  }

  void uploadHealthIfNeeded(uint64_t now) {
    if (!workerSettings.configured || workerHealthMailbox == nullptr ||
        now < nextHealthAttemptMs_) {
      return;
    }
    DeviceHealthSnapshot snapshot = {};
    if (!workerHealthMailbox->copySnapshot(&snapshot) ||
        !snapshot.initialized) {
      return;
    }
    const bool periodicDue = lastHealthSuccessMs_ == 0 ||
                             now - lastHealthSuccessMs_ >= kHealthIntervalMs;
    const bool levelChanged = !hasReportedHealthLevel_ ||
                              snapshot.level != lastReportedHealthLevel_;
    if (!periodicDue && !levelChanged) {
      return;
    }

    snapshot.sequence = nextHealthSequence_++;
    FixedBufferSink payload{healthPayloadBytes, sizeof(healthPayloadBytes), 0};
    const HealthJsonSink sink{&payload, bufferSink};
    if (!writeDeviceHealthJson(snapshot, sink)) {
      nextHealthAttemptMs_ = now + kHealthRetryMs;
      Serial.println("EVENT,health,serialize_failed");
      return;
    }

    char url[224] = {};
    const int urlLength =
        std::snprintf(url, sizeof(url), "%s/v1/devices/%s/health",
                      workerSettings.serverBaseUrl, workerIdentity.deviceId);
    if (urlLength <= 0 || static_cast<size_t>(urlLength) >= sizeof(url) ||
        std::strncmp(url, "http://", 7) != 0) {
      nextHealthAttemptMs_ = now + kHealthRetryMs;
      Serial.println("EVENT,health,unsupported_server_url");
      return;
    }

    if (!beginBackendRequest(url, 5000)) {
      nextHealthAttemptMs_ = now + kHealthRetryMs;
      return;
    }
    HTTPClient& http = backendHttp_;
    http.addHeader("Content-Type", "application/json");
    http.setAuthorizationType("Bearer");
    http.setAuthorization(workerSettings.apiToken);
    const int status = http.sendRequest("POST", healthPayloadBytes,
                                        payload.size);
    String response;
    const bool bodyOk = status == HTTP_CODE_OK &&
                        readExactAckBody(http, response);
    endBackendRequest(bodyOk, status);
    if (bodyOk) {
      lastHealthSuccessMs_ = monotonicMillis();
      // Periodicity is derived from lastHealthSuccessMs_. Keep the retry gate
      // open so a subsequent local level change is reported immediately.
      nextHealthAttemptMs_ = 0;
      lastReportedHealthLevel_ = snapshot.level;
      hasReportedHealthLevel_ = true;
      Serial.printf("EVENT,health,acked,%llu,%s\n",
                    static_cast<unsigned long long>(snapshot.sequence),
                    deviceHealthLevelWireName(snapshot.level));
      return;
    }
    nextHealthAttemptMs_ = monotonicMillis() + kHealthRetryMs;
    Serial.printf("EVENT,health,retry,%d\n", status);
  }

  static bool isExplicitHttpUrl(const char* url, size_t capacity) {
    if (url == nullptr || capacity < 8 ||
        std::memchr(url, '\0', capacity) == nullptr) {
      return false;
    }
    return std::strncmp(url, "http://", 7) == 0 && url[7] != '\0';
  }

  static const char* dashboardFetchResultName(DashboardFetchResult result) {
    switch (result) {
      case DashboardFetchResult::kOk:
        return "ok";
      case DashboardFetchResult::kInvalidUrl:
        return "url";
      case DashboardFetchResult::kBeginFailed:
        return "begin";
      case DashboardFetchResult::kHttpStatus:
        return "http";
      case DashboardFetchResult::kTooLarge:
        return "too_large";
      case DashboardFetchResult::kTransferFailed:
        return "transfer";
      case DashboardFetchResult::kTruncated:
        return "truncated";
    }
    return "unknown";
  }

  static DashboardHttpResponse fetchDashboardJson(const char* url,
                                                  size_t urlCapacity,
                                                  size_t bodyCapacity) {
    DashboardHttpResponse response = {};
    if (!isExplicitHttpUrl(url, urlCapacity) || bodyCapacity == 0 ||
        bodyCapacity > kMaximumWeatherResponseBytes) {
      response.result = DashboardFetchResult::kInvalidUrl;
      return response;
    }

    dashboardResponseBytes[0] = '\0';
    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(3000);
    http.setTimeout(5000);
    http.setReuse(false);
    // Keep HTTP/1.1 enabled: Open-Meteo may use chunked transfer encoding,
    // which writeToStream validates through the terminating zero-size chunk.
    http.useHTTP10(false);
    if (!http.begin(client, url)) {
      response.result = DashboardFetchResult::kBeginFailed;
      return response;
    }
    http.addHeader("Accept", "application/json");
    response.httpStatus = http.GET();
    if (response.httpStatus != HTTP_CODE_OK) {
      response.result = DashboardFetchResult::kHttpStatus;
      http.end();
      return response;
    }

    const int declaredSize = http.getSize();
    if (declaredSize > static_cast<int>(bodyCapacity)) {
      response.result = DashboardFetchResult::kTooLarge;
      http.end();
      return response;
    }

    FixedCapacityStream sink(
        reinterpret_cast<uint8_t*>(dashboardResponseBytes), bodyCapacity);
    response.transferResult = http.writeToStream(&sink);
    const int completedSize = http.getSize();
    http.end();
    response.bodySize = sink.size();
    if (sink.overflowed() || response.bodySize > bodyCapacity) {
      response.result = DashboardFetchResult::kTooLarge;
      return response;
    }
    if (response.transferResult < 0) {
      response.result = DashboardFetchResult::kTransferFailed;
      return response;
    }
    if (response.bodySize != static_cast<size_t>(response.transferResult) ||
        (declaredSize >= 0 && response.transferResult != declaredSize) ||
        (completedSize >= 0 && response.transferResult != completedSize)) {
      response.result = DashboardFetchResult::kTruncated;
      return response;
    }

    dashboardResponseBytes[response.bodySize] = '\0';
    response.result = DashboardFetchResult::kOk;
    return response;
  }

  static void logDashboardHttpFailure(
      const char* source, const DashboardHttpResponse& response) {
    if (response.result == DashboardFetchResult::kHttpStatus) {
      Serial.printf("EVENT,dashboard,%s,fail,http,%d\n", source,
                    response.httpStatus);
      return;
    }
    if (response.result == DashboardFetchResult::kTransferFailed) {
      Serial.printf("EVENT,dashboard,%s,fail,transfer,%d\n", source,
                    response.transferResult);
      return;
    }
    Serial.printf("EVENT,dashboard,%s,fail,%s\n", source,
                  dashboardFetchResultName(response.result));
  }

  void fetchEnvironment(uint64_t attemptMs) {
    lastEnvironmentAttemptMs_ = attemptMs;
    environmentFetchResult_ = HealthOperationResult::kRetrying;
    const DashboardHttpResponse response = fetchDashboardJson(
        workerSettings.environmentUrl, sizeof(workerSettings.environmentUrl),
        kMaximumEnvironmentResponseBytes);
    const uint64_t completedMs = monotonicMillis();
    if (response.result != DashboardFetchResult::kOk) {
      workerDashboardMailbox->recordEnvironmentFailure(attemptMs);
      nextEnvironmentFetchMs_ = completedMs + kEnvironmentRetryMs;
      logDashboardHttpFailure("environment", response);
      return;
    }

    const EnvironmentParseResult parsed = parseEnvironmentReading(
        dashboardResponseBytes, response.bodySize);
    if (!parsed.ok()) {
      workerDashboardMailbox->recordEnvironmentFailure(attemptMs);
      nextEnvironmentFetchMs_ = completedMs + kEnvironmentRetryMs;
      Serial.printf("EVENT,dashboard,environment,fail,parse,%s\n",
                    dashboardParseErrorName(parsed.error));
      environmentFetchResult_ = HealthOperationResult::kError;
      return;
    }
    if (!workerDashboardMailbox->publishEnvironment(
            parsed.reading, completedMs, attemptMs)) {
      nextEnvironmentFetchMs_ = completedMs + kEnvironmentRetryMs;
      Serial.println("EVENT,dashboard,environment,fail,publish");
      environmentFetchResult_ = HealthOperationResult::kError;
      return;
    }

    const uint64_t interval = std::max<uint64_t>(
        workerSettings.environmentPollIntervalMs,
        kMinimumEnvironmentPollIntervalMs);
    nextEnvironmentFetchMs_ = completedMs + interval;
    Serial.printf("EVENT,dashboard,environment,ok,%u\n",
                  static_cast<unsigned>(response.bodySize));
    environmentFetchResult_ = HealthOperationResult::kOk;
  }

  void fetchWeather(uint64_t attemptMs) {
    lastWeatherAttemptMs_ = attemptMs;
    weatherFetchResult_ = HealthOperationResult::kRetrying;
    const DashboardHttpResponse response = fetchDashboardJson(
        workerSettings.weatherUrl, sizeof(workerSettings.weatherUrl),
        kMaximumWeatherResponseBytes);
    const uint64_t completedMs = monotonicMillis();
    if (response.result != DashboardFetchResult::kOk) {
      workerDashboardMailbox->recordWeatherFailure(attemptMs);
      nextWeatherFetchMs_ = completedMs + kWeatherRetryMs;
      logDashboardHttpFailure("weather", response);
      return;
    }

    const WeatherParseResult parsed =
        parseWeatherReading(dashboardResponseBytes, response.bodySize);
    if (!parsed.ok()) {
      workerDashboardMailbox->recordWeatherFailure(attemptMs);
      nextWeatherFetchMs_ = completedMs + kWeatherRetryMs;
      Serial.printf("EVENT,dashboard,weather,fail,parse,%s\n",
                    dashboardParseErrorName(parsed.error));
      weatherFetchResult_ = HealthOperationResult::kError;
      return;
    }
    if (!workerDashboardMailbox->publishWeather(parsed.reading, completedMs,
                                                attemptMs)) {
      nextWeatherFetchMs_ = completedMs + kWeatherRetryMs;
      Serial.println("EVENT,dashboard,weather,fail,publish");
      weatherFetchResult_ = HealthOperationResult::kError;
      return;
    }

    const uint64_t interval = std::max<uint64_t>(
        workerSettings.weatherPollIntervalMs,
        kMinimumWeatherPollIntervalMs);
    nextWeatherFetchMs_ = completedMs + interval;
    Serial.printf("EVENT,dashboard,weather,ok,%u\n",
                  static_cast<unsigned>(response.bodySize));
    weatherFetchResult_ = HealthOperationResult::kOk;
  }

  bool fetchOneDashboardSource(uint64_t now) {
    if (workerDashboardMailbox == nullptr) {
      return false;
    }
    if (workerSettings.environmentEnabled &&
        now >= nextEnvironmentFetchMs_) {
      environmentFetchAttempted_ = true;
      fetchEnvironment(now);
      return true;
    }
    if (workerSettings.weatherEnabled && now >= nextWeatherFetchMs_) {
      weatherFetchAttempted_ = true;
      fetchWeather(now);
      return true;
    }
    return false;
  }

  bool dashboardSourceMustPreempt(uint64_t now) const {
    if (workerSettings.environmentEnabled) {
      if (!environmentFetchAttempted_) {
        return true;
      }
      if (now >= nextEnvironmentFetchMs_ &&
          now - nextEnvironmentFetchMs_ >= kMaximumDashboardDeferralMs) {
        return true;
      }
    }
    if (workerSettings.weatherEnabled) {
      if (!weatherFetchAttempted_) {
        return true;
      }
      if (now >= nextWeatherFetchMs_ &&
          now - nextWeatherFetchMs_ >= kMaximumDashboardDeferralMs) {
        return true;
      }
    }
    return false;
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

  bool quarantineOneInvalidTelemetrySpool() {
    File folder = LittleFS.open(kSpoolDirectory);
    if (!folder || !folder.isDirectory()) {
      return false;
    }
    char invalidPath[128] = {};
    File entry = folder.openNextFile();
    while (entry) {
      if (!entry.isDirectory()) {
        SpoolFileMetadata metadata = {};
        if (!parseSpoolFileMetadata(entry.path(), metadata)) {
          std::snprintf(invalidPath, sizeof(invalidPath), "%s", entry.path());
          entry.close();
          break;
        }
      }
      entry.close();
      entry = folder.openNextFile();
    }
    folder.close();
    if (invalidPath[0] == '\0') {
      return false;
    }

    while (deadCount_ >= kMaxDeadFiles) {
      char oldest[128] = {};
      deadCount_ = countFiles(kDeadDirectory, oldest, sizeof(oldest));
      if (deadCount_ < kMaxDeadFiles) {
        break;
      }
      if (oldest[0] == '\0' || !LittleFS.remove(oldest)) {
        Serial.printf("EVENT,uploader,quarantine_blocked,%s\n", invalidPath);
        return false;
      }
      --deadCount_;
    }

    const char* filename = std::strrchr(invalidPath, '/');
    filename = filename == nullptr ? invalidPath : filename + 1;
    char deadPath[160] = {};
    const int length = std::snprintf(deadPath, sizeof(deadPath),
                                     "/dead/corrupt_%s", filename);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(deadPath) ||
        (LittleFS.exists(deadPath) && !LittleFS.remove(deadPath)) ||
        !LittleFS.rename(invalidPath, deadPath)) {
      Serial.printf("EVENT,uploader,quarantine_failed,%s\n", invalidPath);
      return false;
    }
    if (spoolCount_ != 0) {
      --spoolCount_;
    }
    ++deadCount_;
    Serial.printf("EVENT,uploader,quarantined,%s\n", invalidPath);
    return true;
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

  void freezeQueueIfNeeded(uint64_t now, bool force = false) {
    if (workerQueue == nullptr) {
      return;
    }
    const bool criticalWaiting = workerQueue->hasCritical();
    const size_t totalBytes = LittleFS.totalBytes();
    const size_t usedBytes = LittleFS.usedBytes();
    const size_t freeBytes = totalBytes >= usedBytes ? totalBytes - usedBytes : 0;
    if (!mayFreezeTelemetry(spoolCount_, freeBytes, criticalWaiting)) {
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
    const bool drainingCriticalReserve = shouldDrainCriticalReserve(
        spoolCount_, freeBytes, criticalWaiting);
    const size_t requestedBatchSize =
        drainingCriticalReserve
            ? kMaxFirmwareBatchSize
            : std::max<size_t>(
                  1, std::min<size_t>(activeConfig_.uploadBatchSize,
                                      kMaxFirmwareBatchSize));
    copiedCount = std::min(copiedCount, requestedBatchSize);

    const size_t recordCount =
        contiguousRevisionPrefix(records, copiedCount);
    const bool revisionBoundary = recordCount < copiedCount;
    if (!force && recordCount < requestedBatchSize && !revisionBoundary &&
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
    context.firmwareVersion = kM5goFirmwareVersion;
    context.buildId = kM5goBuildId;
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
    context.firmwareVersion = kM5goFirmwareVersion;
    context.buildId = kM5goBuildId;
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
      if (wifiReconnectCount_ != UINT32_MAX) {
        ++wifiReconnectCount_;
      }
      WiFi.begin(workerSettings.wifiSsid, workerSettings.wifiPassword);
      Serial.println("EVENT,wifi,connecting");
    }
  }

  void captureClockAnchor() {
    if (hasClockAnchor_ || WiFi.status() != WL_CONNECTED) {
      return;
    }
    if (!sntpStarted_) {
      configTzTime(kDashboardEasternTimeZone, "pool.ntp.org",
                   "time.cloudflare.com");
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
    if (workerDashboardMailbox != nullptr) {
      workerDashboardMailbox->markClockSynchronized();
    }
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
    lastConfigAttemptMs_ = now;
    configResult_ = HealthOperationResult::kRetrying;
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
      configResult_ = HealthOperationResult::kRejected;
      Serial.println("EVENT,config,unsupported_server_url");
      return;
    }

    if (!beginBackendRequest(url, 5000)) {
      nextConfigPollMs_ = now + kConfigRetryMs;
      return;
    }
    HTTPClient& http = backendHttp_;
    http.setAuthorizationType("Bearer");
    http.setAuthorization(workerSettings.apiToken);
    const int status = http.GET();
    if (status == HTTP_CODE_UNAUTHORIZED || status == HTTP_CODE_NOT_FOUND) {
      endBackendRequest(false, status);
      operatorHalted_ = true;
      configResult_ = HealthOperationResult::kRejected;
      Serial.printf("EVENT,config,operator_halt,%d\n", status);
      return;
    }
    if (status != HTTP_CODE_OK) {
      endBackendRequest(false, status);
      nextConfigPollMs_ = now + kConfigRetryMs;
      Serial.printf("EVENT,config,retry,%d\n", status);
      return;
    }

    const int responseSize = http.getSize();
    if (responseSize < 0 ||
        responseSize > static_cast<int>(kMaximumConfigResponseBytes)) {
      endBackendRequest(false, status);
      nextConfigPollMs_ = now + kConfigRetryMs;
      Serial.println("EVENT,config,retry_bad_response,response_size");
      return;
    }
    const String response = http.getString();
    const bool bodyOk =
        response.length() == static_cast<size_t>(responseSize);
    endBackendRequest(bodyOk, status);
    if (!bodyOk) {
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
    desiredConfigRevision_ =
        std::max<uint64_t>(desiredConfigRevision_, parsed.config.revision);
    if (parsed.config.revision < activeConfig_.revision) {
      Serial.printf("EVENT,config,server_rollback,%llu,%llu\n",
                    static_cast<unsigned long long>(activeConfig_.revision),
                    static_cast<unsigned long long>(parsed.config.revision));
      // The device has durably applied a revision that this server no longer
      // recognizes. Continuing would turn each valid queued batch into a 409
      // and eventually evict it from the bounded diagnostic queue.
      operatorHalted_ = true;
      configResult_ = HealthOperationResult::kRejected;
      return;
    }
    if (parsed.config.revision == activeConfig_.revision) {
      if (!deviceConfigsEqual(parsed.config, activeConfig_)) {
        Serial.printf("EVENT,config,same_revision_conflict,%llu\n",
                      static_cast<unsigned long long>(activeConfig_.revision));
        operatorHalted_ = true;
        configResult_ = HealthOperationResult::kRejected;
        return;
      }
      markConflictConfigValidated();
      nextConfigPollMs_ = now + kConfigPollIntervalMs;
      configResult_ = HealthOperationResult::kOk;
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
    configResult_ = HealthOperationResult::kOk;
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
    desiredConfigRevision_ =
        std::max<uint64_t>(desiredConfigRevision_, desiredRevision);
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
    telemetryAckResult_ = HealthOperationResult::kRetrying;

    File envelope = LittleFS.open(metadata.path, FILE_READ);
    if (!envelope) {
      scheduleBackoff(now);
      return true;
    }
    const size_t envelopeSize = envelope.size();
    envelope.close();
    if (envelopeSize == 0) {
      // A zero-byte immutable envelope can never become uploadable. Preserve
      // it for diagnosis without letting it block every later spool forever.
      if (moveToDeadLetter(metadata, 0)) {
        telemetryAckResult_ = HealthOperationResult::kRejected;
        clearTelemetryConflictProbe(metadata.batchId);
        Serial.printf("EVENT,uploader,dead_letter_empty,%s\n",
                      metadata.batchId);
        resetBackoff(now);
      } else {
        Serial.printf("EVENT,uploader,dead_letter_empty_failed,%s\n",
                      metadata.batchId);
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
      telemetryAckResult_ = HealthOperationResult::kRejected;
      Serial.println("EVENT,uploader,unsupported_server_url");
      return true;
    }

    // Unlike the bounded RAM requests above, this request alternates LittleFS
    // reads with socket writes. Its dedicated keep-alive session isolates a
    // transient lwIP send stall without imposing a fixed backlog drain rate.
    if (!beginTelemetryRequest(url, 5000)) {
      scheduleBackoff(now);
      return true;
    }
    HTTPClient& http = telemetryHttp_;
    http.addHeader("Content-Type", "application/json");
    http.setAuthorizationType("Bearer");
    http.setAuthorization(workerSettings.apiToken);
    ReopeningLittleFsReadStream body(metadata.path, envelopeSize);
    const int status = http.sendRequest("POST", &body, envelopeSize);

    if (status == HTTP_CODE_OK && body.complete()) {
      String response;
      if (!readExactAckBody(http, response)) {
        endTelemetryRequest(false, status);
        Serial.printf("EVENT,uploader,retry_bad_ack,%s,bounded_read\n",
                      metadata.batchId);
        scheduleBackoff(now);
        return true;
      }
      endTelemetryRequest(true, status);
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
      lastTelemetryAckMs_ = monotonicMillis();
      telemetryAckResult_ = HealthOperationResult::kOk;
      resetBackoff(now);
      return true;
    }

    endTelemetryRequest(false, status);
    if (status == HTTP_CODE_UNAUTHORIZED || status == HTTP_CODE_NOT_FOUND) {
      Serial.printf("EVENT,uploader,operator_halt,%d,%s\n", status,
                    metadata.batchId);
      operatorHalted_ = true;
      telemetryAckResult_ = HealthOperationResult::kRejected;
      return true;
    }
    if (status == kHttpConflict) {
      if (!telemetryConflictIsConfirmed(metadata.batchId)) {
        beginTelemetryConflictProbe(now, metadata.batchId);
        return true;
      }
      if (moveToDeadLetter(metadata, status)) {
        telemetryAckResult_ = HealthOperationResult::kRejected;
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
        telemetryAckResult_ = HealthOperationResult::kRejected;
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
    telemetryAckResult_ = HealthOperationResult::kRetrying;

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
      telemetryAckResult_ = HealthOperationResult::kRejected;
      Serial.println("EVENT,feedback,unsupported_server_url");
      return true;
    }

    if (!beginBackendRequest(url, 5000)) {
      scheduleBackoff(now);
      return true;
    }
    HTTPClient& http = backendHttp_;
    http.addHeader("Content-Type", "application/json");
    http.setAuthorizationType("Bearer");
    http.setAuthorization(workerSettings.apiToken);
    const int status = http.sendRequest(
        "POST", bundle + slices.telemetryOffset, slices.telemetryLength);

    if (status == HTTP_CODE_OK) {
      String response;
      if (!readExactAckBody(http, response)) {
        endBackendRequest(false, status);
        Serial.printf("EVENT,feedback,wait_retry_bad_ack,%s,bounded_read\n",
                      metadata.telemetryBatchId);
        scheduleBackoff(now);
        return true;
      }
      endBackendRequest(true, status);
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
      lastTelemetryAckMs_ = monotonicMillis();
      telemetryAckResult_ = HealthOperationResult::kOk;
      Serial.printf("EVENT,feedback,telemetry_acked,%s,%s\n",
                    metadata.feedbackId, metadata.telemetryBatchId);
      resetBackoff(now);
      return true;
    }

    endBackendRequest(false, status);
    if (status == HTTP_CODE_UNAUTHORIZED || status == HTTP_CODE_NOT_FOUND) {
      Serial.printf("EVENT,feedback,wait_operator_halt,%d,%s\n", status,
                    metadata.telemetryBatchId);
      operatorHalted_ = true;
      telemetryAckResult_ = HealthOperationResult::kRejected;
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

    if (!beginBackendRequest(url, 5000)) {
      scheduleBackoff(now);
      return true;
    }
    HTTPClient& http = backendHttp_;
    http.addHeader("Content-Type", "application/json");
    http.setAuthorizationType("Bearer");
    http.setAuthorization(workerSettings.apiToken);
    const int status = http.sendRequest(
        "POST", bundle + slices.feedbackOffset, slices.feedbackLength);

    if (status == HTTP_CODE_OK) {
      String response;
      if (!readExactAckBody(http, response)) {
        endBackendRequest(false, status);
        Serial.printf("EVENT,feedback,ready_retry_bad_ack,%s,bounded_read\n",
                      metadata.feedbackId);
        scheduleBackoff(now);
        return true;
      }
      endBackendRequest(true, status);
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

    endBackendRequest(false, status);
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

  bool uploadOneEnvelope(uint64_t now) {
    // Complete already-acknowledged corrections first, then advance new
    // corrections to ready, and only then drain ordinary telemetry.
    if (uploadOneFeedbackReady(now)) {
      return true;
    }
    if (uploadOneFeedbackWait(now)) {
      return true;
    }
    if (uploadOneTelemetryEnvelope(now)) {
      return true;
    }
    resetBackoff(now + 1000);
    return false;
  }

  size_t spoolCount_ = 0;
  size_t feedbackWaitCount_ = 0;
  size_t feedbackReadyCount_ = 0;
  size_t deadCount_ = 0;
  uint64_t lastFreezeMs_ = 0;
  uint64_t nextFilesystemRetryMs_ = 0;
  uint64_t nextSpoolAuditMs_ = 0;
  uint64_t nextHealthAttemptMs_ = 0;
  uint64_t lastHealthSuccessMs_ = 0;
  uint64_t nextHealthSequence_ = 0;
  uint64_t backlogSinceMs_ = 0;
  uint64_t lastTelemetryAckMs_ = 0;
  uint64_t lastConfigAttemptMs_ = 0;
  uint64_t lastEnvironmentAttemptMs_ = 0;
  uint64_t lastWeatherAttemptMs_ = 0;
  uint64_t desiredConfigRevision_ = 0;
  uint64_t lastWifiBeginMs_ = 0;
  uint64_t nextUploadMs_ = 0;
  uint64_t nextConfigPollMs_ = 0;
  uint64_t nextEnvironmentFetchMs_ = 0;
  uint64_t nextWeatherFetchMs_ = 0;
  uint64_t nextControlPollMs_ = 0;
  uint64_t nextCommandAckMs_ = 0;
  uint64_t nextOperationalLogMs_ = 0;
  uint64_t nextCoreDumpAttemptMs_ = 0;
  uint64_t nextOtaReleaseAttemptMs_ = 0;
  uint64_t nextRuntimePublishMs_ = 0;
  uint64_t controlAnchorUtcMs_ = 0;
  uint64_t controlAnchorUptimeMs_ = 0;
  uint64_t nextWorkerLogSequence_ = UINT64_C(0x4000000000000000);
  uint64_t anchorUtcMs_ = 0;
  uint64_t anchorUptimeMs_ = 0;
  PresenceConfig activeConfig_ = defaultPresenceConfig();
  PresenceConfig pendingConfig_ = defaultPresenceConfig();
  CommandJournal commandJournal_;
  RemoteCommandEnvelope activeCommand_ = {};
  RemoteLogSession remoteLogSession_;
  OtaDevelopmentWindow developmentWindow_;
  OtaInstallState installState_ = {};
  PendingCoreDump pendingCoreDump_;
  OtaSafetyAbortRequest lastSafetyMetrics_ = {};
  WiFiClient telemetryClient_;
  HTTPClient telemetryHttp_;
  WiFiClient backendClient_;
  HTTPClient backendHttp_;
  BackendTransportRecoveryPolicy backendTransportRecovery_;
  char conflictProbeBatchId_[96] = {};
  char developmentHostname_[33] = {};
  char activeReleaseId_[49] = {};
  uint8_t retryExponent_ = 0;
  uint8_t controlRetryExponent_ = 0;
  uint8_t commandAckRetryExponent_ = 0;
  uint8_t operationalLogRetryExponent_ = 0;
  uint8_t coreDumpRetryExponent_ = 0;
  uint32_t wifiReconnectCount_ = 0;
  uint32_t productionBytesWritten_ = 0;
  uint32_t productionImageSize_ = 0;
  CommandStep commandStep_ = CommandStep::kIdle;
  OtaRuntimePhase otaPhase_ = OtaRuntimePhase::kUnavailable;
  OtaRuntimeError otaRuntimeError_ = OtaRuntimeError::kNone;
  bool wifiStarted_ = false;
  bool wifiWasConnected_ = false;
  bool sntpStarted_ = false;
  bool hasClockAnchor_ = false;
  bool hasControlClockAnchor_ = false;
  bool hasActiveCommand_ = false;
  bool restartAfterFinalCommandAck_ = false;
  bool controlOperatorHalted_ = false;
  bool controlStorageHealthy_ = true;
  bool detailedLogWasActive_ = false;
  bool coreDumpReady_ = false;
  bool developmentOtaConfigured_ = false;
  bool developmentServiceStarted_ = false;
  bool developmentUploadJournalStaged_ = false;
  bool developmentRebootPending_ = false;
  bool installStateKnown_ = false;
  bool installStateHealthy_ = true;
  bool productionOtaActive_ = false;
  bool acceptedRebootPending_ = false;
  bool safetyAbortLatched_ = false;
  bool safetyAcceptanceFailed_ = false;
  bool criticalQueueAbortLatched_ = false;
  bool criticalQueueGuardActive_ = false;
  bool pendingImageRunning_ = false;
  bool pendingRunningReport_ = false;
  bool pendingValidatingReport_ = false;
  bool pendingRollbackReport_ = false;
  bool configAwaitingApply_ = false;
  bool conflictProbeActive_ = false;
  bool conflictProbeConfigValidated_ = false;
  bool operatorHalted_ = false;
  bool wifiAvailable_ = false;
  bool environmentFetchAttempted_ = false;
  bool weatherFetchAttempted_ = false;
  bool filesystemReady_ = false;
  bool hasReportedHealthLevel_ = false;
  bool watchdogActive_ = false;
  uint32_t guardedTelemetryCriticalDrops_ = 0;
  uint32_t guardedFeedbackDrops_ = 0;
  DeviceHealthLevel lastReportedHealthLevel_ = DeviceHealthLevel::kUnknown;
  HealthOperationResult telemetryAckResult_ =
      HealthOperationResult::kUnknown;
  HealthOperationResult configResult_ = HealthOperationResult::kUnknown;
  HealthOperationResult environmentFetchResult_ =
      HealthOperationResult::kUnknown;
  HealthOperationResult weatherFetchResult_ =
      HealthOperationResult::kUnknown;
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
                            TouchFeedbackQueue& touchFeedbackQueue,
                            DashboardMailbox& dashboardMailbox,
                            DeviceHealthMailbox& healthMailbox,
                            ControlMailbox& controlMailbox,
                            OperationalLogRing& operationalLog,
                            OtaRuntimeMailbox& otaRuntimeMailbox) {
  if (!identity.deviceIdValid || workerTaskHandle != nullptr) {
    return false;
  }
  workerIdentity = identity;
  workerSettings = settings;
  workerQueue = &queue;
  workerConfigMailbox = &configMailbox;
  workerTouchFeedbackQueue = &touchFeedbackQueue;
  workerDashboardMailbox = &dashboardMailbox;
  workerHealthMailbox = &healthMailbox;
  workerControlMailbox = &controlMailbox;
  workerOperationalLog = &operationalLog;
  workerOtaRuntimeMailbox = &otaRuntimeMailbox;
  const BaseType_t result = xTaskCreatePinnedToCore(
      uploaderTask, "presence_upload", 16384, nullptr, 1, &workerTaskHandle, 0);
  if (result != pdPASS) {
    workerTaskHandle = nullptr;
    workerQueue = nullptr;
    workerConfigMailbox = nullptr;
    workerTouchFeedbackQueue = nullptr;
    workerDashboardMailbox = nullptr;
    workerHealthMailbox = nullptr;
    workerControlMailbox = nullptr;
    workerOperationalLog = nullptr;
    workerOtaRuntimeMailbox = nullptr;
    return false;
  }
  return true;
}
