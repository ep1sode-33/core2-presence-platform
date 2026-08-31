#include "command_ack_protocol.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {

constexpr uint64_t kMaxSigned64 = 0x7fffffffffffffffULL;

class Writer {
 public:
  explicit Writer(const CommandAckJsonSink& sink) : sink_(sink) {}
  bool raw(const char* value) {
    const size_t size = std::strlen(value);
    return ok_ && sink_.write != nullptr
               ? (ok_ = sink_.write(sink_.context, value, size))
               : false;
  }
  bool format(const char* format, ...) {
    char buffer[512] = {};
    va_list arguments;
    va_start(arguments, format);
    const int size = std::vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    if (size < 0 || static_cast<size_t>(size) >= sizeof(buffer)) {
      ok_ = false;
      return false;
    }
    return ok_ && sink_.write != nullptr
               ? (ok_ = sink_.write(sink_.context, buffer,
                                    static_cast<size_t>(size)))
               : false;
  }
  bool ok() const { return ok_; }

 private:
  CommandAckJsonSink sink_;
  bool ok_ = true;
};

class ResponseParser {
 public:
  ResponseParser(const char* input, size_t size,
                 const CommandJournalRecord& expected)
      : input_(input), size_(size), expected_(expected) {}

  CommandAckResponseResult parse() {
    if (input_ == nullptr || !commandJournalRecordIsValid(expected_)) {
      return failure(CommandAckResponseError::kNullArgument);
    }
    if (!buildCommandAckId(expected_, expectedAckId_,
                           sizeof(expectedAckId_))) {
      return failure(CommandAckResponseError::kInvalidValue);
    }
    skip();
    if (!consume('{')) return failure(CommandAckResponseError::kMalformedJson);
    skip();
    uint8_t seen = 0;
    while (!consume('}')) {
      char key[32] = {};
      if (!string(key, sizeof(key))) return failure(error_);
      skip();
      if (!consume(':')) return failure(CommandAckResponseError::kMalformedJson);
      skip();
      uint8_t bit = 0;
      if (std::strcmp(key, "ack_id") == 0) {
        bit = 1U << 0;
        char value[65] = {};
        if (!string(value, sizeof(value))) return failure(error_);
        if (std::strcmp(value, expectedAckId_) != 0) {
          return failure(CommandAckResponseError::kMismatch);
        }
      } else if (std::strcmp(key, "command_id") == 0) {
        bit = 1U << 1;
        char value[65] = {};
        if (!string(value, sizeof(value))) return failure(error_);
        if (std::strcmp(value, expected_.commandId) != 0) {
          return failure(CommandAckResponseError::kMismatch);
        }
      } else if (std::strcmp(key, "status") == 0) {
        bit = 1U << 2;
        char value[24] = {};
        if (!string(value, sizeof(value))) return failure(error_);
        if (std::strcmp(value,
                        commandExecutionStatusWireName(expected_.status)) != 0) {
          return failure(CommandAckResponseError::kMismatch);
        }
      } else if (std::strcmp(key, "duplicate") == 0) {
        bit = 1U << 3;
        if (!boolean(result_.duplicate)) return failure(error_);
      } else if (std::strcmp(key, "server_utc_ms") == 0) {
        bit = 1U << 4;
        if (!integer(result_.serverUtcMs)) return failure(error_);
      } else {
        return failure(CommandAckResponseError::kUnknownField);
      }
      if ((seen & bit) != 0) {
        return failure(CommandAckResponseError::kDuplicateField);
      }
      seen |= bit;
      skip();
      if (consume('}')) break;
      if (!consume(',')) return failure(CommandAckResponseError::kMalformedJson);
      skip();
    }
    skip();
    if (position_ != size_) return failure(CommandAckResponseError::kTrailingData);
    if (seen != 0x1f) return failure(CommandAckResponseError::kMissingField);
    result_.error = CommandAckResponseError::kNone;
    return result_;
  }

