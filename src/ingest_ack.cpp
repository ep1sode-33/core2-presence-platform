#include "ingest_ack.h"

#include <cstddef>
#include <cstring>
#include <limits>

namespace {

constexpr uint8_t kBatchIdBit = 1U << 0;
constexpr uint8_t kStoredBit = 1U << 1;
constexpr uint8_t kDuplicatesBit = 1U << 2;
constexpr uint8_t kMaxSeqBit = 1U << 3;
constexpr uint8_t kServerUtcMsBit = 1U << 4;
constexpr uint8_t kDesiredConfigRevisionBit = 1U << 5;
constexpr uint8_t kAllFields =
    kBatchIdBit | kStoredBit | kDuplicatesBit | kMaxSeqBit |
    kServerUtcMsBit | kDesiredConfigRevisionBit;

struct TextSlice {
  const char* data;
  size_t size;
};

bool textEquals(TextSlice text, const char* expected, size_t expectedSize) {
  return text.size == expectedSize &&
         (text.size == 0 || std::memcmp(text.data, expected, text.size) == 0);
}

class AckParser {
 public:
  AckParser(const char* input, size_t inputSize, const char* expectedBatchId,
            size_t expectedBatchIdSize, uint64_t expectedRecordCount,
            uint64_t expectedMaxSeq)
      : input_(input),
        inputSize_(inputSize),
        expectedBatchId_(expectedBatchId),
        expectedBatchIdSize_(expectedBatchIdSize),
        expectedRecordCount_(expectedRecordCount),
        expectedMaxSeq_(expectedMaxSeq) {}

  IngestAckParseResult parse() {
    if ((input_ == NULL && inputSize_ != 0) ||
        (expectedBatchId_ == NULL && expectedBatchIdSize_ != 0)) {
      return failure(IngestAckParseError::kMalformedJson);
    }
    skipWhitespace();
    if (!consume('{')) {
      return failure(IngestAckParseError::kTopLevelNotObject);
    }

    skipWhitespace();
    if (consume('}')) {
      return failure(IngestAckParseError::kMissingField);
    }

    while (true) {
      TextSlice key = {NULL, 0};
      if (!parseProtocolString(key, IngestAckParseError::kMalformedJson)) {
        return failure(error_);
      }
      skipWhitespace();
      if (!consume(':')) {
        return failure(IngestAckParseError::kMalformedJson);
      }
      skipWhitespace();
      if (!parseField(key)) {
        return failure(error_);
      }

      skipWhitespace();
      if (consume('}')) {
        break;
      }
      if (!consume(',')) {
        return failure(IngestAckParseError::kMalformedJson);
      }
      skipWhitespace();
    }

    skipWhitespace();
    if (position_ != inputSize_) {
      return failure(IngestAckParseError::kTrailingData);
    }
    if (seenFields_ != kAllFields) {
      return failure(IngestAckParseError::kMissingField);
    }
    if (!batchIdMatches_) {
      return failure(IngestAckParseError::kBatchIdMismatch);
    }
    if (ack_.stored >
        std::numeric_limits<uint64_t>::max() - ack_.duplicates) {
      return failure(IngestAckParseError::kCountOverflow);
    }
    if (ack_.stored + ack_.duplicates != expectedRecordCount_) {
      return failure(IngestAckParseError::kCountMismatch);
    }
    if (ack_.maxSeq != expectedMaxSeq_) {
      return failure(IngestAckParseError::kMaxSeqMismatch);
    }
    const uint64_t maxSigned64 =
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    if (ack_.stored > maxSigned64 || ack_.duplicates > maxSigned64 ||
        ack_.maxSeq > maxSigned64 || ack_.serverUtcMs > maxSigned64 ||
        ack_.desiredConfigRevision > maxSigned64) {
      return failure(IngestAckParseError::kIntegerOutOfRange);
    }
    return IngestAckParseResult(ack_, IngestAckParseError::kNone);
  }

 private:
  bool parseField(TextSlice key) {
    uint8_t fieldBit = 0;
    if (textEquals(key, "batch_id", sizeof("batch_id") - 1)) {
      fieldBit = kBatchIdBit;
    } else if (textEquals(key, "stored", sizeof("stored") - 1)) {
      fieldBit = kStoredBit;
    } else if (textEquals(key, "duplicates", sizeof("duplicates") - 1)) {
      fieldBit = kDuplicatesBit;
    } else if (textEquals(key, "max_seq", sizeof("max_seq") - 1)) {
      fieldBit = kMaxSeqBit;
    } else if (textEquals(key, "server_utc_ms",
                          sizeof("server_utc_ms") - 1)) {
      fieldBit = kServerUtcMsBit;
    } else if (textEquals(key, "desired_config_revision",
                          sizeof("desired_config_revision") - 1)) {
      fieldBit = kDesiredConfigRevisionBit;
    } else {
      return fail(IngestAckParseError::kUnknownField);
    }

    if ((seenFields_ & fieldBit) != 0) {
      return fail(IngestAckParseError::kDuplicateField);
    }
    seenFields_ |= fieldBit;

    if (fieldBit == kBatchIdBit) {
      TextSlice batchId = {NULL, 0};
      if (!parseProtocolString(batchId, IngestAckParseError::kWrongType)) {
        return false;
      }
      batchIdMatches_ = textEquals(batchId, expectedBatchId_,
                                   expectedBatchIdSize_);
      return true;
    }
    if (fieldBit == kStoredBit) {
      return parseUnsignedInteger(ack_.stored);
    }
    if (fieldBit == kDuplicatesBit) {
      return parseUnsignedInteger(ack_.duplicates);
    }
    if (fieldBit == kMaxSeqBit) {
      return parseUnsignedInteger(ack_.maxSeq);
    }
    if (fieldBit == kServerUtcMsBit) {
      return parseUnsignedInteger(ack_.serverUtcMs);
    }
    return parseUnsignedInteger(ack_.desiredConfigRevision);
  }

