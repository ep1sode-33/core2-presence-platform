#include "control_protocol.h"

#include <cstring>
#include <limits>

namespace {

constexpr uint64_t kMaxSigned64 = 0x7fffffffffffffffULL;

bool equals(const char* value, const char* expected) {
  return std::strcmp(value, expected) == 0;
}

bool validProtocolId(const char* value, size_t minimum, size_t maximum) {
  const size_t length = std::strlen(value);
  if (length < minimum || length > maximum) return false;
  for (size_t index = 0; index < length; ++index) {
    const char byte = value[index];
    if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
        (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' ||
        byte == ':' || byte == '-') {
      continue;
    }
    return false;
  }
  return true;
}

bool validCanonicalText(const char* value, size_t minimum, size_t maximum) {
  const size_t length = std::strlen(value);
  if (length < minimum || length > maximum) return false;
  for (size_t index = 0; index < length; ++index) {
    const char byte = value[index];
    if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
        (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' ||
        byte == '+' || byte == '-') {
      continue;
    }
    return false;
  }
  return true;
}

bool validLowerHexDigest(const char* value) {
  if (std::strlen(value) != 64) return false;
  for (size_t index = 0; index < 64; ++index) {
    if (!((value[index] >= '0' && value[index] <= '9') ||
          (value[index] >= 'a' && value[index] <= 'f'))) {
      return false;
    }
  }
  return true;
}

bool validRelativeDeviceUrl(const char* value) {
  const size_t length = std::strlen(value);
  if (length < 2 || value[0] != '/' || value[1] == '/' ||
      std::strstr(value, "..") != nullptr) {
    return false;
  }
  for (size_t index = 0; index < length; ++index) {
    const unsigned char byte = static_cast<unsigned char>(value[index]);
    if (byte <= 0x20 || byte == 0x7f || value[index] == '?' ||
        value[index] == '#' || value[index] == '\\') {
      return false;
    }
  }
  return true;
}

class Parser {
 public:
  Parser(const char* input, size_t size) : input_(input), size_(size) {}

  ControlPollParseResult parse() {
    if (input_ == nullptr) return fail(ControlPollParseError::kNullArgument);
    skipWhitespace();
    if (!consume('{')) {
      return fail(ControlPollParseError::kTopLevelNotObject);
    }
    skipWhitespace();
    if (consume('}')) return fail(ControlPollParseError::kMissingField);

    uint8_t seen = 0;
    while (true) {
      char key[32] = {};
      if (!string(key, sizeof(key))) return fail(error_);
      skipWhitespace();
      if (!consume(':')) return fail(ControlPollParseError::kMalformedJson);
      skipWhitespace();

      uint8_t bit = 0;
      if (equals(key, "server_utc_ms")) {
        bit = 1U << 0;
        if (!unsignedInteger(result_.serverUtcMs, kMaxSigned64)) {
          return fail(error_);
        }
      } else if (equals(key, "poll_after_ms")) {
        bit = 1U << 1;
        uint64_t parsed = 0;
        if (!unsignedInteger(parsed, UINT32_MAX) || parsed != 5000) {
          if (error_ == ControlPollParseError::kNone) {
            error_ = ControlPollParseError::kInvalidValue;
          }
          return fail(error_);
        }
        result_.pollAfterMs = static_cast<uint32_t>(parsed);
      } else if (equals(key, "desired_release")) {
        bit = 1U << 2;
        if (literal("null")) {
          result_.hasDesiredRelease = false;
        } else {
          if (!desiredRelease()) return fail(error_);
          result_.hasDesiredRelease = true;
        }
      } else if (equals(key, "command")) {
        bit = 1U << 3;
        if (literal("null")) {
          result_.hasCommand = false;
        } else {
          if (!leasedCommand()) return fail(error_);
          result_.hasCommand = true;
        }
      } else {
        return fail(ControlPollParseError::kUnknownField);
      }
      if ((seen & bit) != 0) {
        return fail(ControlPollParseError::kDuplicateField);
      }
      seen |= bit;

      skipWhitespace();
      if (consume('}')) break;
      if (!consume(',')) return fail(ControlPollParseError::kMalformedJson);
      skipWhitespace();
    }
    skipWhitespace();
    if (position_ != size_) return fail(ControlPollParseError::kTrailingData);
    if (seen != 0x0f) return fail(ControlPollParseError::kMissingField);
    return {result_, ControlPollParseError::kNone};
  }

