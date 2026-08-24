#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>

#include "device_config.h"

namespace {

constexpr std::string_view kDeviceId = "core2-123456789abc";

constexpr std::string_view kDefaultConfigMembers =
    "\"minimum_on_ms\":10000,"
    "\"pir_hold_ms\":30000,"
    "\"sound_hold_ms\":12000,"
    "\"max_sound_bridge_ms\":300000,"
    "\"cooldown_ms\":5000,"
    "\"sound_factor\":1.12,"
    "\"telemetry_interval_ms\":1000,"
    "\"upload_batch_size\":30";

std::string responseWithConfig(std::string_view config,
                               std::string_view revision = "7",
                               std::string_view createdAt = "1800000000123",
                               std::string_view createdBy = "\"native-test\"") {
  std::string body = "{\"device_id\":\"";
  body.append(kDeviceId);
  body += "\",\"revision\":";
  body.append(revision);
  body += ",\"created_at_ms\":";
  body.append(createdAt);
  body += ",\"created_by\":";
  body.append(createdBy);
  body += ",\"config\":{";
  body.append(config);
  body += "}}";
  return body;
}

DeviceConfigParseResult parse(std::string_view body) {
  return parseDeviceConfigResponse(body.data(), body.size(), kDeviceId.data(),
                                   kDeviceId.size());
}

void expectError(
    std::string_view body, DeviceConfigParseError expected,
    PresenceConfigValidationError validation =
        PresenceConfigValidationError::kNone) {
  const DeviceConfigParseResult result = parse(body);
  assert(!result.ok());
  if (result.error != expected || result.validationError != validation) {
    std::fprintf(stderr, "expected %s/%s, got %s/%s for %.*s\n",
                 deviceConfigParseErrorName(expected),
                 presenceConfigValidationErrorName(validation),
                 deviceConfigParseErrorName(result.error),
                 presenceConfigValidationErrorName(result.validationError),
                 static_cast<int>(body.size()), body.data());
  }
  assert(result.error == expected);
  assert(result.validationError == validation);

  // A rejected response must never leak a partially parsed config to callers.
  const PresenceConfig defaults = defaultPresenceConfig();
  assert(result.config.revision == defaults.revision);
  assert(result.config.pirHoldMs == defaults.pirHoldMs);
  assert(result.config.uploadBatchSize == defaults.uploadBatchSize);
}

void testDefaultsAndPodLayout() {
  static_assert(std::is_trivial<PresenceConfig>::value,
                "PresenceConfig must remain a POD");
  static_assert(std::is_standard_layout<PresenceConfig>::value,
                "PresenceConfig must remain a POD");

  const PresenceConfig config = defaultPresenceConfig();
  assert(config.revision == 0);
  assert(config.minimumOnMs == 10000);
  assert(config.pirHoldMs == 30000);
  assert(config.soundHoldMs == 12000);
  assert(config.maxSoundBridgeMs == 300000);
  assert(config.cooldownMs == 5000);
  assert(std::fabs(config.soundFactor - 1.12f) < 0.0001f);
  assert(config.telemetryIntervalMs == 1000);
  assert(config.uploadBatchSize == 30);
  assert(validatePresenceConfig(config) ==
         PresenceConfigValidationError::kNone);
  assert(validatePresenceConfigDeviceCapabilities(config) ==
         PresenceConfigCapabilityError::kNone);
}

void testCompleteAndDefaultedResponses() {
  const DeviceConfigParseResult complete = parse(responseWithConfig(
      "\"minimum_on_ms\":600000,"
      "\"pir_hold_ms\":1000,"
      "\"sound_hold_ms\":0,"
      "\"max_sound_bridge_ms\":3600000,"
      "\"cooldown_ms\":600000,"
      "\"sound_factor\":4,"
      "\"telemetry_interval_ms\":250,"
      "\"upload_batch_size\":30",
      "9223372036854775807", "-9223372036854775808",
      "\"operator \\uD83D\\uDE80\""));
  assert(complete.ok());
  assert(complete.config.revision == 9223372036854775807ULL);
  assert(complete.config.minimumOnMs == 600000);
  assert(complete.config.pirHoldMs == 1000);
  assert(complete.config.soundHoldMs == 0);
  assert(complete.config.maxSoundBridgeMs == 3600000);
  assert(complete.config.cooldownMs == 600000);
  assert(complete.config.soundFactor == 4.0f);
  assert(complete.config.telemetryIntervalMs == 250);
  assert(complete.config.uploadBatchSize == 30);
  assert(validatePresenceConfig(complete.config) ==
         PresenceConfigValidationError::kNone);
  assert(validatePresenceConfigDeviceCapabilities(complete.config) ==
         PresenceConfigCapabilityError::kNone);

  const DeviceConfigParseResult allDefaults =
      parse(responseWithConfig(kDefaultConfigMembers, "0", "null", "null"));
  assert(allDefaults.ok());
  const PresenceConfig defaults = defaultPresenceConfig();
  assert(allDefaults.config.revision == 0);
  assert(allDefaults.config.minimumOnMs == defaults.minimumOnMs);
  assert(allDefaults.config.pirHoldMs == defaults.pirHoldMs);
  assert(allDefaults.config.soundHoldMs == defaults.soundHoldMs);
  assert(allDefaults.config.maxSoundBridgeMs ==
         defaults.maxSoundBridgeMs);
  assert(allDefaults.config.cooldownMs == defaults.cooldownMs);
  assert(allDefaults.config.soundFactor == defaults.soundFactor);
  assert(allDefaults.config.telemetryIntervalMs ==
         defaults.telemetryIntervalMs);
  assert(allDefaults.config.uploadBatchSize == defaults.uploadBatchSize);

  expectError(responseWithConfig(
                  "\"sound_factor\":1e0,\"pir_hold_ms\":45000", "12"),
              DeviceConfigParseError::kMissingField);
}

void testOrderingWhitespaceAndEscapes() {
  const std::string body =
      " \n{"
      "\"config\":{"
      "\"minimum_on_ms\":10000,"
      "\"pir\\u005fhold_ms\":30001,"
      "\"sound_hold_ms\":12000,"
      "\"max_sound_bridge_ms\":300000,"
      "\"cooldown_ms\":5000,"
      "\"sound_factor\":1.12,"
      "\"telemetry_interval_ms\":1000,"
      "\"upload_batch_size\":30},"
      "\"created_by\":\"caf\xc3\xa9\\nowner\","
      "\"created_at_ms\":null,"
      "\"revi\\u0073ion\":9,"
      "\"device\\u005fid\":\"core2-123456789ab\\u0063\""
      "}\r\n";
  const DeviceConfigParseResult result = parse(body);
  assert(result.ok());
  assert(result.config.revision == 9);
  assert(result.config.pirHoldMs == 30001);
}

void testTopLevelShapeAndIdentityFailures() {
  expectError("[]", DeviceConfigParseError::kTopLevelNotObject);
  expectError("{}", DeviceConfigParseError::kMissingField);
  expectError(
      R"({"device_id":"core2-123456789abc","revision":0,"created_at_ms":null,"created_by":null})",
      DeviceConfigParseError::kMissingField);
  expectError(
      responseWithConfig(kDefaultConfigMembers).substr(
          0, responseWithConfig(kDefaultConfigMembers).size() - 1) +
          ",\"future\":1}",
      DeviceConfigParseError::kUnknownField);
  expectError(
      R"({"device_id":"core2-123456789abc","device\u005fid":"core2-123456789abc","revision":0,"created_at_ms":null,"created_by":null,"config":{}})",
      DeviceConfigParseError::kDuplicateField);
  expectError(
      R"({"device_id":"core2-000000000000","revision":0,"created_at_ms":null,"created_by":null,"config":{}})",
      DeviceConfigParseError::kDeviceIdMismatch);
  expectError(
      R"({"device_id":7,"revision":0,"created_at_ms":null,"created_by":null,"config":{}})",
      DeviceConfigParseError::kWrongType);
  expectError(responseWithConfig(kDefaultConfigMembers) + "garbage",
              DeviceConfigParseError::kTrailingData);

  const DeviceConfigParseResult nullBody = parseDeviceConfigResponse(
      nullptr, 0, kDeviceId.data(), kDeviceId.size());
  assert(!nullBody.ok());
  assert(nullBody.error == DeviceConfigParseError::kTopLevelNotObject);
  const DeviceConfigParseResult badBodyPointer = parseDeviceConfigResponse(
      nullptr, 1, kDeviceId.data(), kDeviceId.size());
  assert(!badBodyPointer.ok());
  assert(badBodyPointer.error == DeviceConfigParseError::kNullArgument);
  const std::string valid = responseWithConfig("");
  const DeviceConfigParseResult badIdPointer = parseDeviceConfigResponse(
      valid.data(), valid.size(), nullptr, 1);
  assert(!badIdPointer.ok());
  assert(badIdPointer.error == DeviceConfigParseError::kNullArgument);
}

void testStrictMemberAndTypeFailures() {
  expectError(responseWithConfig("\"future\":1"),
              DeviceConfigParseError::kUnknownField);
  expectError(responseWithConfig(
                  "\"pir_hold_ms\":30000,"
                  "\"pir\\u005fhold_ms\":30000"),
              DeviceConfigParseError::kDuplicateField);
  expectError(responseWithConfig("\"pir_hold_ms\":\"30000\""),
              DeviceConfigParseError::kWrongType);
  expectError(responseWithConfig("\"pir_hold_ms\":30000.0"),
              DeviceConfigParseError::kWrongType);
  expectError(responseWithConfig("\"pir_hold_ms\":3e4"),
              DeviceConfigParseError::kWrongType);
  expectError(responseWithConfig("\"sound_factor\":\"1.12\""),
              DeviceConfigParseError::kWrongType);
  expectError(responseWithConfig("\"sound_factor\":true"),
              DeviceConfigParseError::kWrongType);
  expectError(responseWithConfig("", "false"),
              DeviceConfigParseError::kWrongType);
  expectError(responseWithConfig("", "1.0"),
              DeviceConfigParseError::kWrongType);
  expectError(responseWithConfig("", "0", "1.5"),
              DeviceConfigParseError::kWrongType);
  expectError(responseWithConfig("", "0", "null", "4"),
              DeviceConfigParseError::kWrongType);

  std::string configArray = responseWithConfig("");
  const std::string needle = "\"config\":{}";
  configArray.replace(configArray.find(needle), needle.size(),
                      "\"config\":[]");
  expectError(configArray, DeviceConfigParseError::kWrongType);
}

void testMalformedJsonAndStrings() {
  expectError(responseWithConfig("\"pir_hold_ms\":030000"),
              DeviceConfigParseError::kMalformedJson);
  expectError(responseWithConfig("\"sound_factor\":1."),
              DeviceConfigParseError::kMalformedJson);
  expectError(responseWithConfig("\"sound_factor\":1e"),
              DeviceConfigParseError::kMalformedJson);
  expectError(
      R"({"device_id":"core2-123456789abc","revision":0,"created_at_ms":null,"created_by":"\uD800","config":{}})",
      DeviceConfigParseError::kMalformedJson);
  expectError(
      R"({"device_id":"core2-123456789abc","revision":0,"created_at_ms":null,"created_by":"\uDC00","config":{}})",
      DeviceConfigParseError::kMalformedJson);
  expectError(
      R"({"device_id":"core2-123456789abc","revision":0,"created_at_ms":null,"created_by":"\q","config":{}})",
      DeviceConfigParseError::kMalformedJson);
  std::string trailingComma = responseWithConfig(kDefaultConfigMembers);
  trailingComma.insert(trailingComma.size() - 1, ",");
  expectError(trailingComma, DeviceConfigParseError::kMalformedJson);

  std::string invalidUtf8 =
      R"({"device_id":"core2-123456789abc","revision":0,"created_at_ms":null,"created_by":")";
  invalidUtf8.push_back(static_cast<char>(0xc0));
  invalidUtf8 += R"(","config":{}})";
  expectError(invalidUtf8, DeviceConfigParseError::kMalformedJson);
}

