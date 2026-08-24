#include "device_config.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {

constexpr uint64_t kMaxSigned64 =
    static_cast<uint64_t>(std::numeric_limits<int64_t>::max());

constexpr uint8_t kDeviceIdBit = 1U << 0;
constexpr uint8_t kRevisionBit = 1U << 1;
constexpr uint8_t kCreatedAtMsBit = 1U << 2;
constexpr uint8_t kCreatedByBit = 1U << 3;
constexpr uint8_t kConfigBit = 1U << 4;
constexpr uint8_t kAllTopLevelFields =
    kDeviceIdBit | kRevisionBit | kCreatedAtMsBit | kCreatedByBit |
    kConfigBit;

constexpr uint8_t kMinimumOnMsBit = 1U << 0;
constexpr uint8_t kPirHoldMsBit = 1U << 1;
constexpr uint8_t kSoundHoldMsBit = 1U << 2;
constexpr uint8_t kMaxSoundBridgeMsBit = 1U << 3;
constexpr uint8_t kCooldownMsBit = 1U << 4;
constexpr uint8_t kSoundFactorBit = 1U << 5;
constexpr uint8_t kTelemetryIntervalMsBit = 1U << 6;
constexpr uint8_t kUploadBatchSizeBit = 1U << 7;
constexpr uint8_t kAllConfigFields =
    kMinimumOnMsBit | kPirHoldMsBit | kSoundHoldMsBit |
    kMaxSoundBridgeMsBit | kCooldownMsBit | kSoundFactorBit |
    kTelemetryIntervalMsBit | kUploadBatchSizeBit;

struct DecodedString {
  char bytes[65];
  size_t length;
  bool truncated;
};

struct NumberToken {
  size_t start;
  size_t end;
  bool negative;
  bool integer;
};

bool textEquals(const char* actual, size_t actualLength,
                const char* expected, size_t expectedLength) {
  return actualLength == expectedLength &&
         (actualLength == 0 ||
          std::memcmp(actual, expected, actualLength) == 0);
}

class ConfigResponseParser {
 public:
  ConfigResponseParser(const char* input, size_t inputLength,
                       const char* expectedDeviceId,
                       size_t expectedDeviceIdLength)
      : input_(input),
        inputLength_(inputLength),
        expectedDeviceId_(expectedDeviceId),
        expectedDeviceIdLength_(expectedDeviceIdLength),
        config_(defaultPresenceConfig()) {}

  DeviceConfigParseResult parse() {
    if ((input_ == nullptr && inputLength_ != 0) ||
        (expectedDeviceId_ == nullptr && expectedDeviceIdLength_ != 0)) {
      return failure(DeviceConfigParseError::kNullArgument);
    }

    skipWhitespace();
    if (!consume('{')) {
      return failure(DeviceConfigParseError::kTopLevelNotObject);
    }
    skipWhitespace();
    if (consume('}')) {
      return failure(DeviceConfigParseError::kMissingField);
    }

    while (true) {
      DecodedString key = {};
      if (!parseString(key, DeviceConfigParseError::kMalformedJson)) {
        return failure(error_);
      }
      skipWhitespace();
      if (!consume(':')) {
        return failure(DeviceConfigParseError::kMalformedJson);
      }
      skipWhitespace();
      if (!parseTopLevelField(key)) {
        return failure(error_, validationError_);
      }

      skipWhitespace();
      if (consume('}')) {
        break;
      }
      if (!consume(',')) {
        return failure(DeviceConfigParseError::kMalformedJson);
      }
      skipWhitespace();
    }

    skipWhitespace();
    if (position_ != inputLength_) {
      return failure(DeviceConfigParseError::kTrailingData);
    }
    if (seenTopLevelFields_ != kAllTopLevelFields) {
      return failure(DeviceConfigParseError::kMissingField);
    }

    const PresenceConfigValidationError validation =
        validatePresenceConfig(config_);
    if (validation != PresenceConfigValidationError::kNone) {
      return failure(DeviceConfigParseError::kValueOutOfRange, validation);
    }
    return {config_, DeviceConfigParseError::kNone,
            PresenceConfigValidationError::kNone};
  }