 private:
  bool desiredRelease() {
    if (!consume('{')) return setError(ControlPollParseError::kWrongType);
    skipWhitespace();
    if (consume('}')) return setError(ControlPollParseError::kMissingField);
    uint16_t seen = 0;
    while (true) {
      char key[32] = {};
      if (!string(key, sizeof(key))) return false;
      skipWhitespace();
      if (!consume(':')) return setError(ControlPollParseError::kMalformedJson);
      skipWhitespace();
      uint16_t bit = 0;
      if (equals(key, "release_id")) {
        bit = 1U << 0;
        if (!string(result_.desiredRelease.releaseId,
                    sizeof(result_.desiredRelease.releaseId))) return false;
      } else if (equals(key, "hardware_model")) {
        bit = 1U << 1;
        if (!string(result_.desiredRelease.hardwareModel,
                    sizeof(result_.desiredRelease.hardwareModel))) return false;
      } else if (equals(key, "firmware_version")) {
        bit = 1U << 2;
        if (!string(result_.desiredRelease.firmwareVersion,
                    sizeof(result_.desiredRelease.firmwareVersion))) return false;
      } else if (equals(key, "release_counter")) {
        bit = 1U << 3;
        if (!unsignedInteger(result_.desiredRelease.releaseCounter,
                             kMaxSigned64)) return false;
      } else if (equals(key, "build_id")) {
        bit = 1U << 4;
        if (!string(result_.desiredRelease.buildId,
                    sizeof(result_.desiredRelease.buildId))) return false;
      } else if (equals(key, "image_size")) {
        bit = 1U << 5;
        uint64_t value = 0;
        if (!unsignedInteger(value, UINT32_MAX)) return false;
        result_.desiredRelease.imageSize = static_cast<uint32_t>(value);
      } else if (equals(key, "image_sha256")) {
        bit = 1U << 6;
        if (!string(result_.desiredRelease.imageSha256,
                    sizeof(result_.desiredRelease.imageSha256))) return false;
      } else if (equals(key, "elf_sha256")) {
        bit = 1U << 7;
        if (!string(result_.desiredRelease.elfSha256,
                    sizeof(result_.desiredRelease.elfSha256))) return false;
      } else if (equals(key, "key_id")) {
        bit = 1U << 8;
        if (!string(result_.desiredRelease.signingKeyId,
                    sizeof(result_.desiredRelease.signingKeyId))) return false;
      } else if (equals(key, "signature_format")) {
        bit = 1U << 9;
        char signatureFormat[40] = {};
        if (!string(signatureFormat, sizeof(signatureFormat))) return false;
        if (!equals(signatureFormat, "ecdsa-p256-sha256-raw")) {
          return setError(ControlPollParseError::kInvalidValue);
        }
      } else if (equals(key, "imported_at_ms")) {
        bit = 1U << 10;
        uint64_t ignored = 0;
        if (!unsignedInteger(ignored, kMaxSigned64)) return false;
      } else if (equals(key, "imported_by")) {
        bit = 1U << 11;
        char ignored[65] = {};
        if (!string(ignored, sizeof(ignored))) return false;
      } else if (equals(key, "verified")) {
        bit = 1U << 12;
        bool verified = false;
        if (!boolean(verified)) return false;
        if (!verified) return setError(ControlPollParseError::kInvalidValue);
      } else if (equals(key, "manifest_url")) {
        bit = 1U << 13;
        if (!string(result_.desiredRelease.manifestUrl,
                    sizeof(result_.desiredRelease.manifestUrl))) return false;
      } else if (equals(key, "image_url")) {
        bit = 1U << 14;
        if (!string(result_.desiredRelease.imageUrl,
                    sizeof(result_.desiredRelease.imageUrl))) return false;
      } else {
        return setError(ControlPollParseError::kUnknownField);
      }
      if ((seen & bit) != 0) return setError(ControlPollParseError::kDuplicateField);
      seen |= bit;
      skipWhitespace();
      if (consume('}')) break;
      if (!consume(',')) return setError(ControlPollParseError::kMalformedJson);
      skipWhitespace();
    }
    if (seen != 0x7fff) return setError(ControlPollParseError::kMissingField);
    const DesiredFirmwareRelease& release = result_.desiredRelease;
    if (!validProtocolId(release.releaseId, 8, 48) ||
        !validCanonicalText(release.hardwareModel, 1, 48) ||
        !validCanonicalText(release.firmwareVersion, 1, 32) ||
        !validCanonicalText(release.buildId, 1, 64) ||
        !validCanonicalText(release.signingKeyId, 1, 32) ||
        !validLowerHexDigest(release.imageSha256) ||
        !validLowerHexDigest(release.elfSha256) ||
        release.releaseCounter == 0 || release.imageSize == 0 ||
        !validRelativeDeviceUrl(release.manifestUrl) ||
        !validRelativeDeviceUrl(release.imageUrl)) {
      return setError(ControlPollParseError::kInvalidValue);
    }
    return true;
  }

