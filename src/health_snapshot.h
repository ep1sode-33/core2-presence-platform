#pragma once

#include <cstddef>
#include <cstdint>

#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#else
#include <mutex>
#endif

enum class DeviceHealthLevel : uint8_t {
  kUnknown,
  kHealthy,
  kDegraded,
  kActionRequired,
};

enum class SensorHealthStatus : uint8_t {
  kUnknown,
  kHealthy,
  kDegraded,
  kFault,
};

enum class UploaderHealthStatus : uint8_t {
  kStarting,
  kReady,
  kRetrying,
  kFilesystemUnavailable,
  kOperatorHalted,
  kTaskUnavailable,
};

enum class HealthOperationResult : uint8_t {
  kUnknown,
  kOk,
  kRetrying,
  kRejected,
  kError,
};

struct DeviceHealthSnapshot {
  static constexpr size_t kDeviceIdCapacity = 19;
  static constexpr size_t kBootIdCapacity = 33;
  static constexpr size_t kVersionCapacity = 24;
  static constexpr size_t kBuildIdCapacity = 48;
  static constexpr size_t kResetReasonCapacity = 32;
  static constexpr size_t kIpCapacity = 16;
  static constexpr size_t kOtaStateCapacity = 24;
  static constexpr size_t kDebugStateCapacity = 24;

  char deviceId[kDeviceIdCapacity] = {};
  char bootId[kBootIdCapacity] = {};
  char firmwareVersion[kVersionCapacity] = {};
  char buildId[kBuildIdCapacity] = {};
  char resetReason[kResetReasonCapacity] = {};
  char localIp[kIpCapacity] = {};
  char otaState[kOtaStateCapacity] = "inactive";
  char debugState[kDebugStateCapacity] = "inactive";

  uint64_t uptimeMs = 0;
  uint64_t sequence = 0;
  uint64_t appliedConfigRevision = 0;
  uint64_t storedConfigRevision = 0;
  uint64_t desiredConfigRevision = 0;
  uint64_t lastTelemetryAckAgeMs = UINT64_MAX;
  uint64_t lastConfigAttemptAgeMs = UINT64_MAX;
  uint64_t lastRoomFetchAgeMs = UINT64_MAX;
  uint64_t lastWeatherFetchAgeMs = UINT64_MAX;
  uint64_t mainHeartbeatAgeMs = UINT64_MAX;
  uint64_t uploaderHeartbeatAgeMs = UINT64_MAX;
  uint64_t oldestBacklogAgeMs = 0;

  uint32_t bootCount = 0;
  uint32_t wifiReconnectCount = 0;
  int32_t wifiRssiDbm = 0;
  uint32_t mainStackHighWaterBytes = 0;
  uint32_t uploaderStackHighWaterBytes = 0;
  uint32_t telemetryQueueDepth = 0;
  uint32_t telemetryQueueCapacity = 0;
  uint32_t telemetryDroppedSamples = 0;
  uint32_t telemetryDroppedCritical = 0;
  uint32_t feedbackQueueDepth = 0;
  uint32_t feedbackQueueCapacity = 0;
  uint32_t feedbackDroppedFull = 0;
  uint32_t feedbackRejectedInvalid = 0;
  uint32_t spoolFiles = 0;
  uint32_t feedbackWaitFiles = 0;
  uint32_t feedbackReadyFiles = 0;
  uint32_t deadFiles = 0;
  uint32_t littlefsTotalBytes = 0;
  uint32_t littlefsUsedBytes = 0;
  uint32_t freeHeapBytes = 0;
  uint32_t minimumFreeHeapBytes = 0;
  uint32_t largestFreeBlockBytes = 0;

  DeviceHealthLevel level = DeviceHealthLevel::kUnknown;
  SensorHealthStatus pirStatus = SensorHealthStatus::kUnknown;
  SensorHealthStatus microphoneStatus = SensorHealthStatus::kUnknown;
  UploaderHealthStatus uploaderStatus = UploaderHealthStatus::kStarting;
  HealthOperationResult telemetryAckResult = HealthOperationResult::kUnknown;
  HealthOperationResult configResult = HealthOperationResult::kUnknown;
  HealthOperationResult roomFetchResult = HealthOperationResult::kUnknown;
  HealthOperationResult weatherFetchResult = HealthOperationResult::kUnknown;
  bool initialized = false;
  bool wifiConnected = false;
  bool clockSynchronized = false;
  bool filesystemReady = false;
  bool pirOnlyMode = false;
  bool safeMode = false;
  bool otaActive = false;
  bool debugActive = false;
  // Local policy input; represented remotely by level + ota/debug state.
  bool runtimeActionRequired = false;
  uint32_t version = 0;
};

