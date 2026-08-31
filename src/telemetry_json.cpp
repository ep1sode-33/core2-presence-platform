#include "telemetry_json.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

constexpr uint64_t kMaxSigned64 = INT64_MAX;
constexpr float kMaximumSensorMetric = 1000000000.0f;

bool validState(PresenceState state) {
  switch (state) {
    case PresenceState::kCalibrating:
    case PresenceState::kIdle:
    case PresenceState::kPresent:
    case PresenceState::kCooldown:
      return true;
  }
  return false;
}

bool validReason(TransitionReason reason) {
  switch (reason) {
    case TransitionReason::kBoot:
    case TransitionReason::kCalibrationComplete:
    case TransitionReason::kPirMotion:
    case TransitionReason::kSoundBridge:
    case TransitionReason::kQuietTimeout:
    case TransitionReason::kCooldownTimeout:
    case TransitionReason::kTouchWake:
    case TransitionReason::kBenchOverride:
    case TransitionReason::kConfigChange:
    case TransitionReason::kUnknown:
      return true;
  }
  return false;
}

bool validNonnegativeFloat(float value) {
  return std::isfinite(value) && value >= 0.0f &&
         value <= kMaximumSensorMetric;
}

bool validContext(const TelemetryBatchContext& context) {
  if (context.batchId == nullptr || context.batchId[0] == '\0' ||
      std::strlen(context.batchId) > 96 || context.bootId == nullptr ||
      std::strlen(context.bootId) < 16 || std::strlen(context.bootId) > 64 ||
      context.firmwareVersion == nullptr ||
      context.firmwareVersion[0] == '\0' ||
      std::strlen(context.firmwareVersion) > 64 ||
      context.buildId == nullptr || context.buildId[0] == '\0' ||
      std::strlen(context.buildId) > 64 ||
      context.appliedConfigRevision > kMaxSigned64) {
    return false;
  }
  if (context.hasClockAnchor &&
      (context.anchorUtcMs > kMaxSigned64 ||
       context.anchorUptimeMs > kMaxSigned64)) {
    return false;
  }
  return true;
}

bool validRecords(const TelemetryRecord* records, size_t recordCount,
                  uint64_t appliedConfigRevision) {
  if (records == nullptr || recordCount == 0 || recordCount > 256) {
    return false;
  }

  for (size_t index = 0; index < recordCount; ++index) {
    const TelemetryRecord& record = records[index];
    if (record.seq > kMaxSigned64 || record.uptimeMs > kMaxSigned64 ||
        record.appliedConfigRevision != appliedConfigRevision) {
      return false;
    }
    for (size_t previous = 0; previous < index; ++previous) {
      if (records[previous].seq == record.seq) {
        return false;
      }
    }

    switch (record.kind) {
      case TelemetryKind::kSample:
        if (!validNonnegativeFloat(record.sample.micRms) ||
            !validNonnegativeFloat(record.sample.micEnvelope) ||
            !validNonnegativeFloat(record.sample.noiseFloor) ||
            !validNonnegativeFloat(record.sample.soundThreshold) ||
            record.sample.micMin > record.sample.micMax ||
            !validState(record.sample.state)) {
          return false;
        }
        break;
      case TelemetryKind::kTransition:
        if ((record.transition.hasFromState &&
             !validState(record.transition.fromState)) ||
            !validState(record.transition.toState) ||
            !validReason(record.transition.reason) ||
            record.transition.pirAgeMs > kMaxSigned64 ||
            record.transition.soundAgeMs > kMaxSigned64 ||
            !validNonnegativeFloat(record.transition.micEnvelope) ||
            !validNonnegativeFloat(record.transition.noiseFloor) ||
            !validNonnegativeFloat(record.transition.soundThreshold)) {
          return false;
        }
        break;
      default:
        return false;
    }
  }
  return true;
}

class Writer {
 public:
  explicit Writer(const TelemetryJsonSink& sink) : sink_(sink) {}

  bool raw(const char* value) {
    return bytes(value, std::strlen(value));
  }

  bool bytes(const char* value, size_t size) {
    return ok_ && size > 0 && sink_.write != nullptr
               ? (ok_ = sink_.write(sink_.context, value, size))
               : ok_ && size == 0;
  }

  bool format(const char* formatString, ...) {
    char buffer[256];
    va_list arguments;
    va_start(arguments, formatString);
    const int length = std::vsnprintf(buffer, sizeof(buffer), formatString,
                                      arguments);
    va_end(arguments);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(buffer)) {
      ok_ = false;
      return false;
    }
    return bytes(buffer, static_cast<size_t>(length));
  }

  bool jsonString(const char* value) {
    if (!raw("\"")) {
      return false;
    }
    for (const unsigned char* cursor =
             reinterpret_cast<const unsigned char*>(value);
         *cursor != '\0'; ++cursor) {
      switch (*cursor) {
        case '\"':
          if (!raw("\\\"")) return false;
          break;
        case '\\':
          if (!raw("\\\\")) return false;
          break;
        case '\b':
          if (!raw("\\b")) return false;
          break;
        case '\f':
          if (!raw("\\f")) return false;
          break;
        case '\n':
          if (!raw("\\n")) return false;
          break;
        case '\r':
          if (!raw("\\r")) return false;
          break;
        case '\t':
          if (!raw("\\t")) return false;
          break;
        default:
          if (*cursor < 0x20) {
            if (!format("\\u%04x", static_cast<unsigned>(*cursor))) {
              return false;
            }
          } else if (!bytes(reinterpret_cast<const char*>(cursor), 1)) {
            return false;
          }
      }
    }
    return raw("\"");
  }

  bool ok() const { return ok_; }

 private:
  TelemetryJsonSink sink_;
  bool ok_ = true;
};