  bool leasedCommand() {
    if (!consume('{')) return setError(ControlPollParseError::kWrongType);
    skipWhitespace();
    if (consume('}')) return setError(ControlPollParseError::kMissingField);
    uint8_t seen = 0;
    while (true) {
      char key[32] = {};
      if (!string(key, sizeof(key))) return false;
      skipWhitespace();
      if (!consume(':')) return setError(ControlPollParseError::kMalformedJson);
      skipWhitespace();
      uint8_t bit = 0;
      if (equals(key, "command_id")) {
        bit = 1U << 0;
        if (!string(result_.command.commandId,
                    sizeof(result_.command.commandId))) return false;
      } else if (equals(key, "created_at_ms")) {
        bit = 1U << 1;
        if (!unsignedInteger(result_.command.createdAtMs, kMaxSigned64)) return false;
      } else if (equals(key, "expires_at_ms")) {
        bit = 1U << 2;
        if (!unsignedInteger(result_.command.expiresAtMs, kMaxSigned64)) return false;
      } else if (equals(key, "lease_id")) {
        bit = 1U << 3;
        if (!string(result_.command.leaseId,
                    sizeof(result_.command.leaseId))) return false;
      } else if (equals(key, "lease_expires_at_ms")) {
        bit = 1U << 4;
        if (!unsignedInteger(result_.command.leaseExpiresAtMs, kMaxSigned64)) {
          return false;
        }
      } else if (equals(key, "delivery_attempt")) {
        bit = 1U << 5;
        uint64_t attempt = 0;
        if (!unsignedInteger(attempt, UINT32_MAX) || attempt == 0) {
          if (error_ == ControlPollParseError::kNone) {
            error_ = ControlPollParseError::kInvalidValue;
          }
          return false;
        }
      } else if (equals(key, "command")) {
        bit = 1U << 6;
        if (!commandPayload()) return false;
      } else {
        return setError(ControlPollParseError::kUnknownField);
      }
      if ((seen & bit) != 0) return setError(ControlPollParseError::kDuplicateField);
      seen |= bit;
      skipWhitespace();
      if (consume('}')) break;
      if (!consume(',')) return setError(ControlPollParseError::kMalformedJson);
      skipWhitespace();
    }
    if (seen != 0x7f) return setError(ControlPollParseError::kMissingField);
    if (!remoteCommandEnvelopeIsValid(result_.command)) {
      return setError(ControlPollParseError::kInvalidValue);
    }
    return true;
  }