 private:
  bool parseTopLevelField(const DecodedString& key) {
    uint8_t bit = 0;
    if (keyEquals(key, "device_id")) {
      bit = kDeviceIdBit;
    } else if (keyEquals(key, "revision")) {
      bit = kRevisionBit;
    } else if (keyEquals(key, "created_at_ms")) {
      bit = kCreatedAtMsBit;
    } else if (keyEquals(key, "created_by")) {
      bit = kCreatedByBit;
    } else if (keyEquals(key, "config")) {
      bit = kConfigBit;
    } else {
      return fail(DeviceConfigParseError::kUnknownField);
    }

    if ((seenTopLevelFields_ & bit) != 0) {
      return fail(DeviceConfigParseError::kDuplicateField);
    }
    seenTopLevelFields_ |= bit;

    if (bit == kDeviceIdBit) {
      DecodedString deviceId = {};
      if (!parseString(deviceId, DeviceConfigParseError::kWrongType)) {
        return false;
      }
      if (deviceId.truncated || expectedDeviceIdLength_ > sizeof(deviceId.bytes) ||
          !textEquals(deviceId.bytes, deviceId.length, expectedDeviceId_,
                      expectedDeviceIdLength_)) {
        return fail(DeviceConfigParseError::kDeviceIdMismatch);
      }
      return true;
    }
    if (bit == kRevisionBit) {
      return parseBoundedUnsigned(config_.revision, 0, kMaxSigned64,
                                  PresenceConfigValidationError::
                                      kRevisionOutOfRange);
    }
    if (bit == kCreatedAtMsBit) {
      if (consumeLiteral("null")) {
        return true;
      }
      return parseDiscardedSignedInteger();
    }
    if (bit == kCreatedByBit) {
      if (consumeLiteral("null")) {
        return true;
      }
      DecodedString ignored = {};
      return parseString(ignored, DeviceConfigParseError::kWrongType);
    }
    return parseConfigObject();
  }

  bool parseConfigObject() {
    if (!consume('{')) {
      return fail(DeviceConfigParseError::kWrongType);
    }
    skipWhitespace();
    if (consume('}')) {
      return fail(DeviceConfigParseError::kMissingField);
    }

    while (true) {
      DecodedString key = {};
      if (!parseString(key, DeviceConfigParseError::kMalformedJson)) {
        return false;
      }
      skipWhitespace();
      if (!consume(':')) {
        return fail(DeviceConfigParseError::kMalformedJson);
      }
      skipWhitespace();
      if (!parseConfigField(key)) {
        return false;
      }

      skipWhitespace();
      if (consume('}')) {
        if (seenConfigFields_ != kAllConfigFields) {
          return fail(DeviceConfigParseError::kMissingField);
        }
        return true;
      }
      if (!consume(',')) {
        return fail(DeviceConfigParseError::kMalformedJson);
      }
      skipWhitespace();
    }
  }