bool writeRecord(Writer& writer, const TelemetryRecord& record) {
  if (!writer.format("{\"seq\":%llu,\"kind\":",
                     static_cast<unsigned long long>(record.seq))) {
    return false;
  }

  if (record.kind == TelemetryKind::kSample) {
    return writer.raw(
               "\"sample\",\"uptime_ms\":") &&
           writer.format("%llu", static_cast<unsigned long long>(
                                     record.uptimeMs)) &&
           writer.raw(",\"pir\":") &&
           writer.raw(record.sample.pir ? "true" : "false") &&
           writer.format(
               ",\"mic_rms\":%.3f,\"mic_envelope\":%.3f,\"mic_min\":%d,"
               "\"mic_max\":%d,\"noise_floor\":%.3f,"
               "\"sound_threshold\":%.3f,\"sound_active\":",
               static_cast<double>(record.sample.micRms),
               static_cast<double>(record.sample.micEnvelope),
               static_cast<int>(record.sample.micMin),
               static_cast<int>(record.sample.micMax),
               static_cast<double>(record.sample.noiseFloor),
               static_cast<double>(record.sample.soundThreshold)) &&
           writer.raw(record.sample.soundActive ? "true" : "false") &&
           writer.raw(",\"state\":") &&
           writer.jsonString(presenceStateWireName(record.sample.state)) &&
           writer.format(",\"brightness\":%u}",
                         static_cast<unsigned>(record.sample.brightness));
  }

  if (!writer.raw("\"transition\",\"uptime_ms\":") ||
      !writer.format("%llu", static_cast<unsigned long long>(record.uptimeMs)) ||
      !writer.raw(",\"from_state\":")) {
    return false;
  }
  if (record.transition.hasFromState) {
    if (!writer.jsonString(
            presenceStateWireName(record.transition.fromState))) {
      return false;
    }
  } else if (!writer.raw("null")) {
    return false;
  }
  return writer.raw(",\"to_state\":") &&
         writer.jsonString(presenceStateWireName(record.transition.toState)) &&
         writer.raw(",\"reason\":") &&
         writer.jsonString(
             transitionReasonWireName(record.transition.reason)) &&
         writer.raw(",\"pir\":") &&
         writer.raw(record.transition.pir ? "true" : "false") &&
         writer.format(",\"pir_age_ms\":%llu,\"sound_active\":%s,"
                       "\"sound_age_ms\":%llu,\"mic_envelope\":%.3f,"
                       "\"noise_floor\":%.3f,\"sound_threshold\":%.3f,"
                       "\"brightness_before\":%u,\"brightness_after\":%u}",
                       static_cast<unsigned long long>(
                           record.transition.pirAgeMs),
                       record.transition.soundActive ? "true" : "false",
                       static_cast<unsigned long long>(
                           record.transition.soundAgeMs),
                       static_cast<double>(record.transition.micEnvelope),
                       static_cast<double>(record.transition.noiseFloor),
                       static_cast<double>(record.transition.soundThreshold),
                       static_cast<unsigned>(
                           record.transition.brightnessBefore),
                       static_cast<unsigned>(
                           record.transition.brightnessAfter));
}

}  // namespace

bool writeTelemetryBatchJson(const TelemetryBatchContext& context,
                             const TelemetryRecord* records,
                             size_t recordCount,
                             const TelemetryJsonSink& sink) {
  if (sink.write == nullptr || !validContext(context) ||
      !validRecords(records, recordCount, context.appliedConfigRevision)) {
    return false;
  }

  Writer writer(sink);
  if (!writer.raw("{\"schema_version\":1,\"batch_id\":") ||
      !writer.jsonString(context.batchId) || !writer.raw(",\"boot_id\":") ||
      !writer.jsonString(context.bootId) ||
      !writer.raw(",\"firmware_version\":") ||
      !writer.jsonString(context.firmwareVersion) ||
      !writer.raw(",\"build_id\":") || !writer.jsonString(context.buildId) ||
      !writer.format(",\"applied_config_revision\":%llu,\"clock_anchor\":",
                     static_cast<unsigned long long>(
                         context.appliedConfigRevision))) {
    return false;
  }

  if (context.hasClockAnchor) {
    if (!writer.format("{\"utc_ms\":%llu,\"uptime_ms\":%llu,\"source\":",
                       static_cast<unsigned long long>(context.anchorUtcMs),
                       static_cast<unsigned long long>(
                           context.anchorUptimeMs)) ||
        !writer.jsonString(context.anchorSource == ClockAnchorSource::kSntp
                              ? "sntp"
                              : "rtc") ||
        !writer.raw("}")) {
      return false;
    }
  } else if (!writer.raw("null")) {
    return false;
  }

  if (!writer.raw(",\"records\":[")) {
    return false;
  }
  for (size_t index = 0; index < recordCount; ++index) {
    if ((index > 0 && !writer.raw(",")) ||
        !writeRecord(writer, records[index])) {
      return false;
    }
  }
  return writer.raw("]}") && writer.ok();
}