  bool commandPayload() {
    if (!consume('{')) return setError(ControlPollParseError::kWrongType);
    skipWhitespace();
    if (consume('}')) return setError(ControlPollParseError::kMissingField);
    char action[32] = {};
    char level[20] = {};
    uint64_t duration = 0;
    bool confirmation = false;
    uint8_t seen = 0;
    while (true) {
      char key[40] = {};
      if (!string(key, sizeof(key))) return false;
      skipWhitespace();
      if (!consume(':')) return setError(ControlPollParseError::kMalformedJson);
      skipWhitespace();
      uint8_t bit = 0;
      if (equals(key, "action")) {
        bit = 1U << 0;
        if (!string(action, sizeof(action))) return false;
      } else if (equals(key, "level")) {
        bit = 1U << 1;
        if (!string(level, sizeof(level))) return false;
      } else if (equals(key, "duration_seconds")) {
        bit = 1U << 2;
        if (!unsignedInteger(duration, UINT16_MAX)) return false;
      } else if (equals(key, "requires_local_confirmation")) {
        bit = 1U << 3;
        if (!boolean(confirmation)) return false;
      } else {
        return setError(ControlPollParseError::kUnknownField);
      }
      if ((seen & bit) != 0) return setError(ControlPollParseError::kDuplicateField);
      seen |= bit;
      skipWhitespace();
      if (consume('}')) break;
      if (!consume(',')) return setError(ControlPollParseError::kMalformedJson);
      skipWhitespace();
    }
    if ((seen & 1U) == 0) return setError(ControlPollParseError::kMissingField);

    if (equals(action, "diagnostic_snapshot")) {
      if (seen != 1U) return setError(ControlPollParseError::kInvalidValue);
      result_.command.action = RemoteCommandAction::kDiagnosticSnapshot;
    } else if (equals(action, "set_log_level")) {
      if (seen != 0x07 || duration < 1 || duration > 600 ||
          !(equals(level, "event") || equals(level, "debug_sensor"))) {
        return setError(ControlPollParseError::kInvalidValue);
      }
      result_.command.action = RemoteCommandAction::kSetLogLevel;
      result_.command.durationSeconds = static_cast<uint16_t>(duration);
      result_.command.detailedLog = equals(level, "debug_sensor");
    } else if (equals(action, "recalibrate_microphone")) {
      if (seen != 1U) return setError(ControlPollParseError::kInvalidValue);
      result_.command.action = RemoteCommandAction::kRecalibrateMicrophone;
    } else if (equals(action, "retry_upload")) {
      if (seen != 1U) return setError(ControlPollParseError::kInvalidValue);
      result_.command.action = RemoteCommandAction::kRetryUpload;
    } else if (equals(action, "reboot")) {
      if (seen != 1U) return setError(ControlPollParseError::kInvalidValue);
      result_.command.action = RemoteCommandAction::kReboot;
    } else if (equals(action, "open_dev_ota")) {
      if (seen != 0x09 || !confirmation) {
        return setError(ControlPollParseError::kInvalidValue);
      }
      result_.command.action = RemoteCommandAction::kOpenDevOta;
      result_.command.requiresLocalConfirmation = true;
    } else {
      return setError(ControlPollParseError::kInvalidValue);
    }
    return true;
  }

  bool string(char* output, size_t capacity) {
    if (output == nullptr || capacity == 0) {
      return setError(ControlPollParseError::kStringTooLong);
    }
    if (!consume('"')) return setError(ControlPollParseError::kWrongType);
    size_t length = 0;
    while (position_ < size_) {
      const unsigned char byte = static_cast<unsigned char>(input_[position_++]);
      if (byte == '"') {
        output[length] = '\0';
        return true;
      }
      if (byte == '\\') {
        return setError(ControlPollParseError::kStringEscapeNotAllowed);
      }
      if (byte < 0x20) return setError(ControlPollParseError::kMalformedJson);
      if (length + 1 >= capacity) {
        return setError(ControlPollParseError::kStringTooLong);
      }
      output[length++] = static_cast<char>(byte);
    }
    return setError(ControlPollParseError::kMalformedJson);
  }