  bool parseConfigField(const DecodedString& key) {
    uint8_t bit = 0;
    PresenceConfigValidationError validation =
        PresenceConfigValidationError::kNone;
    if (keyEquals(key, "minimum_on_ms")) {
      bit = kMinimumOnMsBit;
      validation =
          PresenceConfigValidationError::kMinimumOnMsOutOfRange;
    } else if (keyEquals(key, "pir_hold_ms")) {
      bit = kPirHoldMsBit;
      validation = PresenceConfigValidationError::kPirHoldMsOutOfRange;
    } else if (keyEquals(key, "sound_hold_ms")) {
      bit = kSoundHoldMsBit;
      validation = PresenceConfigValidationError::kSoundHoldMsOutOfRange;
    } else if (keyEquals(key, "max_sound_bridge_ms")) {
      bit = kMaxSoundBridgeMsBit;
      validation =
          PresenceConfigValidationError::kMaxSoundBridgeMsOutOfRange;
    } else if (keyEquals(key, "cooldown_ms")) {
      bit = kCooldownMsBit;
      validation = PresenceConfigValidationError::kCooldownMsOutOfRange;
    } else if (keyEquals(key, "sound_factor")) {
      bit = kSoundFactorBit;
      validation = PresenceConfigValidationError::kSoundFactorOutOfRange;
    } else if (keyEquals(key, "telemetry_interval_ms")) {
      bit = kTelemetryIntervalMsBit;
      validation =
          PresenceConfigValidationError::kTelemetryIntervalMsOutOfRange;
    } else if (keyEquals(key, "upload_batch_size")) {
      bit = kUploadBatchSizeBit;
      validation =
          PresenceConfigValidationError::kUploadBatchSizeOutOfRange;
    } else {
      return fail(DeviceConfigParseError::kUnknownField);
    }

    if ((seenConfigFields_ & bit) != 0) {
      return fail(DeviceConfigParseError::kDuplicateField);
    }
    seenConfigFields_ |= bit;

    uint64_t parsed = 0;
    if (bit == kMinimumOnMsBit) {
      if (!parseBoundedUnsigned(parsed, 0, 600000, validation)) {
        return false;
      }
      config_.minimumOnMs = static_cast<uint32_t>(parsed);
      return true;
    }
    if (bit == kPirHoldMsBit) {
      if (!parseBoundedUnsigned(parsed, 1000, 3600000, validation)) {
        return false;
      }
      config_.pirHoldMs = static_cast<uint32_t>(parsed);
      return true;
    }
    if (bit == kSoundHoldMsBit) {
      if (!parseBoundedUnsigned(parsed, 0, 600000, validation)) {
        return false;
      }
      config_.soundHoldMs = static_cast<uint32_t>(parsed);
      return true;
    }
    if (bit == kMaxSoundBridgeMsBit) {
      if (!parseBoundedUnsigned(parsed, 0, 3600000, validation)) {
        return false;
      }
      config_.maxSoundBridgeMs = static_cast<uint32_t>(parsed);
      return true;
    }
    if (bit == kCooldownMsBit) {
      if (!parseBoundedUnsigned(parsed, 0, 600000, validation)) {
        return false;
      }
      config_.cooldownMs = static_cast<uint32_t>(parsed);
      return true;
    }
    if (bit == kSoundFactorBit) {
      double value = 0.0;
      if (!parseNumber(value)) {
        if (error_ == DeviceConfigParseError::kNonFiniteNumber) {
          validationError_ =
              PresenceConfigValidationError::kSoundFactorNotFinite;
        }
        return false;
      }
      if (!std::isfinite(value)) {
        validationError_ =
            PresenceConfigValidationError::kSoundFactorNotFinite;
        return fail(DeviceConfigParseError::kNonFiniteNumber);
      }
      if (value < 1.0 || value > 4.0) {
        validationError_ = validation;
        return fail(DeviceConfigParseError::kValueOutOfRange);
      }
      config_.soundFactor = static_cast<float>(value);
      return true;
    }
    if (bit == kTelemetryIntervalMsBit) {
      if (!parseBoundedUnsigned(parsed, 250, 60000, validation)) {
        return false;
      }
      config_.telemetryIntervalMs = static_cast<uint32_t>(parsed);
      return true;
    }
    if (!parseBoundedUnsigned(
            parsed, 1, kPresenceConfigContractMaxUploadBatchSize,
            validation)) {
      return false;
    }
    config_.uploadBatchSize = static_cast<uint16_t>(parsed);
    return true;
  }

