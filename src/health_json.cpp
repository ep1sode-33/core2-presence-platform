#include "health_json.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

constexpr uint64_t kMaximumWireInteger = INT64_MAX;

class Writer {
 public:
  explicit Writer(const HealthJsonSink& sink) : sink_(sink) {}

  bool raw(const char* value) { return bytes(value, std::strlen(value)); }
  bool bytes(const char* value, size_t size) {
    if (!ok_ || sink_.write == nullptr || (size != 0 && value == nullptr)) {
      ok_ = false;
      return false;
    }
    if (size != 0) {
      ok_ = sink_.write(sink_.context, value, size);
    }
    return ok_;
  }
  bool format(const char* format, ...) {
    // The largest health section is emitted in one bounded format call. Keep
    // this fixed (no heap/String growth) while leaving room for all counters.
    char buffer[1536] = {};
    va_list arguments;
    va_start(arguments, format);
    const int length = std::vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(buffer)) {
      ok_ = false;
      return false;
    }
    return bytes(buffer, static_cast<size_t>(length));
  }
  bool string(const char* value) {
    if (value == nullptr || !raw("\"")) return false;
    for (const unsigned char* cursor =
             reinterpret_cast<const unsigned char*>(value);
         *cursor != '\0'; ++cursor) {
      if (*cursor == '\"' || *cursor == '\\') {
        if (!format("\\%c", *cursor)) return false;
      } else if (*cursor < 0x20) {
        if (!format("\\u%04x", static_cast<unsigned>(*cursor))) return false;
      } else if (!bytes(reinterpret_cast<const char*>(cursor), 1)) {
        return false;
      }
    }
    return raw("\"");
  }
  bool ok() const { return ok_; }

 private:
  HealthJsonSink sink_;
  bool ok_ = true;
};

bool writeAge(Writer& writer, uint64_t ageMs) {
  return ageMs == UINT64_MAX
             ? writer.raw("null")
             : writer.format("%llu", static_cast<unsigned long long>(ageMs));
}

bool boundedString(const char* value, size_t capacity, bool allowEmpty) {
  if (value == nullptr || capacity == 0 ||
      std::memchr(value, '\0', capacity) == nullptr) {
    return false;
  }
  return allowEmpty || value[0] != '\0';
}

bool validAge(uint64_t value) {
  return value == UINT64_MAX || value <= kMaximumWireInteger;
}

}  // namespace