  bool unsignedInteger(uint64_t& output, uint64_t maximum) {
    if (position_ >= size_ || input_[position_] < '0' ||
        input_[position_] > '9') {
      return setError(ControlPollParseError::kWrongType);
    }
    if (input_[position_] == '0') {
      output = 0;
      ++position_;
      if (position_ < size_ && input_[position_] >= '0' &&
          input_[position_] <= '9') {
        return setError(ControlPollParseError::kMalformedJson);
      }
      return true;
    }
    uint64_t value = 0;
    while (position_ < size_ && input_[position_] >= '0' &&
           input_[position_] <= '9') {
      const uint8_t digit = static_cast<uint8_t>(input_[position_] - '0');
      if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10U) {
        return setError(ControlPollParseError::kIntegerOverflow);
      }
      value = value * 10U + digit;
      ++position_;
    }
    if (value > maximum) {
      return setError(ControlPollParseError::kIntegerOutOfRange);
    }
    output = value;
    return true;
  }

  bool boolean(bool& output) {
    if (literal("true")) {
      output = true;
      return true;
    }
    if (literal("false")) {
      output = false;
      return true;
    }
    return setError(ControlPollParseError::kWrongType);
  }

  bool literal(const char* value) {
    const size_t length = std::strlen(value);
    if (length > size_ - position_ ||
        std::memcmp(input_ + position_, value, length) != 0) {
      return false;
    }
    position_ += length;
    return true;
  }

  void skipWhitespace() {
    while (position_ < size_ &&
           (input_[position_] == ' ' || input_[position_] == '\t' ||
            input_[position_] == '\r' || input_[position_] == '\n')) {
      ++position_;
    }
  }

  bool consume(char value) {
    if (position_ >= size_ || input_[position_] != value) return false;
    ++position_;
    return true;
  }

  bool setError(ControlPollParseError error) {
    error_ = error;
    return false;
  }

  ControlPollParseResult fail(ControlPollParseError error) const {
    return {ControlPoll{}, error};
  }

  const char* input_ = nullptr;
  size_t size_ = 0;
  size_t position_ = 0;
  ControlPoll result_ = {};
  ControlPollParseError error_ = ControlPollParseError::kNone;
};

}  // namespace

ControlPollParseResult parseControlPollResponse(const char* json,
                                                size_t jsonLength) {
  return Parser(json, jsonLength).parse();
}

const char* controlPollParseErrorName(ControlPollParseError error) {
  switch (error) {
    case ControlPollParseError::kNone:
      return "none";
    case ControlPollParseError::kNullArgument:
      return "null_argument";
    case ControlPollParseError::kTopLevelNotObject:
      return "top_level_not_object";
    case ControlPollParseError::kMalformedJson:
      return "malformed_json";
    case ControlPollParseError::kMissingField:
      return "missing_field";
    case ControlPollParseError::kDuplicateField:
      return "duplicate_field";
    case ControlPollParseError::kUnknownField:
      return "unknown_field";
    case ControlPollParseError::kWrongType:
      return "wrong_type";
    case ControlPollParseError::kStringTooLong:
      return "string_too_long";
    case ControlPollParseError::kStringEscapeNotAllowed:
      return "string_escape_not_allowed";
    case ControlPollParseError::kIntegerOverflow:
      return "integer_overflow";
    case ControlPollParseError::kIntegerOutOfRange:
      return "integer_out_of_range";
    case ControlPollParseError::kInvalidValue:
      return "invalid_value";
    case ControlPollParseError::kTrailingData:
      return "trailing_data";
  }
  return "unknown";
}