  bool parseBoundedUnsigned(uint64_t& destination, uint64_t minimum,
                            uint64_t maximum,
                            PresenceConfigValidationError validation) {
    NumberToken token = {};
    if (!scanNumber(token)) {
      return false;
    }
    if (!token.integer) {
      return fail(DeviceConfigParseError::kWrongType);
    }

    size_t cursor = token.start + (token.negative ? 1U : 0U);
    uint64_t value = 0;
    while (cursor < token.end) {
      const uint8_t digit = static_cast<uint8_t>(input_[cursor] - '0');
      if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10U) {
        return fail(DeviceConfigParseError::kIntegerOverflow);
      }
      value = value * 10U + digit;
      ++cursor;
    }
    if ((token.negative && value != 0) || value < minimum || value > maximum) {
      validationError_ = validation;
      return fail(DeviceConfigParseError::kValueOutOfRange);
    }
    destination = value;
    return true;
  }

  bool parseDiscardedSignedInteger() {
    NumberToken token = {};
    if (!scanNumber(token)) {
      return false;
    }
    if (!token.integer) {
      return fail(DeviceConfigParseError::kWrongType);
    }

    size_t cursor = token.start + (token.negative ? 1U : 0U);
    uint64_t magnitude = 0;
    while (cursor < token.end) {
      const uint8_t digit = static_cast<uint8_t>(input_[cursor] - '0');
      if (magnitude >
          (std::numeric_limits<uint64_t>::max() - digit) / 10U) {
        return fail(DeviceConfigParseError::kIntegerOverflow);
      }
      magnitude = magnitude * 10U + digit;
      ++cursor;
    }
    const uint64_t maximum = token.negative ? kMaxSigned64 + 1U : kMaxSigned64;
    if (magnitude > maximum) {
      return fail(DeviceConfigParseError::kIntegerOverflow);
    }
    return true;
  }

  bool parseNumber(double& value) {
    NumberToken token = {};
    if (!scanNumber(token)) {
      return false;
    }
    const size_t tokenLength = token.end - token.start;
    char buffer[128] = {};
    if (tokenLength >= sizeof(buffer)) {
      return fail(DeviceConfigParseError::kNumberOverflow);
    }
    std::memcpy(buffer, input_ + token.start, tokenLength);
    buffer[tokenLength] = '\0';

    errno = 0;
    char* end = nullptr;
    const double parsed = std::strtod(buffer, &end);
    if (end != buffer + tokenLength) {
      return fail(DeviceConfigParseError::kMalformedJson);
    }
    if (!std::isfinite(parsed)) {
      return fail(DeviceConfigParseError::kNumberOverflow);
    }
    if (errno == ERANGE && parsed != 0.0) {
      return fail(DeviceConfigParseError::kNumberOverflow);
    }
    value = parsed;
    return true;
  }

  bool scanNumber(NumberToken& token) {
    if (startsWith("NaN") || startsWith("Infinity") ||
        startsWith("-Infinity")) {
      return fail(DeviceConfigParseError::kNonFiniteNumber);
    }
    if (position_ >= inputLength_ ||
        (input_[position_] != '-' && !isDigit(input_[position_]))) {
      return fail(DeviceConfigParseError::kWrongType);
    }

    token.start = position_;
    token.negative = consume('-');
    token.integer = true;
    if (position_ >= inputLength_ || !isDigit(input_[position_])) {
      return fail(DeviceConfigParseError::kMalformedJson);
    }
    if (input_[position_] == '0') {
      ++position_;
      if (position_ < inputLength_ && isDigit(input_[position_])) {
        return fail(DeviceConfigParseError::kMalformedJson);
      }
    } else {
      while (position_ < inputLength_ && isDigit(input_[position_])) {
        ++position_;
      }
    }

    if (position_ < inputLength_ && input_[position_] == '.') {
      token.integer = false;
      ++position_;
      if (position_ >= inputLength_ || !isDigit(input_[position_])) {
        return fail(DeviceConfigParseError::kMalformedJson);
      }
      while (position_ < inputLength_ && isDigit(input_[position_])) {
        ++position_;
      }
    }
    if (position_ < inputLength_ &&
        (input_[position_] == 'e' || input_[position_] == 'E')) {
      token.integer = false;
      ++position_;
      if (position_ < inputLength_ &&
          (input_[position_] == '+' || input_[position_] == '-')) {
        ++position_;
      }
      if (position_ >= inputLength_ || !isDigit(input_[position_])) {
        return fail(DeviceConfigParseError::kMalformedJson);
      }
      while (position_ < inputLength_ && isDigit(input_[position_])) {
        ++position_;
      }
    }
    token.end = position_;
    return true;
  }

  bool parseString(DecodedString& decoded,
                   DeviceConfigParseError nonStringError) {
    if (!consume('"')) {
      return fail(nonStringError);
    }
    decoded.length = 0;
    decoded.truncated = false;

    while (position_ < inputLength_) {
      const uint8_t byte = static_cast<uint8_t>(input_[position_]);
      if (byte == static_cast<uint8_t>('"')) {
        ++position_;
        return true;
      }
      if (byte == static_cast<uint8_t>('\\')) {
        ++position_;
        if (position_ >= inputLength_) {
          return fail(DeviceConfigParseError::kMalformedJson);
        }
        const char escape = input_[position_++];
        switch (escape) {
          case '"':
          case '\\':
          case '/':
            appendDecodedByte(static_cast<uint8_t>(escape), decoded);
            break;
          case 'b':
            appendDecodedByte(0x08U, decoded);
            break;
          case 'f':
            appendDecodedByte(0x0cU, decoded);
            break;
          case 'n':
            appendDecodedByte(0x0aU, decoded);
            break;
          case 'r':
            appendDecodedByte(0x0dU, decoded);
            break;
          case 't':
            appendDecodedByte(0x09U, decoded);
            break;
          case 'u':
            if (!parseUnicodeEscape(decoded)) {
              return false;
            }
            break;
          default:
            return fail(DeviceConfigParseError::kMalformedJson);
        }
        continue;
      }
      if (byte < 0x20U) {
        return fail(DeviceConfigParseError::kMalformedJson);
      }
      if (byte < 0x80U) {
        ++position_;
        appendDecodedByte(byte, decoded);
        continue;
      }
      if (!parseRawUtf8(decoded)) {
        return false;
      }
    }
    return fail(DeviceConfigParseError::kMalformedJson);
  }

  bool parseUnicodeEscape(DecodedString& decoded) {
    uint16_t first = 0;
    if (!parseHexQuad(first)) {
      return false;
    }
    uint32_t codePoint = first;
    if (first >= 0xd800U && first <= 0xdbffU) {
      if (position_ + 2U > inputLength_ || input_[position_] != '\\' ||
          input_[position_ + 1U] != 'u') {
        return fail(DeviceConfigParseError::kMalformedJson);
      }
      position_ += 2U;
      uint16_t second = 0;
      if (!parseHexQuad(second) || second < 0xdc00U || second > 0xdfffU) {
        return fail(DeviceConfigParseError::kMalformedJson);
      }
      codePoint = 0x10000U +
                  ((static_cast<uint32_t>(first) - 0xd800U) << 10U) +
                  (static_cast<uint32_t>(second) - 0xdc00U);
    } else if (first >= 0xdc00U && first <= 0xdfffU) {
      return fail(DeviceConfigParseError::kMalformedJson);
    }
    appendCodePoint(codePoint, decoded);
    return true;
  }

  bool parseHexQuad(uint16_t& value) {
    if (position_ + 4U > inputLength_) {
      return fail(DeviceConfigParseError::kMalformedJson);
    }
    uint16_t parsed = 0;
    for (size_t index = 0; index < 4U; ++index) {
      const int nibble = hexValue(input_[position_ + index]);
      if (nibble < 0) {
        return fail(DeviceConfigParseError::kMalformedJson);
      }
      parsed = static_cast<uint16_t>((parsed << 4U) |
                                     static_cast<uint16_t>(nibble));
    }
    position_ += 4U;
    value = parsed;
    return true;
  }

  bool parseRawUtf8(DecodedString& decoded) {
    const uint8_t first = static_cast<uint8_t>(input_[position_]);
    size_t length = 0;
    uint8_t secondMinimum = 0x80U;
    uint8_t secondMaximum = 0xbfU;
    if (first >= 0xc2U && first <= 0xdfU) {
      length = 2;
    } else if (first >= 0xe0U && first <= 0xefU) {
      length = 3;
      if (first == 0xe0U) {
        secondMinimum = 0xa0U;
      } else if (first == 0xedU) {
        secondMaximum = 0x9fU;
      }
    } else if (first >= 0xf0U && first <= 0xf4U) {
      length = 4;
      if (first == 0xf0U) {
        secondMinimum = 0x90U;
      } else if (first == 0xf4U) {
        secondMaximum = 0x8fU;
      }
    } else {
      return fail(DeviceConfigParseError::kMalformedJson);
    }
    if (position_ + length > inputLength_) {
      return fail(DeviceConfigParseError::kMalformedJson);
    }
    const uint8_t second = static_cast<uint8_t>(input_[position_ + 1U]);
    if (second < secondMinimum || second > secondMaximum) {
      return fail(DeviceConfigParseError::kMalformedJson);
    }
    for (size_t index = 2; index < length; ++index) {
      const uint8_t continuation =
          static_cast<uint8_t>(input_[position_ + index]);
      if (continuation < 0x80U || continuation > 0xbfU) {
        return fail(DeviceConfigParseError::kMalformedJson);
      }
    }
    for (size_t index = 0; index < length; ++index) {
      appendDecodedByte(static_cast<uint8_t>(input_[position_ + index]),
                        decoded);
    }
    position_ += length;
    return true;
  }

  static void appendCodePoint(uint32_t codePoint, DecodedString& decoded) {
    if (codePoint <= 0x7fU) {
      appendDecodedByte(static_cast<uint8_t>(codePoint), decoded);
    } else if (codePoint <= 0x7ffU) {
      appendDecodedByte(static_cast<uint8_t>(0xc0U | (codePoint >> 6U)),
                        decoded);
      appendDecodedByte(static_cast<uint8_t>(0x80U | (codePoint & 0x3fU)),
                        decoded);
    } else if (codePoint <= 0xffffU) {
      appendDecodedByte(static_cast<uint8_t>(0xe0U | (codePoint >> 12U)),
                        decoded);
      appendDecodedByte(
          static_cast<uint8_t>(0x80U | ((codePoint >> 6U) & 0x3fU)),
          decoded);
      appendDecodedByte(static_cast<uint8_t>(0x80U | (codePoint & 0x3fU)),
                        decoded);
    } else {
      appendDecodedByte(static_cast<uint8_t>(0xf0U | (codePoint >> 18U)),
                        decoded);
      appendDecodedByte(
          static_cast<uint8_t>(0x80U | ((codePoint >> 12U) & 0x3fU)),
          decoded);
      appendDecodedByte(
          static_cast<uint8_t>(0x80U | ((codePoint >> 6U) & 0x3fU)),
          decoded);
      appendDecodedByte(static_cast<uint8_t>(0x80U | (codePoint & 0x3fU)),
                        decoded);
    }
  }

  static void appendDecodedByte(uint8_t byte, DecodedString& decoded) {
    if (decoded.length < sizeof(decoded.bytes)) {
      decoded.bytes[decoded.length] = static_cast<char>(byte);
    } else {
      decoded.truncated = true;
    }
    ++decoded.length;
  }

  static int hexValue(char character) {
    if (character >= '0' && character <= '9') {
      return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
      return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
      return character - 'A' + 10;
    }
    return -1;
  }

  static bool isDigit(char character) {
    return character >= '0' && character <= '9';
  }

  static bool keyEquals(const DecodedString& key, const char* expected) {
    const size_t expectedLength = std::strlen(expected);
    return !key.truncated &&
           textEquals(key.bytes, key.length, expected, expectedLength);
  }

  bool startsWith(const char* literal) const {
    const size_t length = std::strlen(literal);
    return position_ + length <= inputLength_ &&
           std::memcmp(input_ + position_, literal, length) == 0;
  }

  bool consumeLiteral(const char* literal) {
    if (!startsWith(literal)) {
      return false;
    }
    position_ += std::strlen(literal);
    return true;
  }

  void skipWhitespace() {
    while (position_ < inputLength_) {
      const char character = input_[position_];
      if (character != ' ' && character != '\t' && character != '\n' &&
          character != '\r') {
        break;
      }
      ++position_;
    }
  }

  bool consume(char expected) {
    if (position_ >= inputLength_ || input_[position_] != expected) {
      return false;
    }
    ++position_;
    return true;
  }

  bool fail(DeviceConfigParseError error) {
    error_ = error;
    return false;
  }

  static DeviceConfigParseResult failure(
      DeviceConfigParseError error,
      PresenceConfigValidationError validation =
          PresenceConfigValidationError::kNone) {
    return {defaultPresenceConfig(), error, validation};
  }

  const char* input_;
  size_t inputLength_;
  const char* expectedDeviceId_;
  size_t expectedDeviceIdLength_;
  size_t position_ = 0;
  uint8_t seenTopLevelFields_ = 0;
  uint8_t seenConfigFields_ = 0;
  PresenceConfig config_;
  DeviceConfigParseError error_ = DeviceConfigParseError::kNone;
  PresenceConfigValidationError validationError_ =
      PresenceConfigValidationError::kNone;
};

}  // namespace

