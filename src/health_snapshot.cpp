#include "health_snapshot.h"

#include <cstdio>
#include <cstring>
#include <type_traits>

namespace {

bool terminated(const char* value, size_t capacity) {
  return value != nullptr && std::memchr(value, '\0', capacity) != nullptr;
}

uint64_t advancedAge(uint64_t age, uint64_t elapsed) {
  if (age == UINT64_MAX || UINT64_MAX - age < elapsed) {
    return UINT64_MAX;
  }
  return age + elapsed;
}

}  // namespace

static_assert(std::is_trivially_copyable<DeviceHealthSnapshot>::value,
              "health snapshot must remain POD-copyable");

const char* deviceHealthLevelWireName(DeviceHealthLevel value) {
  switch (value) {
    case DeviceHealthLevel::kUnknown:
      return "unknown";
    case DeviceHealthLevel::kHealthy:
      return "healthy";
    case DeviceHealthLevel::kDegraded:
      return "degraded";
    case DeviceHealthLevel::kActionRequired:
      return "action_required";
  }
  return "unknown";
}

const char* sensorHealthStatusWireName(SensorHealthStatus value) {
  switch (value) {
    case SensorHealthStatus::kUnknown:
      return "unknown";
    case SensorHealthStatus::kHealthy:
      return "healthy";
    case SensorHealthStatus::kDegraded:
      return "degraded";
    case SensorHealthStatus::kFault:
      return "fault";
  }
  return "unknown";
}

const char* uploaderHealthStatusWireName(UploaderHealthStatus value) {
  switch (value) {
    case UploaderHealthStatus::kStarting:
      return "starting";
    case UploaderHealthStatus::kReady:
      return "ready";
    case UploaderHealthStatus::kRetrying:
      return "retrying";
    case UploaderHealthStatus::kFilesystemUnavailable:
      return "filesystem_unavailable";
    case UploaderHealthStatus::kOperatorHalted:
      return "operator_halted";
    case UploaderHealthStatus::kTaskUnavailable:
      return "task_unavailable";
  }
  return "task_unavailable";
}

const char* healthOperationResultWireName(HealthOperationResult value) {
  switch (value) {
    case HealthOperationResult::kUnknown:
      return "unknown";
    case HealthOperationResult::kOk:
      return "ok";
    case HealthOperationResult::kRetrying:
      return "retrying";
    case HealthOperationResult::kRejected:
      return "rejected";
    case HealthOperationResult::kError:
      return "error";
  }
  return "unknown";
}

DeviceHealthLevel evaluateDeviceHealth(const DeviceHealthSnapshot& snapshot) {
  if (!snapshot.initialized) {
    return DeviceHealthLevel::kUnknown;
  }
  if (snapshot.safeMode || snapshot.runtimeActionRequired ||
      snapshot.uploaderStatus == UploaderHealthStatus::kOperatorHalted ||
      snapshot.uploaderStatus == UploaderHealthStatus::kTaskUnavailable ||
      snapshot.telemetryDroppedCritical != 0) {
    return DeviceHealthLevel::kActionRequired;
  }
  if (!snapshot.filesystemReady || !snapshot.wifiConnected ||
      snapshot.uploaderStatus != UploaderHealthStatus::kReady ||
      (snapshot.wifiConnected && snapshot.wifiRssiDbm <= -80) ||
      (snapshot.lastTelemetryAckAgeMs != UINT64_MAX &&
       snapshot.lastTelemetryAckAgeMs >= 120000) ||
      (snapshot.lastConfigAttemptAgeMs != UINT64_MAX &&
       snapshot.lastConfigAttemptAgeMs >= 10ULL * 60ULL * 1000ULL) ||
      (snapshot.lastRoomFetchAgeMs != UINT64_MAX &&
       snapshot.lastRoomFetchAgeMs >= 2ULL * 60ULL * 1000ULL) ||
      (snapshot.lastWeatherFetchAgeMs != UINT64_MAX &&
       snapshot.lastWeatherFetchAgeMs >= 30ULL * 60ULL * 1000ULL) ||
      (snapshot.telemetryAckResult != HealthOperationResult::kUnknown &&
       snapshot.telemetryAckResult != HealthOperationResult::kOk) ||
      (snapshot.configResult != HealthOperationResult::kUnknown &&
       snapshot.configResult != HealthOperationResult::kOk) ||
      (snapshot.roomFetchResult != HealthOperationResult::kUnknown &&
       snapshot.roomFetchResult != HealthOperationResult::kOk) ||
      (snapshot.weatherFetchResult != HealthOperationResult::kUnknown &&
       snapshot.weatherFetchResult != HealthOperationResult::kOk) ||
      snapshot.pirStatus == SensorHealthStatus::kFault ||
      snapshot.microphoneStatus == SensorHealthStatus::kFault ||
      snapshot.pirStatus == SensorHealthStatus::kDegraded ||
      snapshot.microphoneStatus == SensorHealthStatus::kDegraded ||
      snapshot.spoolFiles >= 64 ||
      snapshot.feedbackWaitFiles + snapshot.feedbackReadyFiles >= 8 ||
      snapshot.deadFiles != 0 || snapshot.oldestBacklogAgeMs >= 120000 ||
      (snapshot.telemetryQueueCapacity != 0 &&
       snapshot.telemetryQueueDepth * 10 >=
           snapshot.telemetryQueueCapacity * 9) ||
      (snapshot.feedbackQueueCapacity != 0 &&
       snapshot.feedbackQueueDepth * 4 >=
           snapshot.feedbackQueueCapacity * 3) ||
      (snapshot.littlefsTotalBytes != 0 &&
       snapshot.littlefsTotalBytes >= snapshot.littlefsUsedBytes &&
       snapshot.littlefsTotalBytes - snapshot.littlefsUsedBytes <
           128U * 1024U) ||
      (snapshot.freeHeapBytes != 0 && snapshot.freeHeapBytes < 32U * 1024U) ||
      snapshot.mainHeartbeatAgeMs > 5000 ||
      snapshot.uploaderHeartbeatAgeMs > 15000) {
    return DeviceHealthLevel::kDegraded;
  }
  return DeviceHealthLevel::kHealthy;
}

