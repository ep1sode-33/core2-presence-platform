#include "feedback_protocol.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <type_traits>

namespace {

constexpr uint64_t kMaxSigned64 =
    static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
constexpr size_t kMaximumAckBodyLength = 2048;

bool isLowerHex(char value) {
  return (value >= '0' && value <= '9') ||
         (value >= 'a' && value <= 'f');
}

size_t boundedLength(const char* value, size_t capacity) {
  if (value == nullptr) {
    return capacity;
  }
  size_t length = 0;
  while (length < capacity && value[length] != '\0') {
    ++length;
  }
  return length;
}

bool validBootId(const char* bootId, size_t capacity) {
  if (boundedLength(bootId, capacity) != 32) {
    return false;
  }
  for (size_t index = 0; index < 32; ++index) {
    if (!isLowerHex(bootId[index])) {
      return false;
    }
  }
  return true;
}

bool validDeviceId(const char* deviceId) {
  constexpr char kPrefix[] = "core2-";
  if (boundedLength(deviceId, 19) != 18 ||
      std::memcmp(deviceId, kPrefix, sizeof(kPrefix) - 1) != 0) {
    return false;
  }
  for (size_t index = sizeof(kPrefix) - 1; index < 18; ++index) {
    if (!isLowerHex(deviceId[index])) {
      return false;
    }
  }
  return true;
}

bool validState(PresenceState state) {
  switch (state) {
    case PresenceState::kCalibrating:
    case PresenceState::kIdle:
    case PresenceState::kPresent:
    case PresenceState::kCooldown:
      return true;
  }
  return false;
}

bool validActualPresence(ActualPresence value) {
  switch (value) {
    case ActualPresence::kPresent:
    case ActualPresence::kAbsent:
      return true;
  }
  return false;
}

bool buildFeedbackId(const char* bootId, size_t bootIdCapacity, uint64_t seq,
                     char* output, size_t outputCapacity) {
  if (!validBootId(bootId, bootIdCapacity) || seq > kMaxSigned64 ||
      output == nullptr || outputCapacity < sizeof(FeedbackRecord::feedbackId)) {
    return false;
  }
  const int written =
      std::snprintf(output, outputCapacity, "f:%s:%016llx", bootId,
                    static_cast<unsigned long long>(seq));
  return written == 51 && static_cast<size_t>(written) < outputCapacity;
}

struct TextSlice {
  const char* data;
  size_t size;
};

bool textEquals(TextSlice text, const char* expected, size_t expectedSize) {
  return text.size == expectedSize &&
         (text.size == 0 || std::memcmp(text.data, expected, text.size) == 0);
}

constexpr uint16_t kFeedbackIdBit = 1U << 0;
constexpr uint16_t kDeviceIdBit = 1U << 1;
constexpr uint16_t kBootIdBit = 1U << 2;
constexpr uint16_t kSeqBit = 1U << 3;
constexpr uint16_t kCreatedAtMsBit = 1U << 4;
constexpr uint16_t kOccurredAtMsBit = 1U << 5;
constexpr uint16_t kOccurredUptimeMsBit = 1U << 6;
constexpr uint16_t kTimeQualityBit = 1U << 7;
constexpr uint16_t kActualPresenceBit = 1U << 8;
constexpr uint16_t kObservedStateBit = 1U << 9;
constexpr uint16_t kSourceBit = 1U << 10;
constexpr uint16_t kNoteBit = 1U << 11;
constexpr uint16_t kDuplicateBit = 1U << 12;
constexpr uint16_t kAllFields =
    kFeedbackIdBit | kDeviceIdBit | kBootIdBit | kSeqBit |
    kCreatedAtMsBit | kOccurredAtMsBit | kOccurredUptimeMsBit |
    kTimeQualityBit | kActualPresenceBit | kObservedStateBit | kSourceBit |
    kNoteBit | kDuplicateBit;

class FeedbackAckParser {
 public:
  FeedbackAckParser(const char* input, size_t inputSize,
                    const char* expectedDeviceId,
                    const FeedbackRecord& expectedRecord)
      : input_(input),
        inputSize_(inputSize),
        expectedDeviceId_(expectedDeviceId),
        expectedRecord_(expectedRecord) {}