PresenceConfig defaultPresenceConfig() {
  return {
      0,       // revision
      10000,   // minimum_on_ms
      30000,   // pir_hold_ms
      12000,   // sound_hold_ms
      300000,  // max_sound_bridge_ms
      5000,    // cooldown_ms
      1.12f,   // sound_factor
      1000,    // telemetry_interval_ms
      30,      // upload_batch_size
  };
}

PresenceConfigValidationError validatePresenceConfig(
    const PresenceConfig& config) {
  if (config.revision > kMaxSigned64) {
    return PresenceConfigValidationError::kRevisionOutOfRange;
  }
  if (config.minimumOnMs > 600000U) {
    return PresenceConfigValidationError::kMinimumOnMsOutOfRange;
  }
  if (config.pirHoldMs < 1000U || config.pirHoldMs > 3600000U) {
    return PresenceConfigValidationError::kPirHoldMsOutOfRange;
  }
  if (config.soundHoldMs > 600000U) {
    return PresenceConfigValidationError::kSoundHoldMsOutOfRange;
  }
  if (config.maxSoundBridgeMs > 3600000U) {
    return PresenceConfigValidationError::kMaxSoundBridgeMsOutOfRange;
  }
  if (config.cooldownMs > 600000U) {
    return PresenceConfigValidationError::kCooldownMsOutOfRange;
  }
  if (!std::isfinite(config.soundFactor)) {
    return PresenceConfigValidationError::kSoundFactorNotFinite;
  }
  if (config.soundFactor < 1.0f || config.soundFactor > 4.0f) {
    return PresenceConfigValidationError::kSoundFactorOutOfRange;
  }
  if (config.telemetryIntervalMs < 250U ||
      config.telemetryIntervalMs > 60000U) {
    return PresenceConfigValidationError::kTelemetryIntervalMsOutOfRange;
  }
  if (config.uploadBatchSize < 1U ||
      config.uploadBatchSize > kPresenceConfigContractMaxUploadBatchSize) {
    return PresenceConfigValidationError::kUploadBatchSizeOutOfRange;
  }
  return PresenceConfigValidationError::kNone;
}