void testNumericBoundsAndOverflow() {
  expectError(responseWithConfig("", "-1"),
              DeviceConfigParseError::kValueOutOfRange,
              PresenceConfigValidationError::kRevisionOutOfRange);
  expectError(responseWithConfig("", "9223372036854775808"),
              DeviceConfigParseError::kValueOutOfRange,
              PresenceConfigValidationError::kRevisionOutOfRange);
  expectError(responseWithConfig("", "18446744073709551616"),
              DeviceConfigParseError::kIntegerOverflow);
  expectError(responseWithConfig("", "0", "9223372036854775808"),
              DeviceConfigParseError::kIntegerOverflow);
  expectError(responseWithConfig("", "0", "-9223372036854775809"),
              DeviceConfigParseError::kIntegerOverflow);

  expectError(responseWithConfig("\"minimum_on_ms\":600001"),
              DeviceConfigParseError::kValueOutOfRange,
              PresenceConfigValidationError::kMinimumOnMsOutOfRange);
  expectError(responseWithConfig("\"pir_hold_ms\":999"),
              DeviceConfigParseError::kValueOutOfRange,
              PresenceConfigValidationError::kPirHoldMsOutOfRange);
  expectError(responseWithConfig("\"pir_hold_ms\":3600001"),
              DeviceConfigParseError::kValueOutOfRange,
              PresenceConfigValidationError::kPirHoldMsOutOfRange);
  expectError(responseWithConfig("\"sound_hold_ms\":600001"),
              DeviceConfigParseError::kValueOutOfRange,
              PresenceConfigValidationError::kSoundHoldMsOutOfRange);
  expectError(responseWithConfig("\"max_sound_bridge_ms\":3600001"),
              DeviceConfigParseError::kValueOutOfRange,
              PresenceConfigValidationError::kMaxSoundBridgeMsOutOfRange);
  expectError(responseWithConfig("\"cooldown_ms\":600001"),
              DeviceConfigParseError::kValueOutOfRange,
              PresenceConfigValidationError::kCooldownMsOutOfRange);
  expectError(responseWithConfig("\"telemetry_interval_ms\":249"),
              DeviceConfigParseError::kValueOutOfRange,
              PresenceConfigValidationError::kTelemetryIntervalMsOutOfRange);
  expectError(responseWithConfig("\"telemetry_interval_ms\":60001"),
              DeviceConfigParseError::kValueOutOfRange,
              PresenceConfigValidationError::kTelemetryIntervalMsOutOfRange);
  expectError(responseWithConfig("\"upload_batch_size\":0"),
              DeviceConfigParseError::kValueOutOfRange,
              PresenceConfigValidationError::kUploadBatchSizeOutOfRange);
  expectError(responseWithConfig("\"upload_batch_size\":31"),
              DeviceConfigParseError::kValueOutOfRange,
              PresenceConfigValidationError::kUploadBatchSizeOutOfRange);

  expectError(responseWithConfig("\"sound_factor\":0.999"),
              DeviceConfigParseError::kValueOutOfRange,
              PresenceConfigValidationError::kSoundFactorOutOfRange);
  expectError(responseWithConfig("\"sound_factor\":4.001"),
              DeviceConfigParseError::kValueOutOfRange,
              PresenceConfigValidationError::kSoundFactorOutOfRange);
  expectError(responseWithConfig("\"sound_factor\":NaN"),
              DeviceConfigParseError::kNonFiniteNumber,
              PresenceConfigValidationError::kSoundFactorNotFinite);
  expectError(responseWithConfig("\"sound_factor\":Infinity"),
              DeviceConfigParseError::kNonFiniteNumber,
              PresenceConfigValidationError::kSoundFactorNotFinite);
  expectError(responseWithConfig("\"sound_factor\":-Infinity"),
              DeviceConfigParseError::kNonFiniteNumber,
              PresenceConfigValidationError::kSoundFactorNotFinite);
  expectError(responseWithConfig("\"sound_factor\":1e999"),
              DeviceConfigParseError::kNumberOverflow);
}