 private:
  bool string(char* output, size_t capacity) {
    if (!consume('"')) return set(CommandAckResponseError::kWrongType);
    size_t length = 0;
    while (position_ < size_) {
      const char byte = input_[position_++];
      if (byte == '"') {
        output[length] = '\0';
        return true;
      }
      if (byte == '\\' || static_cast<unsigned char>(byte) < 0x20 ||
          length + 1 >= capacity) {
        return set(CommandAckResponseError::kInvalidValue);
      }
      output[length++] = byte;
    }
    return set(CommandAckResponseError::kMalformedJson);
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
    return set(CommandAckResponseError::kWrongType);
  }

  bool integer(uint64_t& output) {
    if (position_ >= size_ || input_[position_] < '0' ||
        input_[position_] > '9') {
      return set(CommandAckResponseError::kWrongType);
    }
    uint64_t value = 0;
    do {
      const uint8_t digit = static_cast<uint8_t>(input_[position_] - '0');
      if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10U) {
        return set(CommandAckResponseError::kInvalidValue);
      }
      value = value * 10U + digit;
      ++position_;
    } while (position_ < size_ && input_[position_] >= '0' &&
             input_[position_] <= '9');
    if (value > kMaxSigned64) return set(CommandAckResponseError::kInvalidValue);
    output = value;
    return true;
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

  bool set(CommandAckResponseError error) {
    error_ = error;
    return false;
  }

  CommandAckResponseResult failure(CommandAckResponseError error) const {
    CommandAckResponseResult failed = {};
    failed.error = error;
    return failed;
  }

  const char* input_ = nullptr;
  size_t size_ = 0;
  const CommandJournalRecord& expected_;
  size_t position_ = 0;
  char expectedAckId_[CommandJournalRecord::kIdCapacity] = {};
  CommandAckResponseResult result_ = {};
  CommandAckResponseError error_ = CommandAckResponseError::kNone;
};

}  // namespace

const char* commandExecutionStatusWireName(CommandExecutionStatus status) {
  switch (status) {
    case CommandExecutionStatus::kAccepted:
      return "accepted";
    case CommandExecutionStatus::kRunning:
      return "running";
    case CommandExecutionStatus::kSucceeded:
      return "succeeded";
    case CommandExecutionStatus::kFailed:
      return "failed";
    case CommandExecutionStatus::kExpired:
      return "expired";
    case CommandExecutionStatus::kRejected:
      return "rejected";
  }
  return "rejected";
}

bool writeCommandAckJson(const CommandJournalRecord& record,
                         const CommandAckJsonSink& sink) {
  if (sink.write == nullptr || !commandJournalRecordIsValid(record)) {
    return false;
  }
  char ackId[CommandJournalRecord::kIdCapacity] = {};
  if (!buildCommandAckId(record, ackId, sizeof(ackId))) return false;
  Writer writer(sink);
  return writer.format(
             "{\"schema_version\":1,\"ack_id\":\"%s\","
             "\"command_id\":\"%s\",\"lease_id\":\"%s\","
             "\"status\":\"%s\",\"result\":null}",
             ackId, record.commandId, record.leaseId,
             commandExecutionStatusWireName(record.status)) &&
         writer.ok();
}

CommandAckResponseResult parseCommandAckResponse(
    const char* json, size_t jsonLength, const CommandJournalRecord& expected) {
  return ResponseParser(json, jsonLength, expected).parse();
}

const char* commandAckResponseErrorName(CommandAckResponseError error) {
  switch (error) {
    case CommandAckResponseError::kNone:
      return "none";
    case CommandAckResponseError::kNullArgument:
      return "null_argument";
    case CommandAckResponseError::kMalformedJson:
      return "malformed_json";
    case CommandAckResponseError::kMissingField:
      return "missing_field";
    case CommandAckResponseError::kDuplicateField:
      return "duplicate_field";
    case CommandAckResponseError::kUnknownField:
      return "unknown_field";
    case CommandAckResponseError::kWrongType:
      return "wrong_type";
    case CommandAckResponseError::kInvalidValue:
      return "invalid_value";
    case CommandAckResponseError::kMismatch:
      return "mismatch";
    case CommandAckResponseError::kTrailingData:
      return "trailing_data";
  }
  return "unknown";
}