struct DeviceHealthMainUpdate {
  char deviceId[DeviceHealthSnapshot::kDeviceIdCapacity] = {};
  char bootId[DeviceHealthSnapshot::kBootIdCapacity] = {};
  char firmwareVersion[DeviceHealthSnapshot::kVersionCapacity] = {};
  char buildId[DeviceHealthSnapshot::kBuildIdCapacity] = {};
  char resetReason[DeviceHealthSnapshot::kResetReasonCapacity] = {};
  uint64_t uptimeMs = 0;
  uint64_t appliedConfigRevision = 0;
  uint64_t storedConfigRevision = 0;
  uint64_t mainHeartbeatAgeMs = 0;
  uint32_t bootCount = 0;
  uint32_t mainStackHighWaterBytes = 0;
  uint32_t telemetryQueueDepth = 0;
  uint32_t telemetryQueueCapacity = 0;
  uint32_t telemetryDroppedSamples = 0;
  uint32_t telemetryDroppedCritical = 0;
  uint32_t feedbackQueueDepth = 0;
  uint32_t feedbackQueueCapacity = 0;
  uint32_t feedbackDroppedFull = 0;
  uint32_t feedbackRejectedInvalid = 0;
  uint32_t freeHeapBytes = 0;
  uint32_t minimumFreeHeapBytes = 0;
  uint32_t largestFreeBlockBytes = 0;
  SensorHealthStatus pirStatus = SensorHealthStatus::kUnknown;
  SensorHealthStatus microphoneStatus = SensorHealthStatus::kUnknown;
  bool initialized = false;
  bool pirOnlyMode = false;
  bool safeMode = false;
};

struct DeviceHealthWorkerUpdate {
  char localIp[DeviceHealthSnapshot::kIpCapacity] = {};
  uint64_t uptimeMs = 0;
  uint64_t desiredConfigRevision = 0;
  uint64_t lastTelemetryAckAgeMs = UINT64_MAX;
  uint64_t lastConfigAttemptAgeMs = UINT64_MAX;
  uint64_t lastRoomFetchAgeMs = UINT64_MAX;
  uint64_t lastWeatherFetchAgeMs = UINT64_MAX;
  uint64_t uploaderHeartbeatAgeMs = 0;
  uint64_t oldestBacklogAgeMs = 0;
  uint32_t wifiReconnectCount = 0;
  int32_t wifiRssiDbm = 0;
  uint32_t uploaderStackHighWaterBytes = 0;
  uint32_t spoolFiles = 0;
  uint32_t feedbackWaitFiles = 0;
  uint32_t feedbackReadyFiles = 0;
  uint32_t deadFiles = 0;
  uint32_t littlefsTotalBytes = 0;
  uint32_t littlefsUsedBytes = 0;
  UploaderHealthStatus uploaderStatus = UploaderHealthStatus::kStarting;
  HealthOperationResult telemetryAckResult = HealthOperationResult::kUnknown;
  HealthOperationResult configResult = HealthOperationResult::kUnknown;
  HealthOperationResult roomFetchResult = HealthOperationResult::kUnknown;
  HealthOperationResult weatherFetchResult = HealthOperationResult::kUnknown;
  bool wifiConnected = false;
  bool clockSynchronized = false;
  bool filesystemReady = false;
};

const char* deviceHealthLevelWireName(DeviceHealthLevel value);
const char* sensorHealthStatusWireName(SensorHealthStatus value);
const char* uploaderHealthStatusWireName(UploaderHealthStatus value);
const char* healthOperationResultWireName(HealthOperationResult value);

// Pure policy used by the mailbox and native tests. Offline is deliberately
// absent: only the backend can infer that from server-observed arrival time.
DeviceHealthLevel evaluateDeviceHealth(const DeviceHealthSnapshot& snapshot);

constexpr uint32_t nextHealthSnapshotVersion(uint32_t current) {
  return static_cast<uint32_t>(current + UINT32_C(1));
}

class DeviceHealthMailbox {
 public:
  DeviceHealthMailbox() = default;
  DeviceHealthMailbox(const DeviceHealthMailbox&) = delete;
  DeviceHealthMailbox& operator=(const DeviceHealthMailbox&) = delete;

  // Main and the Core 0 worker build a complete candidate from the latest
  // snapshot, then publish atomically. Callers must copySnapshot immediately
  // before changing only their owned fields so cross-task fields are retained.
  bool publish(const DeviceHealthSnapshot& snapshot);
  bool publishMain(const DeviceHealthMainUpdate& update);
  void publishWorker(const DeviceHealthWorkerUpdate& update);
  bool publishRuntimeActivity(bool otaActive, const char* otaState,
                              bool debugActive, const char* debugState);
  DeviceHealthSnapshot snapshot() const;
  bool copySnapshot(DeviceHealthSnapshot* output) const;

 private:
  void lock() const;
  void unlock() const;

#if defined(ARDUINO_ARCH_ESP32)
  mutable portMUX_TYPE mutex_ = portMUX_INITIALIZER_UNLOCKED;
#else
  mutable std::mutex mutex_;
#endif
  DeviceHealthSnapshot snapshot_ = {};
};