  FeedbackAckParseResult parse() {
    if (!feedbackRecordIsValid(expectedRecord_) ||
        !validDeviceId(expectedDeviceId_)) {
      return failure(FeedbackAckParseError::kInvalidExpectation);
    }
    if (inputSize_ > kMaximumAckBodyLength) {
      return failure(FeedbackAckParseError::kBodyTooLarge);
    }
    if (input_ == nullptr && inputSize_ != 0) {
      return failure(FeedbackAckParseError::kMalformedJson);
    }

    skipWhitespace();
    if (!consume('{')) {
      return failure(FeedbackAckParseError::kTopLevelNotObject);
    }
    skipWhitespace();
    if (consume('}')) {
      return failure(FeedbackAckParseError::kMissingField);
    }

    while (true) {
      TextSlice key = {nullptr, 0};
      if (!parseProtocolString(key, FeedbackAckParseError::kMalformedJson)) {
        return failure(error_);
      }
      skipWhitespace();
      if (!consume(':')) {
        return failure(FeedbackAckParseError::kMalformedJson);
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
        return failure(FeedbackAckParseError::kMalformedJson);
      }
      skipWhitespace();
    }

    skipWhitespace();
    if (position_ != inputSize_) {
      return failure(FeedbackAckParseError::kTrailingData);
    }
    if (seenFields_ != kAllFields) {
      return failure(FeedbackAckParseError::kMissingField);
    }
    FeedbackAckParseResult result = {};
    result.ack = ack_;
    result.error = FeedbackAckParseError::kNone;
    return result;
  }

 private:
  bool parseField(TextSlice key) {
    uint16_t fieldBit = 0;
    if (textEquals(key, "feedback_id", sizeof("feedback_id") - 1)) {
      fieldBit = kFeedbackIdBit;
    } else if (textEquals(key, "device_id", sizeof("device_id") - 1)) {
      fieldBit = kDeviceIdBit;
    } else if (textEquals(key, "boot_id", sizeof("boot_id") - 1)) {
      fieldBit = kBootIdBit;
    } else if (textEquals(key, "seq", sizeof("seq") - 1)) {
      fieldBit = kSeqBit;
    } else if (textEquals(key, "created_at_ms", sizeof("created_at_ms") - 1)) {
      fieldBit = kCreatedAtMsBit;
    } else if (textEquals(key, "occurred_at_ms",
                          sizeof("occurred_at_ms") - 1)) {
      fieldBit = kOccurredAtMsBit;
    } else if (textEquals(key, "occurred_uptime_ms",
                          sizeof("occurred_uptime_ms") - 1)) {
      fieldBit = kOccurredUptimeMsBit;
    } else if (textEquals(key, "time_quality", sizeof("time_quality") - 1)) {
      fieldBit = kTimeQualityBit;
    } else if (textEquals(key, "actual_presence",
                          sizeof("actual_presence") - 1)) {
      fieldBit = kActualPresenceBit;
    } else if (textEquals(key, "observed_state",
                          sizeof("observed_state") - 1)) {
      fieldBit = kObservedStateBit;
    } else if (textEquals(key, "source", sizeof("source") - 1)) {
      fieldBit = kSourceBit;
    } else if (textEquals(key, "note", sizeof("note") - 1)) {
      fieldBit = kNoteBit;
    } else if (textEquals(key, "duplicate", sizeof("duplicate") - 1)) {
      fieldBit = kDuplicateBit;
    } else {
      return fail(FeedbackAckParseError::kUnknownField);
    }

    if ((seenFields_ & fieldBit) != 0) {
      return fail(FeedbackAckParseError::kDuplicateField);
    }
    seenFields_ |= fieldBit;

    if (fieldBit == kFeedbackIdBit) {
      return parseExpectedString(expectedRecord_.feedbackId,
                                 sizeof(expectedRecord_.feedbackId) - 1,
                                 FeedbackAckParseError::kFeedbackIdMismatch);
    }
    if (fieldBit == kDeviceIdBit) {
      return parseExpectedString(expectedDeviceId_, 18,
                                 FeedbackAckParseError::kDeviceIdMismatch);
    }
    if (fieldBit == kBootIdBit) {
      return parseExpectedString(expectedRecord_.bootId, 32,
                                 FeedbackAckParseError::kRecordReferenceMismatch);
    }
    if (fieldBit == kSeqBit) {
      uint64_t value = 0;
      return parseSignedRangeInteger(value) &&
             (value == expectedRecord_.seq ||
              fail(FeedbackAckParseError::kRecordReferenceMismatch));
    }
    if (fieldBit == kCreatedAtMsBit) {
      return parseSignedRangeInteger(ack_.createdAtMs);
    }
    if (fieldBit == kOccurredAtMsBit) {
      return parseNullableSignedRangeInteger(ack_.hasOccurredAtMs,
                                             ack_.occurredAtMs);
    }
    if (fieldBit == kOccurredUptimeMsBit) {
      bool hasValue = false;
      uint64_t value = 0;
      if (!parseNullableSignedRangeInteger(hasValue, value)) {
        return false;
      }
      if (!hasValue || value != expectedRecord_.linkedSampleUptimeMs) {
        return fail(FeedbackAckParseError::kRecordReferenceMismatch);
      }
      ack_.occurredUptimeMs = value;
      return true;
    }
    if (fieldBit == kTimeQualityBit) {
      TextSlice value = {nullptr, 0};
      if (!parseProtocolString(value, FeedbackAckParseError::kWrongType)) {
        return false;
      }
      if (value.size > sizeof(ack_.timeQuality) - 1) {
        return fail(FeedbackAckParseError::kStringTooLong);
      }
      std::memcpy(ack_.timeQuality, value.data, value.size);
      ack_.timeQuality[value.size] = '\0';
      return true;
    }
    if (fieldBit == kActualPresenceBit) {
      const char* expected =
          actualPresenceWireName(expectedRecord_.actualPresence);
      return parseExpectedString(expected, std::strlen(expected),
                                 FeedbackAckParseError::kPayloadMismatch);
    }
    if (fieldBit == kObservedStateBit) {
      const char* expected =
          presenceStateWireName(expectedRecord_.observedState);
      return parseExpectedString(expected, std::strlen(expected),
                                 FeedbackAckParseError::kPayloadMismatch);
    }
    if (fieldBit == kSourceBit) {
      return parseExpectedString("touch", sizeof("touch") - 1,
                                 FeedbackAckParseError::kPayloadMismatch);
    }
    if (fieldBit == kNoteBit) {
      if (consumeLiteral("null", sizeof("null") - 1)) {
        return true;
      }
      if (position_ < inputSize_ && input_[position_] == '"') {
        TextSlice ignored = {nullptr, 0};
        if (!parseProtocolString(ignored, FeedbackAckParseError::kWrongType)) {
          return false;
        }
        return fail(FeedbackAckParseError::kPayloadMismatch);
      }
      return fail(FeedbackAckParseError::kWrongType);
    }
    return parseBoolean(ack_.duplicate);
  }

