#include "operational_log_json.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

bool validIdentifier(const char* value, size_t minimum, size_t maximum,
                     bool allowColon) {
  if (value == nullptr) {
    return false;
  }
  const size_t length = std::strlen(value);
  if (length < minimum || length > maximum) {
    return false;
  }
  for (size_t index = 0; index < length; ++index) {
    const char byte = value[index];
    if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
        (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' ||
        byte == '-' || (allowColon && (byte == ':' || byte == '+'))) {
      continue;
    }
    return false;
  }
  return true;
}

const char* levelName(OperationalLogLevel level) {
  switch (level) {
    case OperationalLogLevel::kError:
      return "error";
    case OperationalLogLevel::kWarning:
      return "warning";
    case OperationalLogLevel::kInfo:
      return "info";
    case OperationalLogLevel::kDebug:
    case OperationalLogLevel::kSensorDetail:
      return "debug";
  }
  return nullptr;
}

const char* codeName(OperationalLogCode code) {
  switch (code) {
    case OperationalLogCode::kBoot:
      return "boot";
    case OperationalLogCode::kHealthChanged:
      return "health_changed";
    case OperationalLogCode::kPresenceTransition:
      return "presence_transition";
    case OperationalLogCode::kWifiChanged:
      return "wifi_changed";
    case OperationalLogCode::kBackendRequest:
      return "backend_request";
    case OperationalLogCode::kStorageChanged:
      return "storage_changed";
    case OperationalLogCode::kSensorChanged:
      return "sensor_changed";
    case OperationalLogCode::kRecoveryAction:
      return "recovery_action";
    case OperationalLogCode::kOtaChanged:
      return "ota_changed";
    case OperationalLogCode::kDebugSessionChanged:
      return "debug_session_changed";
    case OperationalLogCode::kCommandChanged:
      return "command_changed";
  }
  return nullptr;
}

class Writer {
 public:
  explicit Writer(const OperationalLogJsonSink& sink) : sink_(sink) {}

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
  bool format(const char* formatString, ...) {
    char buffer[256] = {};
    va_list arguments;
    va_start(arguments, formatString);
    const int length =
        std::vsnprintf(buffer, sizeof(buffer), formatString, arguments);
    va_end(arguments);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(buffer)) {
      ok_ = false;
      return false;
    }
    return bytes(buffer, static_cast<size_t>(length));
  }
  bool string(const char* value) {
    // Identifiers and enum names have already been character-validated.
    return raw("\"") && raw(value) && raw("\"");
  }
  bool ok() const { return ok_; }

 private:
  OperationalLogJsonSink sink_;
  bool ok_ = true;
};

bool validEvents(const OperationalLogEvent* events, size_t eventCount) {
  if (events == nullptr || eventCount == 0 ||
      eventCount > OperationalLogRing::kCapacity) {
    return false;
  }
  for (size_t index = 0; index < eventCount; ++index) {
    if (!operationalLogEventIsValid(events[index]) ||
        levelName(events[index].level) == nullptr ||
        codeName(events[index].code) == nullptr) {
      return false;
    }
    for (size_t previous = 0; previous < index; ++previous) {
      if (events[previous].sequence == events[index].sequence) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

bool writeOperationalLogBatchJson(const OperationalLogBatchContext& context,
                                  const OperationalLogEvent* events,
                                  size_t eventCount,
                                  const OperationalLogJsonSink& sink) {
  if (sink.write == nullptr ||
      !validIdentifier(context.batchId, 1, 96, true) ||
      !validIdentifier(context.bootId, 16, 64, false) ||
      !validIdentifier(context.buildId, 1, 128, true) ||
      !validEvents(events, eventCount)) {
    return false;
  }

  Writer writer(sink);
  if (!writer.raw("{\"schema_version\":1,\"batch_id\":") ||
      !writer.string(context.batchId) || !writer.raw(",\"boot_id\":") ||
      !writer.string(context.bootId) || !writer.raw(",\"build_id\":") ||
      !writer.string(context.buildId) || !writer.raw(",\"records\":[")) {
    return false;
  }

  for (size_t index = 0; index < eventCount; ++index) {
    const OperationalLogEvent& event = events[index];
    if ((index != 0 && !writer.raw(",")) ||
        !writer.format("{\"sequence\":%llu,\"uptime_ms\":%llu,\"level\":",
                       static_cast<unsigned long long>(event.sequence),
                       static_cast<unsigned long long>(event.uptimeMs)) ||
        !writer.string(levelName(event.level)) ||
        !writer.raw(",\"event_type\":") ||
        !writer.string(codeName(event.code)) ||
        !writer.format(",\"fields\":{\"value0\":%ld,\"value1\":%ld}}",
                       static_cast<long>(event.value0),
                       static_cast<long>(event.value1))) {
      return false;
    }
  }
  return writer.raw("]}") && writer.ok();
}