PresenceConfigCapabilityError validatePresenceConfigDeviceCapabilities(
    const PresenceConfig& config) {
  if (config.uploadBatchSize > kDeviceTelemetryBatchCapacity) {
    return PresenceConfigCapabilityError::
        kUploadBatchSizeExceedsDeviceCapacity;
  }
  return PresenceConfigCapabilityError::kNone;
}

const char* presenceConfigCapabilityErrorName(
    PresenceConfigCapabilityError error) {
  switch (error) {
    case PresenceConfigCapabilityError::kNone:
      return "none";
    case PresenceConfigCapabilityError::
        kUploadBatchSizeExceedsDeviceCapacity:
      return "upload_batch_size_exceeds_device_capacity";
  }
  return "unknown";
}

const char* presenceConfigValidationErrorName(
    PresenceConfigValidationError error) {
  switch (error) {
    case PresenceConfigValidationError::kNone:
      return "none";
    case PresenceConfigValidationError::kRevisionOutOfRange:
      return "revision_out_of_range";
    case PresenceConfigValidationError::kMinimumOnMsOutOfRange:
      return "minimum_on_ms_out_of_range";
    case PresenceConfigValidationError::kPirHoldMsOutOfRange:
      return "pir_hold_ms_out_of_range";
    case PresenceConfigValidationError::kSoundHoldMsOutOfRange:
      return "sound_hold_ms_out_of_range";
    case PresenceConfigValidationError::kMaxSoundBridgeMsOutOfRange:
      return "max_sound_bridge_ms_out_of_range";
    case PresenceConfigValidationError::kCooldownMsOutOfRange:
      return "cooldown_ms_out_of_range";
    case PresenceConfigValidationError::kSoundFactorNotFinite:
      return "sound_factor_not_finite";
    case PresenceConfigValidationError::kSoundFactorOutOfRange:
      return "sound_factor_out_of_range";
    case PresenceConfigValidationError::kTelemetryIntervalMsOutOfRange:
      return "telemetry_interval_ms_out_of_range";
    case PresenceConfigValidationError::kUploadBatchSizeOutOfRange:
      return "upload_batch_size_out_of_range";
  }
  return "unknown";
}