  bool parseExpectedString(const char* expected, size_t expectedSize,
                           FeedbackAckParseError mismatchError) {
    TextSlice value = {nullptr, 0};
    if (!parseProtocolString(value, FeedbackAckParseError::kWrongType)) {
      return false;
    }
    return textEquals(value, expected, expectedSize) || fail(mismatchError);
  }

  bool parseProtocolString(TextSlice& output,
                           FeedbackAckParseError nonStringError) {
    if (!consume('"')) {
      return fail(nonStringError);
    }
    const size_t start = position_;
    while (position_ < inputSize_) {
      const unsigned char character =
          static_cast<unsigned char>(input_[position_]);
      if (character == '"') {
        output.data = input_ + start;
        output.size = position_ - start;
        ++position_;
        return true;
      }
      if (character == '\\') {
        return fail(FeedbackAckParseError::kStringEscapeNotAllowed);
      }
      // Every string in the generated device ACK is ASCII. Reject high bytes
      // instead of accidentally accepting malformed UTF-8 as valid JSON.
      if (character < 0x20U || character >= 0x80U) {
        return fail(FeedbackAckParseError::kMalformedJson);
      }
      ++position_;
    }
    return fail(FeedbackAckParseError::kMalformedJson);
  }

  bool parseSignedRangeInteger(uint64_t& output) {
    if (!parseUnsignedInteger(output)) {
      return false;
    }
    return output <= kMaxSigned64 ||
           fail(FeedbackAckParseError::kIntegerOutOfRange);
  }

  bool parseNullableSignedRangeInteger(bool& hasValue, uint64_t& output) {
    if (consumeLiteral("null", sizeof("null") - 1)) {
      hasValue = false;
      output = 0;
      return true;
    }
    hasValue = true;
    return parseSignedRangeInteger(output);
  }