  bool parseProtocolString(TextSlice& value,
                           IngestAckParseError nonStringError) {
    if (!consume('"')) {
      return fail(nonStringError);
    }
    const size_t start = position_;
    while (position_ < inputSize_) {
      const unsigned char character =
          static_cast<unsigned char>(input_[position_]);
      if (character == '"') {
        value.data = input_ + start;
        value.size = position_ - start;
        ++position_;
        return true;
      }
      if (character == '\\') {
        return fail(IngestAckParseError::kStringEscapeNotAllowed);
      }
      if (character < 0x20U) {
        return fail(IngestAckParseError::kMalformedJson);
      }
      ++position_;
    }
    return fail(IngestAckParseError::kMalformedJson);
  }

  bool parseUnsignedInteger(uint64_t& value) {
    if (position_ >= inputSize_) {
      return fail(IngestAckParseError::kWrongType);
    }
    if (input_[position_] == '-') {
      return fail(IngestAckParseError::kNegativeInteger);
    }
    if (!isDigit(input_[position_])) {
      return fail(IngestAckParseError::kWrongType);
    }

    if (input_[position_] == '0') {
      value = 0;
      ++position_;
      if (position_ < inputSize_ && isDigit(input_[position_])) {
        return fail(IngestAckParseError::kMalformedJson);
      }
      if (position_ < inputSize_ && isFractionOrExponent(input_[position_])) {
        return fail(IngestAckParseError::kWrongType);
      }
      return true;
    }

    uint64_t parsed = 0;
    while (position_ < inputSize_ && isDigit(input_[position_])) {
      const uint8_t digit = static_cast<uint8_t>(input_[position_] - '0');
      if (parsed > (std::numeric_limits<uint64_t>::max() - digit) / 10U) {
        return fail(IngestAckParseError::kIntegerOverflow);
      }
      parsed = parsed * 10U + digit;
      ++position_;
    }
    if (position_ < inputSize_ && isFractionOrExponent(input_[position_])) {
      return fail(IngestAckParseError::kWrongType);
    }
    value = parsed;
    return true;
  }

  static bool isDigit(char character) {
    return character >= '0' && character <= '9';
  }

  static bool isFractionOrExponent(char character) {
    return character == '.' || character == 'e' || character == 'E';
  }

  void skipWhitespace() {
    while (position_ < inputSize_) {
      const char character = input_[position_];
      if (character != ' ' && character != '\t' && character != '\n' &&
          character != '\r') {
        break;
      }
      ++position_;
    }
  }

  bool consume(char expected) {
    if (position_ >= inputSize_ || input_[position_] != expected) {
      return false;
    }
    ++position_;
    return true;
  }

  bool fail(IngestAckParseError error) {
    error_ = error;
    return false;
  }

  IngestAckParseResult failure(IngestAckParseError error) const {
    return IngestAckParseResult(IngestAck(), error);
  }

  const char* input_;
  size_t inputSize_;
  const char* expectedBatchId_;
  size_t expectedBatchIdSize_;
  uint64_t expectedRecordCount_;
  uint64_t expectedMaxSeq_;
  size_t position_ = 0;
  uint8_t seenFields_ = 0;
  bool batchIdMatches_ = false;
  IngestAck ack_;
  IngestAckParseError error_ = IngestAckParseError::kNone;
};

}  // namespace

IngestAckParseResult parseIngestAck(const char* responseBody,
                                    size_t responseBodyLength,
                                    const char* expectedBatchId,
                                    size_t expectedBatchIdLength,
                                    uint64_t expectedRecordCount,
                                    uint64_t expectedMaxSeq) {
  return AckParser(responseBody, responseBodyLength, expectedBatchId,
                   expectedBatchIdLength, expectedRecordCount, expectedMaxSeq)
      .parse();
}

const char* ingestAckParseErrorName(IngestAckParseError error) {
  switch (error) {
    case IngestAckParseError::kNone:
      return "none";
    case IngestAckParseError::kTopLevelNotObject:
      return "top_level_not_object";
    case IngestAckParseError::kMalformedJson:
      return "malformed_json";
    case IngestAckParseError::kMissingField:
      return "missing_field";
    case IngestAckParseError::kDuplicateField:
      return "duplicate_field";
    case IngestAckParseError::kUnknownField:
      return "unknown_field";
    case IngestAckParseError::kWrongType:
      return "wrong_type";
    case IngestAckParseError::kNegativeInteger:
      return "negative_integer";
    case IngestAckParseError::kIntegerOverflow:
      return "integer_overflow";
    case IngestAckParseError::kIntegerOutOfRange:
      return "integer_out_of_range";
    case IngestAckParseError::kStringEscapeNotAllowed:
      return "string_escape_not_allowed";
    case IngestAckParseError::kTrailingData:
      return "trailing_data";
    case IngestAckParseError::kBatchIdMismatch:
      return "batch_id_mismatch";
    case IngestAckParseError::kCountOverflow:
      return "count_overflow";
    case IngestAckParseError::kCountMismatch:
      return "count_mismatch";
    case IngestAckParseError::kMaxSeqMismatch:
      return "max_seq_mismatch";
  }
  return "unknown";
}
