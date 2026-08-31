#include "core_dump_ack.h"

#include <cstring>
#include <limits>

namespace {

constexpr uint64_t kMaxSigned64 = 0x7fffffffffffffffULL;

class Parser {
 public:
  Parser(const char* input, size_t size, const char* expected)
      : input_(input), size_(size), expected_(expected) {}

  CoreDumpAckResult parse() {
    if (input_ == nullptr || expected_ == nullptr || expected_[0] == '\0') {
      return fail(CoreDumpAckError::kNullArgument);
    }
    skip();
    if (!consume('{')) return fail(CoreDumpAckError::kMalformedJson);
    skip();
    uint8_t seen = 0;
    while (!consume('}')) {
      char key[40] = {};
      if (!string(key, sizeof(key))) return fail(error_);
      skip();
      if (!consume(':')) return fail(CoreDumpAckError::kMalformedJson);
      skip();
      uint8_t bit = 0;
      if (std::strcmp(key, "crash_id") == 0) {
        bit = 1U << 0;
        char value[65] = {};
        if (!string(value, sizeof(value))) return fail(error_);
        if (std::strcmp(value, expected_) != 0) {
          return fail(CoreDumpAckError::kMismatch);
        }
      } else if (std::strcmp(key, "duplicate") == 0) {
        bit = 1U << 1;
        if (!boolean(result_.duplicate)) return fail(error_);
      } else if (std::strcmp(key, "durable") == 0) {
        bit = 1U << 2;
        if (!boolean(result_.durable)) return fail(error_);
      } else if (std::strcmp(key, "server_utc_ms") == 0) {
        bit = 1U << 3;
        if (!integer(result_.serverUtcMs)) return fail(error_);
      } else if (std::strcmp(key, "symbolication_status") == 0) {
        bit = 1U << 4;
        char ignored[65] = {};
        if (!string(ignored, sizeof(ignored)) || ignored[0] == '\0') {
          return fail(error_ == CoreDumpAckError::kNone
                          ? CoreDumpAckError::kInvalidValue
                          : error_);
        }
      } else {
        return fail(CoreDumpAckError::kUnknownField);
      }
      if ((seen & bit) != 0) return fail(CoreDumpAckError::kDuplicateField);
      seen |= bit;
      skip();
      if (consume('}')) break;
      if (!consume(',')) return fail(CoreDumpAckError::kMalformedJson);
      skip();
    }
    skip();
    if (position_ != size_) return fail(CoreDumpAckError::kTrailingData);
    if (seen != 0x1f) return fail(CoreDumpAckError::kMissingField);
    if (!result_.durable) return fail(CoreDumpAckError::kInvalidValue);
    result_.error = CoreDumpAckError::kNone;
    return result_;
  }

 private:
  bool string(char* output, size_t capacity) {
    if (!consume('"')) return set(CoreDumpAckError::kWrongType);
    size_t length = 0;
    while (position_ < size_) {
      const unsigned char value =
          static_cast<unsigned char>(input_[position_++]);
      if (value == '"') {
        output[length] = '\0';
        return true;
      }
      if (value == '\\' || value < 0x20 || length + 1 >= capacity) {
        return set(CoreDumpAckError::kInvalidValue);
      }
      output[length++] = static_cast<char>(value);
    }
    return set(CoreDumpAckError::kMalformedJson);
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
    return set(CoreDumpAckError::kWrongType);
  }

  bool integer(uint64_t& output) {
    if (position_ >= size_ || input_[position_] < '0' ||
        input_[position_] > '9') {
      return set(CoreDumpAckError::kWrongType);
    }
    uint64_t value = 0;
    do {
      const uint8_t digit = static_cast<uint8_t>(input_[position_] - '0');
      if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10U) {
        return set(CoreDumpAckError::kInvalidValue);
      }
      value = value * 10U + digit;
      ++position_;
    } while (position_ < size_ && input_[position_] >= '0' &&
             input_[position_] <= '9');
    if (value > kMaxSigned64) return set(CoreDumpAckError::kInvalidValue);
    output = value;
    return true;
  }

  bool literal(const char* expected) {
    const size_t length = std::strlen(expected);
    if (length > size_ - position_ ||
        std::memcmp(input_ + position_, expected, length) != 0) {
      return false;
    }
    position_ += length;
    return true;
  }

  void skip() {
    while (position_ < size_ &&
           (input_[position_] == ' ' || input_[position_] == '\t' ||
            input_[position_] == '\n' || input_[position_] == '\r')) {
      ++position_;
    }
  }

  bool consume(char value) {
    if (position_ >= size_ || input_[position_] != value) return false;
    ++position_;
    return true;
  }

  bool set(CoreDumpAckError error) {
    error_ = error;
    return false;
  }

  CoreDumpAckResult fail(CoreDumpAckError error) const {
    CoreDumpAckResult failed = {};
    failed.error = error;
    return failed;
  }

  const char* input_ = nullptr;
  size_t size_ = 0;
  const char* expected_ = nullptr;
  size_t position_ = 0;
  CoreDumpAckResult result_ = {};
  CoreDumpAckError error_ = CoreDumpAckError::kNone;
};

}  // namespace

CoreDumpAckResult parseCoreDumpAck(const char* json, size_t jsonLength,
                                   const char* expectedCrashId) {
  return Parser(json, jsonLength, expectedCrashId).parse();
}
const char* coreDumpAckErrorName(CoreDumpAckError error) {
  switch (error) {
    case CoreDumpAckError::kNone:
      return "none";
    case CoreDumpAckError::kNullArgument:
      return "null_argument";
    case CoreDumpAckError::kMalformedJson:
      return "malformed_json";
    case CoreDumpAckError::kMissingField:
      return "missing_field";
    case CoreDumpAckError::kDuplicateField:
      return "duplicate_field";
    case CoreDumpAckError::kUnknownField:
      return "unknown_field";
    case CoreDumpAckError::kWrongType:
      return "wrong_type";
    case CoreDumpAckError::kInvalidValue:
      return "invalid_value";
    case CoreDumpAckError::kMismatch:
      return "mismatch";
    case CoreDumpAckError::kTrailingData:
      return "trailing_data";
  }
  return "unknown";
}