  bool parseUnsignedInteger(uint64_t& output) {
    if (position_ >= inputSize_) {
      return fail(FeedbackAckParseError::kWrongType);
    }
    if (input_[position_] == '-') {
      return fail(FeedbackAckParseError::kNegativeInteger);
    }
    if (!isDigit(input_[position_])) {
      return fail(FeedbackAckParseError::kWrongType);
    }
    if (input_[position_] == '0') {
      output = 0;
      ++position_;
      if (position_ < inputSize_ && isDigit(input_[position_])) {
        return fail(FeedbackAckParseError::kMalformedJson);
      }
      if (position_ < inputSize_ && isFractionOrExponent(input_[position_])) {
        return fail(FeedbackAckParseError::kWrongType);
      }
      return true;
    }

    uint64_t parsed = 0;
    while (position_ < inputSize_ && isDigit(input_[position_])) {
      const uint8_t digit = static_cast<uint8_t>(input_[position_] - '0');
      if (parsed > (std::numeric_limits<uint64_t>::max() - digit) / 10U) {
        return fail(FeedbackAckParseError::kIntegerOverflow);
      }
      parsed = parsed * 10U + digit;
      ++position_;
    }
    if (position_ < inputSize_ && isFractionOrExponent(input_[position_])) {
      return fail(FeedbackAckParseError::kWrongType);
    }
    output = parsed;
    return true;
  }

  bool parseBoolean(bool& output) {
    if (consumeLiteral("true", sizeof("true") - 1)) {
      output = true;
      return true;
    }
    if (consumeLiteral("false", sizeof("false") - 1)) {
      output = false;
      return true;
    }
    return fail(FeedbackAckParseError::kWrongType);
  }

  static bool isDigit(char value) {
    return value >= '0' && value <= '9';
  }

  static bool isFractionOrExponent(char value) {
    return value == '.' || value == 'e' || value == 'E';
  }

