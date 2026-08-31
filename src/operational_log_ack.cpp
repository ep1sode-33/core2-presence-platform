#include "operational_log_ack.h"

#include <cstring>
#include <limits>

namespace {

constexpr uint64_t kMaxSigned64 = 0x7fffffffffffffffULL;

class Parser {
 public:
  Parser(const char* input, size_t size, const char* expectedBatchId,
         size_t expectedRecordCount)
      : input_(input),
        size_(size),
        expectedBatchId_(expectedBatchId),
        expectedRecordCount_(expectedRecordCount) {}

  OperationalLogAckResult parse() {
    if (input_ == nullptr || expectedBatchId_ == nullptr ||
        expectedRecordCount_ == 0) {
      return fail(OperationalLogAckError::kNullArgument);
    }
    skip();
    if (!consume('{')) return fail(OperationalLogAckError::kMalformedJson);
    skip();
    uint8_t seen = 0;
    while (!consume('}')) {
      char key[32] = {};
      if (!string(key, sizeof(key))) return fail(error_);
      skip();
      if (!consume(':')) return fail(OperationalLogAckError::kMalformedJson);
      skip();
      uint8_t bit = 0;
      if (std::strcmp(key, "batch_id") == 0) {
        bit = 1U << 0;
        char value[97] = {};
        if (!string(value, sizeof(value))) return fail(error_);
        if (std::strcmp(value, expectedBatchId_) != 0) {
          return fail(OperationalLogAckError::kMismatch);
        }
      } else if (std::strcmp(key, "stored") == 0) {
        bit = 1U << 1;
        if (!integer(result_.ack.stored)) return fail(error_);
      } else if (std::strcmp(key, "duplicates") == 0) {
        bit = 1U << 2;
        if (!integer(result_.ack.duplicates)) return fail(error_);
      } else if (std::strcmp(key, "server_utc_ms") == 0) {
        bit = 1U << 3;
        if (!integer(result_.ack.serverUtcMs)) return fail(error_);
      } else if (std::strcmp(key, "retained_records") == 0) {
        bit = 1U << 4;
        if (!integer(result_.ack.retainedRecords)) return fail(error_);
      } else {
        return fail(OperationalLogAckError::kUnknownField);
      }
      if ((seen & bit) != 0) {
        return fail(OperationalLogAckError::kDuplicateField);
      }
      seen |= bit;
      skip();
      if (consume('}')) break;
      if (!consume(',')) return fail(OperationalLogAckError::kMalformedJson);
      skip();
    }
    skip();
    if (position_ != size_) return fail(OperationalLogAckError::kTrailingData);
    if (seen != 0x1f) return fail(OperationalLogAckError::kMissingField);
    if (result_.ack.stored > expectedRecordCount_ ||
        result_.ack.duplicates > expectedRecordCount_ ||
        result_.ack.stored + result_.ack.duplicates != expectedRecordCount_) {
      return fail(OperationalLogAckError::kMismatch);
    }
    result_.error = OperationalLogAckError::kNone;
    return result_;
  }

 private:
  bool string(char* output, size_t capacity) {
    if (!consume('"')) return set(OperationalLogAckError::kWrongType);
    size_t length = 0;
    while (position_ < size_) {
      const unsigned char byte = static_cast<unsigned char>(input_[position_++]);
      if (byte == '"') {
        output[length] = '\0';
        return true;
      }
      if (byte == '\\' || byte < 0x20 || length + 1 >= capacity) {
        return set(OperationalLogAckError::kInvalidValue);
      }
      output[length++] = static_cast<char>(byte);
    }
    return set(OperationalLogAckError::kMalformedJson);
  }

  bool integer(uint64_t& output) {
    if (position_ >= size_ || input_[position_] < '0' ||
        input_[position_] > '9') {
      return set(OperationalLogAckError::kWrongType);
    }
    uint64_t value = 0;
    do {
      const uint8_t digit = static_cast<uint8_t>(input_[position_] - '0');
      if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10U) {
        return set(OperationalLogAckError::kInvalidValue);
      }
      value = value * 10U + digit;
      ++position_;
    } while (position_ < size_ && input_[position_] >= '0' &&
             input_[position_] <= '9');
    if (value > kMaxSigned64) return set(OperationalLogAckError::kInvalidValue);
    output = value;
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

  bool set(OperationalLogAckError error) {
    error_ = error;
    return false;
  }

  OperationalLogAckResult fail(OperationalLogAckError error) const {
    OperationalLogAckResult failed = {};
    failed.error = error;
    return failed;
  }

  const char* input_ = nullptr;
  size_t size_ = 0;
  const char* expectedBatchId_ = nullptr;
  size_t expectedRecordCount_ = 0;
  size_t position_ = 0;
  OperationalLogAckResult result_ = {};
  OperationalLogAckError error_ = OperationalLogAckError::kNone;
};

}  // namespace

OperationalLogAckResult parseOperationalLogAck(
    const char* json, size_t jsonLength, const char* expectedBatchId,
    size_t expectedRecordCount) {
  return Parser(json, jsonLength, expectedBatchId, expectedRecordCount).parse();
}
const char* operationalLogAckErrorName(OperationalLogAckError error) {
  switch (error) {
    case OperationalLogAckError::kNone:
      return "none";
    case OperationalLogAckError::kNullArgument:
      return "null_argument";
    case OperationalLogAckError::kMalformedJson:
      return "malformed_json";
    case OperationalLogAckError::kMissingField:
      return "missing_field";
    case OperationalLogAckError::kDuplicateField:
      return "duplicate_field";
    case OperationalLogAckError::kUnknownField:
      return "unknown_field";
    case OperationalLogAckError::kWrongType:
      return "wrong_type";
    case OperationalLogAckError::kInvalidValue:
      return "invalid_value";
    case OperationalLogAckError::kMismatch:
      return "mismatch";
    case OperationalLogAckError::kTrailingData:
      return "trailing_data";
  }
  return "unknown";
}