bool writeDeviceHealthJson(const DeviceHealthSnapshot& snapshot,
                           const HealthJsonSink& sink) {
  if (sink.write == nullptr ||
      !boundedString(snapshot.deviceId, sizeof(snapshot.deviceId), false) ||
      !boundedString(snapshot.bootId, sizeof(snapshot.bootId), false) ||
      !boundedString(snapshot.firmwareVersion,
                     sizeof(snapshot.firmwareVersion), false) ||
      !boundedString(snapshot.buildId, sizeof(snapshot.buildId), false) ||
      !boundedString(snapshot.resetReason, sizeof(snapshot.resetReason), false) ||
      !boundedString(snapshot.localIp, sizeof(snapshot.localIp), true) ||
      !boundedString(snapshot.otaState, sizeof(snapshot.otaState), false) ||
      !boundedString(snapshot.debugState, sizeof(snapshot.debugState), false) ||
      snapshot.uptimeMs > kMaximumWireInteger ||
      snapshot.sequence > kMaximumWireInteger ||
      snapshot.appliedConfigRevision > kMaximumWireInteger ||
      snapshot.storedConfigRevision > kMaximumWireInteger ||
      snapshot.desiredConfigRevision > kMaximumWireInteger ||
      snapshot.oldestBacklogAgeMs > kMaximumWireInteger ||
      snapshot.telemetryQueueCapacity == 0 ||
      snapshot.telemetryQueueDepth > snapshot.telemetryQueueCapacity ||
      snapshot.feedbackQueueCapacity == 0 ||
      snapshot.feedbackQueueDepth > snapshot.feedbackQueueCapacity ||
      snapshot.littlefsUsedBytes > snapshot.littlefsTotalBytes ||
      snapshot.wifiRssiDbm < -127 || snapshot.wifiRssiDbm > 0 ||
      !validAge(snapshot.lastTelemetryAckAgeMs) ||
      !validAge(snapshot.lastConfigAttemptAgeMs) ||
      !validAge(snapshot.lastRoomFetchAgeMs) ||
      !validAge(snapshot.lastWeatherFetchAgeMs) ||
      snapshot.mainHeartbeatAgeMs > kMaximumWireInteger ||
      snapshot.uploaderHeartbeatAgeMs > kMaximumWireInteger) {
    return false;
  }

  Writer writer(sink);
  if (!writer.raw("{\"schema_version\":1,\"device_id\":") ||
      !writer.string(snapshot.deviceId) || !writer.raw(",\"boot_id\":") ||
      !writer.string(snapshot.bootId) ||
      !writer.raw(",\"firmware_version\":") ||
      !writer.string(snapshot.firmwareVersion) ||
      !writer.raw(",\"build_id\":") || !writer.string(snapshot.buildId) ||
      !writer.format(",\"uptime_ms\":%llu,\"sequence\":%llu,\"level\":",
                     static_cast<unsigned long long>(snapshot.uptimeMs),
                     static_cast<unsigned long long>(snapshot.sequence)) ||
      !writer.string(deviceHealthLevelWireName(snapshot.level)) ||
      !writer.raw(",\"reset_reason\":") ||
      !writer.string(snapshot.resetReason) ||
      !writer.format(",\"boot_count\":%u,\"wifi\":{\"connected\":%s,"
                     "\"ip\":",
                     snapshot.bootCount,
                     snapshot.wifiConnected ? "true" : "false") ||
      !writer.string(snapshot.localIp) ||
      !writer.format(",\"rssi_dbm\":%ld,\"reconnect_count\":%u,"
                     "\"clock_synchronized\":%s},\"config\":{"
                     "\"desired_revision\":%llu,\"stored_revision\":%llu,"
                     "\"applied_revision\":%llu},\"freshness\":{"
                     "\"last_telemetry_ack_ms\":",
                     static_cast<long>(snapshot.wifiRssiDbm),
                     snapshot.wifiReconnectCount,
                     snapshot.clockSynchronized ? "true" : "false",
                     static_cast<unsigned long long>(
                         snapshot.desiredConfigRevision),
                     static_cast<unsigned long long>(
                         snapshot.storedConfigRevision),
                     static_cast<unsigned long long>(
                         snapshot.appliedConfigRevision)) ||
      !writeAge(writer, snapshot.lastTelemetryAckAgeMs) ||
      !writer.raw(",\"last_config_attempt_ms\":") ||
      !writeAge(writer, snapshot.lastConfigAttemptAgeMs) ||
      !writer.raw(",\"last_room_fetch_ms\":") ||
      !writeAge(writer, snapshot.lastRoomFetchAgeMs) ||
      !writer.raw(",\"last_weather_fetch_ms\":") ||
      !writeAge(writer, snapshot.lastWeatherFetchAgeMs) ||
      !writer.raw(",\"telemetry_ack_result\":") ||
      !writer.string(
          healthOperationResultWireName(snapshot.telemetryAckResult)) ||
      !writer.raw(",\"config_result\":") ||
      !writer.string(healthOperationResultWireName(snapshot.configResult)) ||
      !writer.raw(",\"room_fetch_result\":") ||
      !writer.string(
          healthOperationResultWireName(snapshot.roomFetchResult)) ||
      !writer.raw(",\"weather_fetch_result\":") ||
      !writer.string(
          healthOperationResultWireName(snapshot.weatherFetchResult)) ||
      !writer.raw("},\"tasks\":{\"main_heartbeat_ms\":") ||
      !writeAge(writer, snapshot.mainHeartbeatAgeMs) ||
      !writer.raw(",\"uploader_heartbeat_ms\":") ||
      !writeAge(writer, snapshot.uploaderHeartbeatAgeMs) ||
      !writer.format(",\"main_stack_hwm\":%u,\"uploader_stack_hwm\":%u},"
                     "\"queues\":{\"telemetry_depth\":%u,"
                     "\"telemetry_capacity\":%u,\"dropped_samples\":%u,"
                     "\"dropped_critical\":%u,\"feedback_depth\":%u,"
                     "\"feedback_capacity\":%u,\"feedback_dropped_full\":%u,"
                     "\"feedback_rejected_invalid\":%u},\"storage\":{"
                     "\"filesystem_ready\":%s,\"spool_files\":%u,"
                     "\"feedback_wait_files\":%u,"
                     "\"feedback_ready_files\":%u,\"dead_files\":%u,"
                     "\"oldest_backlog_age_ms\":%llu,"
                     "\"littlefs_total_bytes\":%u,"
                     "\"littlefs_used_bytes\":%u,"
                     "\"littlefs_free_bytes\":%u},\"memory\":{"
                     "\"free_heap_bytes\":%u,\"min_free_heap_bytes\":%u,"
                     "\"largest_free_block_bytes\":%u},\"sensors\":{"
                     "\"pir_status\":",
                     snapshot.mainStackHighWaterBytes,
                     snapshot.uploaderStackHighWaterBytes,
                     snapshot.telemetryQueueDepth,
                     snapshot.telemetryQueueCapacity,
                     snapshot.telemetryDroppedSamples,
                     snapshot.telemetryDroppedCritical,
                     snapshot.feedbackQueueDepth,
                     snapshot.feedbackQueueCapacity,
                     snapshot.feedbackDroppedFull,
                     snapshot.feedbackRejectedInvalid,
                     snapshot.filesystemReady ? "true" : "false",
                     snapshot.spoolFiles, snapshot.feedbackWaitFiles,
                     snapshot.feedbackReadyFiles, snapshot.deadFiles,
                     static_cast<unsigned long long>(
                     snapshot.oldestBacklogAgeMs),
                     snapshot.littlefsTotalBytes, snapshot.littlefsUsedBytes,
                     snapshot.littlefsTotalBytes >= snapshot.littlefsUsedBytes
                         ? snapshot.littlefsTotalBytes -
                               snapshot.littlefsUsedBytes
                         : 0,
                     snapshot.freeHeapBytes, snapshot.minimumFreeHeapBytes,
                     snapshot.largestFreeBlockBytes) ||
      !writer.string(sensorHealthStatusWireName(snapshot.pirStatus)) ||
      !writer.raw(",\"mic_status\":") ||
      !writer.string(sensorHealthStatusWireName(snapshot.microphoneStatus)) ||
      !writer.format(",\"pir_only_mode\":%s},\"uploader_status\":",
                     snapshot.pirOnlyMode ? "true" : "false") ||
      !writer.string(uploaderHealthStatusWireName(snapshot.uploaderStatus)) ||
      !writer.raw(",\"ota\":{\"active\":") ||
      !writer.raw(snapshot.otaActive ? "true" : "false") ||
      !writer.raw(",\"state\":") || !writer.string(snapshot.otaState) ||
      !writer.raw("},\"debug\":{\"active\":") ||
      !writer.raw(snapshot.debugActive ? "true" : "false") ||
      !writer.raw(",\"state\":") || !writer.string(snapshot.debugState) ||
      !writer.format("},\"safe_mode\":%s}",
                     snapshot.safeMode ? "true" : "false")) {
    return false;
  }
  return writer.ok();
}
