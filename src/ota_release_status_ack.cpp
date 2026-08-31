#include "ota_release_status_ack.h"

#include <cstring>
#include <limits>

namespace {

constexpr uint64_t kMaximumSigned64 = UINT64_C(0x7fffffffffffffff);

bool validExpectedId(const char* value) {
  if (value == nullptr) {
    return false;
  }
  const size_t length = std::strlen(value);
  if (length < 8 || length > 64) {
    return false;
  }
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

class Parser {
 public:
  Parser(const char* input, size_t size, const char* expectedStatusId)
      : input_(input), size_(size), expectedStatusId_(expectedStatusId) {}

  OtaReleaseStatusAckResult parse() {
    if (input_ == nullptr || !validExpectedId(expectedStatusId_)) {
      return fail(OtaReleaseStatusAckError::kNullArgument);
    }
    skipWhitespace();
    if (!consume('{')) {
      return fail(OtaReleaseStatusAckError::kMalformedJson);
    }
    skipWhitespace();
    if (consume('}')) {
      return fail(OtaReleaseStatusAckError::kMissingField);
    }

    uint8_t seen = 0;
    while (true) {
      char key[40] = {};
      if (!string(key, sizeof(key))) {
        return fail(error_);
      }
      skipWhitespace();
      if (!consume(':')) {
        return fail(OtaReleaseStatusAckError::kMalformedJson);
      }
      skipWhitespace();

      uint8_t bit = 0;
      if (std::strcmp(key, "status_id") == 0) {
        bit = 1U << 0;
        char value[65] = {};
        if (!string(value, sizeof(value))) {
          return fail(error_);
        }
        if (std::strcmp(value, expectedStatusId_) != 0) {
          return fail(OtaReleaseStatusAckError::kMismatch);
        }
      } else if (std::strcmp(key, "duplicate") == 0) {
        bit = 1U << 1;
        if (!boolean(result_.ack.duplicate)) {
          return fail(error_);
        }
      } else if (std::strcmp(key, "server_utc_ms") == 0) {
        bit = 1U << 2;
        if (!integer(result_.ack.serverUtcMs)) {
          return fail(error_);
        }
      } else if (std::strcmp(key, "desired_release_completed") == 0) {
        bit = 1U << 3;
        if (!boolean(result_.ack.desiredReleaseCompleted)) {
          return fail(error_);
        }
      } else {
        return fail(OtaReleaseStatusAckError::kUnknownField);
      }
      if ((seen & bit) != 0) {
        return fail(OtaReleaseStatusAckError::kDuplicateField);
      }
      seen |= bit;

      skipWhitespace();
      if (consume('}')) {
        break;
      }
      if (!consume(',')) {
        return fail(OtaReleaseStatusAckError::kMalformedJson);
      }
      skipWhitespace();
    }
    skipWhitespace();
    if (position_ != size_) {
      return fail(OtaReleaseStatusAckError::kTrailingData);
    }
    if (seen != 0x0f) {
      return fail(OtaReleaseStatusAckError::kMissingField);
    }
    result_.error = OtaReleaseStatusAckError::kNone;
    return result_;
  }

 private:
  bool string(char* output, size_t capacity) {
    if (output == nullptr || capacity == 0 || !consume('"')) {
      return setError(OtaReleaseStatusAckError::kWrongType);
    }
    size_t length = 0;
    while (position_ < size_) {
      const unsigned char byte =
          static_cast<unsigned char>(input_[position_++]);
      if (byte == '"') {
        output[length] = '\0';
        return true;
      }
      if (byte == '\\' || byte < 0x20 || length + 1 >= capacity) {
        return setError(OtaReleaseStatusAckError::kInvalidValue);
      }
      output[length++] = static_cast<char>(byte);
    }
    return setError(OtaReleaseStatusAckError::kMalformedJson);
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
    return setError(OtaReleaseStatusAckError::kWrongType);
  }

  bool integer(uint64_t& output) {
    if (position_ >= size_ || input_[position_] < '0' ||
        input_[position_] > '9') {
      return setError(OtaReleaseStatusAckError::kWrongType);
    }
    if (input_[position_] == '0') {
      output = 0;
      ++position_;
      if (position_ < size_ && input_[position_] >= '0' &&
          input_[position_] <= '9') {
        return setError(OtaReleaseStatusAckError::kMalformedJson);
      }
      return true;
    }
    uint64_t value = 0;
    while (position_ < size_ && input_[position_] >= '0' &&
           input_[position_] <= '9') {
      const uint8_t digit = static_cast<uint8_t>(input_[position_] - '0');
      if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10U) {
        return setError(OtaReleaseStatusAckError::kInvalidValue);
      }
      value = value * 10U + digit;
      ++position_;
    }
    if (value > kMaximumSigned64) {
      return setError(OtaReleaseStatusAckError::kInvalidValue);
    }
    output = value;
    return true;
  }

  bool literal(const char* value) {
    const size_t length = std::strlen(value);
    if (position_ > size_ || length > size_ - position_ ||
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

  bool consume(char expected) {
    if (position_ >= size_ || input_[position_] != expected) {
      return false;
    }
    ++position_;
    return true;
  }

  bool setError(OtaReleaseStatusAckError error) {
    error_ = error;
    return false;
  }

  OtaReleaseStatusAckResult fail(OtaReleaseStatusAckError error) const {
    OtaReleaseStatusAckResult failure = {};
    failure.error = error;
    return failure;
  }

  const char* input_ = nullptr;
  size_t size_ = 0;
  const char* expectedStatusId_ = nullptr;
  size_t position_ = 0;
  OtaReleaseStatusAckResult result_ = {};
  OtaReleaseStatusAckError error_ = OtaReleaseStatusAckError::kNone;
};

}  // namespace

OtaReleaseStatusAckResult parseOtaReleaseStatusAck(
    const char* json, size_t jsonLength, const char* expectedStatusId) {
  return Parser(json, jsonLength, expectedStatusId).parse();
}

const char* otaReleaseStatusAckErrorName(OtaReleaseStatusAckError error) {
  switch (error) {
    case OtaReleaseStatusAckError::kNone:
      return "none";
    case OtaReleaseStatusAckError::kNullArgument:
      return "null_argument";
    case OtaReleaseStatusAckError::kMalformedJson:
      return "malformed_json";
    case OtaReleaseStatusAckError::kMissingField:
      return "missing_field";
    case OtaReleaseStatusAckError::kDuplicateField:
      return "duplicate_field";
    case OtaReleaseStatusAckError::kUnknownField:
      return "unknown_field";
    case OtaReleaseStatusAckError::kWrongType:
      return "wrong_type";
    case OtaReleaseStatusAckError::kInvalidValue:
      return "invalid_value";
    case OtaReleaseStatusAckError::kMismatch:
      return "mismatch";
    case OtaReleaseStatusAckError::kTrailingData:
      return "trailing_data";
  }
  return "unknown";
}