DeviceConfigParseResult parseDeviceConfigResponse(
    const char* responseBody, size_t responseBodyLength,
    const char* expectedDeviceId, size_t expectedDeviceIdLength) {
  return ConfigResponseParser(responseBody, responseBodyLength,
                              expectedDeviceId, expectedDeviceIdLength)
      .parse();
}

const char* deviceConfigParseErrorName(DeviceConfigParseError error) {
  switch (error) {
    case DeviceConfigParseError::kNone:
      return "none";
    case DeviceConfigParseError::kNullArgument:
      return "null_argument";
    case DeviceConfigParseError::kTopLevelNotObject:
      return "top_level_not_object";
    case DeviceConfigParseError::kMalformedJson:
      return "malformed_json";
    case DeviceConfigParseError::kMissingField:
      return "missing_field";
    case DeviceConfigParseError::kDuplicateField:
      return "duplicate_field";
    case DeviceConfigParseError::kUnknownField:
      return "unknown_field";
    case DeviceConfigParseError::kWrongType:
      return "wrong_type";
    case DeviceConfigParseError::kIntegerOverflow:
      return "integer_overflow";
    case DeviceConfigParseError::kNonFiniteNumber:
      return "non_finite_number";
    case DeviceConfigParseError::kNumberOverflow:
      return "number_overflow";
    case DeviceConfigParseError::kValueOutOfRange:
      return "value_out_of_range";
    case DeviceConfigParseError::kDeviceIdMismatch:
      return "device_id_mismatch";
    case DeviceConfigParseError::kTrailingData:
      return "trailing_data";
  }
  return "unknown";
}