  void skipWhitespace() {
    while (position_ < inputSize_) {
      const char value = input_[position_];
      if (value != ' ' && value != '\t' && value != '\n' && value != '\r') {
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

  bool consumeLiteral(const char* expected, size_t expectedSize) {
    if (expectedSize > inputSize_ - position_ ||
        std::memcmp(input_ + position_, expected, expectedSize) != 0) {
      return false;
    }
    position_ += expectedSize;
    return true;
  }

  bool fail(FeedbackAckParseError error) {
    error_ = error;
    return false;
  }

  static FeedbackAckParseResult failure(FeedbackAckParseError error) {
    FeedbackAckParseResult result = {};
    result.error = error;
    return result;
  }

  const char* input_;
  size_t inputSize_;
  const char* expectedDeviceId_;
  const FeedbackRecord& expectedRecord_;
  size_t position_ = 0;
  uint16_t seenFields_ = 0;
  FeedbackAck ack_ = {};
  FeedbackAckParseError error_ = FeedbackAckParseError::kNone;
};

static_assert(std::is_trivial<QueuedSampleReference>::value,
              "queued reference must remain a POD");
static_assert(std::is_standard_layout<QueuedSampleReference>::value,
              "queued reference must remain a POD");
static_assert(std::is_trivial<FeedbackRecord>::value,
              "feedback record must remain a POD");
static_assert(std::is_standard_layout<FeedbackRecord>::value,
              "feedback record must remain a POD");

}  // namespace

const char* actualPresenceWireName(ActualPresence value) {
  switch (value) {
    case ActualPresence::kPresent:
      return "present";
    case ActualPresence::kAbsent:
      return "absent";
  }
  return nullptr;
}

bool actualPresenceForTouchChoice(TouchPresenceChoice choice,
                                  ActualPresence& output) {
  switch (choice) {
    case TouchPresenceChoice::kPersonWasPresent:
      output = ActualPresence::kPresent;
      return true;
    case TouchPresenceChoice::kRoomWasAbsent:
      output = ActualPresence::kAbsent;
      return true;
  }
  return false;
}

bool bindQueuedSampleForTouchFeedback(const char* bootId,
                                      const TelemetryRecord& sample,
                                      QueuePushResult enqueueResult,
                                      QueuedSampleReference& output) {
  if (enqueueResult != QueuePushResult::kStored ||
      sample.kind != TelemetryKind::kSample ||
      !validBootId(bootId, 33) || sample.seq > kMaxSigned64 ||
      sample.uptimeMs > kMaxSigned64 || !validState(sample.sample.state)) {
    return false;
  }
  QueuedSampleReference candidate = {};
  std::memcpy(candidate.bootId, bootId, 33);
  candidate.seq = sample.seq;
  candidate.uptimeMs = sample.uptimeMs;
  candidate.preTouchObservedState = sample.sample.state;
  output = candidate;
  return true;
}

bool buildTouchFeedbackRecord(const QueuedSampleReference& reference,
                              TouchPresenceChoice choice,
                              FeedbackRecord& output) {
  ActualPresence actualPresence = ActualPresence::kAbsent;
  if (!validBootId(reference.bootId, sizeof(reference.bootId)) ||
      reference.seq > kMaxSigned64 || reference.uptimeMs > kMaxSigned64 ||
      !validState(reference.preTouchObservedState) ||
      !actualPresenceForTouchChoice(choice, actualPresence)) {
    return false;
  }

  FeedbackRecord candidate = {};
  std::memcpy(candidate.bootId, reference.bootId, sizeof(candidate.bootId));
  candidate.seq = reference.seq;
  candidate.linkedSampleUptimeMs = reference.uptimeMs;
  candidate.actualPresence = actualPresence;
  candidate.observedState = reference.preTouchObservedState;
  if (!buildFeedbackId(candidate.bootId, sizeof(candidate.bootId), candidate.seq,
                       candidate.feedbackId, sizeof(candidate.feedbackId))) {
    return false;
  }
  output = candidate;
  return true;
}

bool feedbackRecordIsValid(const FeedbackRecord& record) {
  if (!validBootId(record.bootId, sizeof(record.bootId)) ||
      record.seq > kMaxSigned64 || record.linkedSampleUptimeMs > kMaxSigned64 ||
      !validActualPresence(record.actualPresence) ||
      !validState(record.observedState)) {
    return false;
  }
  char expectedId[sizeof(record.feedbackId)] = {};
  return buildFeedbackId(record.bootId, sizeof(record.bootId), record.seq,
                         expectedId, sizeof(expectedId)) &&
         boundedLength(record.feedbackId, sizeof(record.feedbackId)) == 51 &&
         std::memcmp(record.feedbackId, expectedId, sizeof(expectedId)) == 0;
}

bool writeTouchFeedbackJson(const FeedbackRecord& record,
                            const FeedbackJsonSink& sink) {
  if (!feedbackRecordIsValid(record) || sink.write == nullptr) {
    return false;
  }
  char payload[320] = {};
  const int written = std::snprintf(
      payload, sizeof(payload),
      "{\"feedback_id\":\"%s\",\"boot_id\":\"%s\",\"seq\":%llu,"
      "\"actual_presence\":\"%s\",\"observed_state\":\"%s\","
      "\"source\":\"touch\"}",
      record.feedbackId, record.bootId,
      static_cast<unsigned long long>(record.seq),
      actualPresenceWireName(record.actualPresence),
      presenceStateWireName(record.observedState));
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(payload)) {
    return false;
  }
  return sink.write(sink.context, payload, static_cast<size_t>(written));
}

FeedbackAckParseResult parseFeedbackAck(const char* responseBody,
                                        size_t responseBodyLength,
                                        const char* expectedDeviceId,
                                        const FeedbackRecord& expectedRecord) {
  return FeedbackAckParser(responseBody, responseBodyLength, expectedDeviceId,
                           expectedRecord)
      .parse();
}

const char* feedbackAckParseErrorName(FeedbackAckParseError error) {
  switch (error) {
    case FeedbackAckParseError::kNone:
      return "none";
    case FeedbackAckParseError::kInvalidExpectation:
      return "invalid_expectation";
    case FeedbackAckParseError::kBodyTooLarge:
      return "body_too_large";
    case FeedbackAckParseError::kTopLevelNotObject:
      return "top_level_not_object";
    case FeedbackAckParseError::kMalformedJson:
      return "malformed_json";
    case FeedbackAckParseError::kMissingField:
      return "missing_field";
    case FeedbackAckParseError::kDuplicateField:
      return "duplicate_field";
    case FeedbackAckParseError::kUnknownField:
      return "unknown_field";
    case FeedbackAckParseError::kWrongType:
      return "wrong_type";
    case FeedbackAckParseError::kNegativeInteger:
      return "negative_integer";
    case FeedbackAckParseError::kIntegerOverflow:
      return "integer_overflow";
    case FeedbackAckParseError::kIntegerOutOfRange:
      return "integer_out_of_range";
    case FeedbackAckParseError::kStringEscapeNotAllowed:
      return "string_escape_not_allowed";
    case FeedbackAckParseError::kStringTooLong:
      return "string_too_long";
    case FeedbackAckParseError::kTrailingData:
      return "trailing_data";
    case FeedbackAckParseError::kFeedbackIdMismatch:
      return "feedback_id_mismatch";
    case FeedbackAckParseError::kDeviceIdMismatch:
      return "device_id_mismatch";
    case FeedbackAckParseError::kRecordReferenceMismatch:
      return "record_reference_mismatch";
    case FeedbackAckParseError::kPayloadMismatch:
      return "payload_mismatch";
  }
  return "unknown";
}