void DeviceHealthMailbox::lock() const {
#if defined(ARDUINO_ARCH_ESP32)
  portENTER_CRITICAL(&mutex_);
#else
  mutex_.lock();
#endif
}

void DeviceHealthMailbox::unlock() const {
#if defined(ARDUINO_ARCH_ESP32)
  portEXIT_CRITICAL(&mutex_);
#else
  mutex_.unlock();
#endif
}

bool DeviceHealthMailbox::publish(const DeviceHealthSnapshot& snapshot) {
  if (!terminated(snapshot.deviceId, sizeof(snapshot.deviceId)) ||
      !terminated(snapshot.bootId, sizeof(snapshot.bootId)) ||
      !terminated(snapshot.firmwareVersion, sizeof(snapshot.firmwareVersion)) ||
      !terminated(snapshot.buildId, sizeof(snapshot.buildId)) ||
      !terminated(snapshot.resetReason, sizeof(snapshot.resetReason)) ||
      !terminated(snapshot.localIp, sizeof(snapshot.localIp)) ||
      !terminated(snapshot.otaState, sizeof(snapshot.otaState)) ||
      !terminated(snapshot.debugState, sizeof(snapshot.debugState))) {
    return false;
  }

  lock();
  DeviceHealthSnapshot candidate = snapshot;
  candidate.level = evaluateDeviceHealth(candidate);
  candidate.version = nextHealthSnapshotVersion(snapshot_.version);
  snapshot_ = candidate;
  unlock();
  return true;
}

bool DeviceHealthMailbox::publishMain(const DeviceHealthMainUpdate& update) {
  if (!terminated(update.deviceId, sizeof(update.deviceId)) ||
      !terminated(update.bootId, sizeof(update.bootId)) ||
      !terminated(update.firmwareVersion, sizeof(update.firmwareVersion)) ||
      !terminated(update.buildId, sizeof(update.buildId)) ||
      !terminated(update.resetReason, sizeof(update.resetReason))) {
    return false;
  }
  lock();
  if (update.uptimeMs >= snapshot_.uptimeMs) {
    snapshot_.uploaderHeartbeatAgeMs = advancedAge(
        snapshot_.uploaderHeartbeatAgeMs,
        update.uptimeMs - snapshot_.uptimeMs);
  }
  std::memcpy(snapshot_.deviceId, update.deviceId, sizeof(update.deviceId));
  std::memcpy(snapshot_.bootId, update.bootId, sizeof(update.bootId));
  std::memcpy(snapshot_.firmwareVersion, update.firmwareVersion,
              sizeof(update.firmwareVersion));
  std::memcpy(snapshot_.buildId, update.buildId, sizeof(update.buildId));
  std::memcpy(snapshot_.resetReason, update.resetReason,
              sizeof(update.resetReason));
  snapshot_.uptimeMs = update.uptimeMs;
  snapshot_.appliedConfigRevision = update.appliedConfigRevision;
  snapshot_.storedConfigRevision = update.storedConfigRevision;
  snapshot_.mainHeartbeatAgeMs = update.mainHeartbeatAgeMs;
  snapshot_.bootCount = update.bootCount;
  snapshot_.mainStackHighWaterBytes = update.mainStackHighWaterBytes;
  snapshot_.telemetryQueueDepth = update.telemetryQueueDepth;
  snapshot_.telemetryQueueCapacity = update.telemetryQueueCapacity;
  snapshot_.telemetryDroppedSamples = update.telemetryDroppedSamples;
  snapshot_.telemetryDroppedCritical = update.telemetryDroppedCritical;
  snapshot_.feedbackQueueDepth = update.feedbackQueueDepth;
  snapshot_.feedbackQueueCapacity = update.feedbackQueueCapacity;
  snapshot_.feedbackDroppedFull = update.feedbackDroppedFull;
  snapshot_.feedbackRejectedInvalid = update.feedbackRejectedInvalid;
  snapshot_.freeHeapBytes = update.freeHeapBytes;
  snapshot_.minimumFreeHeapBytes = update.minimumFreeHeapBytes;
  snapshot_.largestFreeBlockBytes = update.largestFreeBlockBytes;
  snapshot_.pirStatus = update.pirStatus;
  snapshot_.microphoneStatus = update.microphoneStatus;
  snapshot_.initialized = update.initialized;
  snapshot_.pirOnlyMode = update.pirOnlyMode;
  snapshot_.safeMode = update.safeMode;
  snapshot_.level = evaluateDeviceHealth(snapshot_);
  snapshot_.version = nextHealthSnapshotVersion(snapshot_.version);
  unlock();
  return true;
}

