#pragma once

#include <cstddef>
#include <cstdint>

// Runtime configuration delivered by GET /v1/devices/{device_id}/config.
// Keep this aggregate independent of Arduino so parsing and validation can be
// exercised on the host. JSON metadata that is not needed by the state machine
// (created_at_ms and created_by) is validated but deliberately not retained.
struct PresenceConfig {
  uint64_t revision;
  uint32_t minimumOnMs;
  uint32_t pirHoldMs;
  uint32_t soundHoldMs;
  uint32_t maxSoundBridgeMs;
  uint32_t cooldownMs;
  float soundFactor;
  uint32_t telemetryIntervalMs;
  uint16_t uploadBatchSize;
};

// The API ceiling is intentionally aligned with the device's immutable
// queue/spool serializer capacity. The separate capability check makes that
// invariant explicit at the application boundary; nothing is silently
// clamped if either side changes later.
constexpr uint16_t kPresenceConfigContractMaxUploadBatchSize = 30;
constexpr uint16_t kDeviceTelemetryBatchCapacity = 30;

enum class PresenceConfigValidationError : uint8_t {
  kNone,
  kRevisionOutOfRange,
  kMinimumOnMsOutOfRange,
  kPirHoldMsOutOfRange,
  kSoundHoldMsOutOfRange,
  kMaxSoundBridgeMsOutOfRange,
  kCooldownMsOutOfRange,
  kSoundFactorNotFinite,
  kSoundFactorOutOfRange,
  kTelemetryIntervalMsOutOfRange,
  kUploadBatchSizeOutOfRange,
};

// These platform defaults mirror PresenceService's revision-zero response;
// validation bounds mirror backend presence_api.schemas.PresenceConfig. There
// are no extra cross-field rules.
PresenceConfig defaultPresenceConfig();
PresenceConfigValidationError validatePresenceConfig(
    const PresenceConfig& config);
const char* presenceConfigValidationErrorName(
    PresenceConfigValidationError error);

enum class PresenceConfigCapabilityError : uint8_t {
  kNone,
  kUploadBatchSizeExceedsDeviceCapacity,
};

PresenceConfigCapabilityError validatePresenceConfigDeviceCapabilities(
    const PresenceConfig& config);
const char* presenceConfigCapabilityErrorName(
    PresenceConfigCapabilityError error);

enum class DeviceConfigParseError : uint8_t {
  kNone,
  kNullArgument,
  kTopLevelNotObject,
  kMalformedJson,
  kMissingField,
  kDuplicateField,
  kUnknownField,
  kWrongType,
  kIntegerOverflow,
  kNonFiniteNumber,
  kNumberOverflow,
  kValueOutOfRange,
  kDeviceIdMismatch,
  kTrailingData,
};

struct DeviceConfigParseResult {
  PresenceConfig config;
  DeviceConfigParseError error;
  PresenceConfigValidationError validationError;

  bool ok() const { return error == DeviceConfigParseError::kNone; }
  explicit operator bool() const { return ok(); }
};

// Parses a complete ConfigResponse and verifies that device_id exactly matches
// the device whose endpoint was requested. ConfigResponse's five top-level
// fields and all eight PresenceConfig members are required. Unknown and
// duplicate members at either level are rejected. The returned config is reset
// to defaultPresenceConfig() on any failure, so partial data cannot be applied.
DeviceConfigParseResult parseDeviceConfigResponse(
    const char* responseBody, size_t responseBodyLength,
    const char* expectedDeviceId, size_t expectedDeviceIdLength);

const char* deviceConfigParseErrorName(DeviceConfigParseError error);