void testStandaloneValidationAndCapabilityBoundary() {
  PresenceConfig config = defaultPresenceConfig();

  config.revision = 1ULL << 63U;
  assert(validatePresenceConfig(config) ==
         PresenceConfigValidationError::kRevisionOutOfRange);
  config = defaultPresenceConfig();
  config.minimumOnMs = 600001;
  assert(validatePresenceConfig(config) ==
         PresenceConfigValidationError::kMinimumOnMsOutOfRange);
  config = defaultPresenceConfig();
  config.pirHoldMs = 999;
  assert(validatePresenceConfig(config) ==
         PresenceConfigValidationError::kPirHoldMsOutOfRange);
  config = defaultPresenceConfig();
  config.soundHoldMs = 600001;
  assert(validatePresenceConfig(config) ==
         PresenceConfigValidationError::kSoundHoldMsOutOfRange);
  config = defaultPresenceConfig();
  config.maxSoundBridgeMs = 3600001;
  assert(validatePresenceConfig(config) ==
         PresenceConfigValidationError::kMaxSoundBridgeMsOutOfRange);
  config = defaultPresenceConfig();
  config.cooldownMs = 600001;
  assert(validatePresenceConfig(config) ==
         PresenceConfigValidationError::kCooldownMsOutOfRange);
  config = defaultPresenceConfig();
  config.soundFactor = std::numeric_limits<float>::quiet_NaN();
  assert(validatePresenceConfig(config) ==
         PresenceConfigValidationError::kSoundFactorNotFinite);
  config.soundFactor = 0.99f;
  assert(validatePresenceConfig(config) ==
         PresenceConfigValidationError::kSoundFactorOutOfRange);
  config = defaultPresenceConfig();
  config.telemetryIntervalMs = 249;
  assert(validatePresenceConfig(config) ==
         PresenceConfigValidationError::kTelemetryIntervalMsOutOfRange);
  config = defaultPresenceConfig();
  config.uploadBatchSize = 0;
  assert(validatePresenceConfig(config) ==
         PresenceConfigValidationError::kUploadBatchSizeOutOfRange);

  config = defaultPresenceConfig();
  config.uploadBatchSize = kDeviceTelemetryBatchCapacity;
  assert(validatePresenceConfig(config) ==
         PresenceConfigValidationError::kNone);
  assert(validatePresenceConfigDeviceCapabilities(config) ==
         PresenceConfigCapabilityError::kNone);
  config.uploadBatchSize = kDeviceTelemetryBatchCapacity + 1;
  assert(validatePresenceConfig(config) == PresenceConfigValidationError::
                                               kUploadBatchSizeOutOfRange);
  assert(validatePresenceConfigDeviceCapabilities(config) ==
         PresenceConfigCapabilityError::
             kUploadBatchSizeExceedsDeviceCapacity);
}

void testErrorNames() {
  assert(std::string_view(deviceConfigParseErrorName(
             DeviceConfigParseError::kDeviceIdMismatch)) ==
         "device_id_mismatch");
  assert(std::string_view(presenceConfigValidationErrorName(
             PresenceConfigValidationError::kSoundFactorNotFinite)) ==
         "sound_factor_not_finite");
  assert(std::string_view(presenceConfigCapabilityErrorName(
             PresenceConfigCapabilityError::
                 kUploadBatchSizeExceedsDeviceCapacity)) ==
         "upload_batch_size_exceeds_device_capacity");
}

}  // namespace

int main() {
  testDefaultsAndPodLayout();
  testCompleteAndDefaultedResponses();
  testOrderingWhitespaceAndEscapes();
  testTopLevelShapeAndIdentityFailures();
  testStrictMemberAndTypeFailures();
  testMalformedJsonAndStrings();
  testNumericBoundsAndOverflow();
  testStandaloneValidationAndCapabilityBoundary();
  testErrorNames();
  return 0;
}