void DeviceHealthMailbox::publishWorker(
    const DeviceHealthWorkerUpdate& update) {
  lock();
  if (update.uptimeMs >= snapshot_.uptimeMs) {
    snapshot_.mainHeartbeatAgeMs = advancedAge(
        snapshot_.mainHeartbeatAgeMs, update.uptimeMs - snapshot_.uptimeMs);
  }
  std::memcpy(snapshot_.localIp, update.localIp, sizeof(update.localIp));
  snapshot_.uptimeMs = update.uptimeMs;
  snapshot_.desiredConfigRevision = update.desiredConfigRevision;
  snapshot_.lastTelemetryAckAgeMs = update.lastTelemetryAckAgeMs;
  snapshot_.lastConfigAttemptAgeMs = update.lastConfigAttemptAgeMs;
  snapshot_.lastRoomFetchAgeMs = update.lastRoomFetchAgeMs;
  snapshot_.lastWeatherFetchAgeMs = update.lastWeatherFetchAgeMs;
  snapshot_.uploaderHeartbeatAgeMs = update.uploaderHeartbeatAgeMs;
  snapshot_.oldestBacklogAgeMs = update.oldestBacklogAgeMs;
  snapshot_.wifiReconnectCount = update.wifiReconnectCount;
  snapshot_.wifiRssiDbm = update.wifiRssiDbm;
  snapshot_.uploaderStackHighWaterBytes = update.uploaderStackHighWaterBytes;
  snapshot_.spoolFiles = update.spoolFiles;
  snapshot_.feedbackWaitFiles = update.feedbackWaitFiles;
  snapshot_.feedbackReadyFiles = update.feedbackReadyFiles;
  snapshot_.deadFiles = update.deadFiles;
  snapshot_.littlefsTotalBytes = update.littlefsTotalBytes;
  snapshot_.littlefsUsedBytes = update.littlefsUsedBytes;
  snapshot_.uploaderStatus = update.uploaderStatus;
  snapshot_.telemetryAckResult = update.telemetryAckResult;
  snapshot_.configResult = update.configResult;
  snapshot_.roomFetchResult = update.roomFetchResult;
  snapshot_.weatherFetchResult = update.weatherFetchResult;
  snapshot_.wifiConnected = update.wifiConnected;
  snapshot_.clockSynchronized = update.clockSynchronized;
  snapshot_.filesystemReady = update.filesystemReady;
  snapshot_.level = evaluateDeviceHealth(snapshot_);
  snapshot_.version = nextHealthSnapshotVersion(snapshot_.version);
  unlock();
}

bool DeviceHealthMailbox::publishRuntimeActivity(bool otaActive,
                                                 const char* otaState,
                                                 bool debugActive,
                                                 const char* debugState) {
  if (otaState == nullptr || debugState == nullptr || otaState[0] == '\0' ||
      debugState[0] == '\0' ||
      std::strlen(otaState) >= DeviceHealthSnapshot::kOtaStateCapacity ||
      std::strlen(debugState) >= DeviceHealthSnapshot::kDebugStateCapacity) {
    return false;
  }
  lock();
  snapshot_.otaActive = otaActive;
  snapshot_.debugActive = debugActive;
  std::snprintf(snapshot_.otaState, sizeof(snapshot_.otaState), "%s", otaState);
  std::snprintf(snapshot_.debugState, sizeof(snapshot_.debugState), "%s",
                debugState);
  snapshot_.runtimeActionRequired =
      std::strcmp(otaState, "failed") == 0 ||
      std::strcmp(otaState, "rollback_failed") == 0 ||
      std::strcmp(debugState, "failed") == 0;
  snapshot_.level = evaluateDeviceHealth(snapshot_);
  snapshot_.version = nextHealthSnapshotVersion(snapshot_.version);
  unlock();
  return true;
}

DeviceHealthSnapshot DeviceHealthMailbox::snapshot() const {
  DeviceHealthSnapshot result = {};
  copySnapshot(&result);
  return result;
}

bool DeviceHealthMailbox::copySnapshot(DeviceHealthSnapshot* output) const {
  if (output == nullptr) {
    return false;
  }
  lock();
  *output = snapshot_;
  unlock();
  return true;
}
