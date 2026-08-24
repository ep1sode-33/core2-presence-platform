#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <type_traits>

#include "feedback_protocol.h"

namespace {

constexpr char kBootId[] = "0123456789abcdef0123456789abcdef";
constexpr char kDeviceId[] = "core2-0123456789ab";

bool appendString(void* context, const char* data, size_t size) {
  static_cast<std::string*>(context)->append(data, size);
  return true;
}

bool rejectWrite(void*, const char*, size_t) { return false; }

TelemetryRecord preTouchSample() {
  TelemetryRecord sample = {};
  sample.kind = TelemetryKind::kSample;
  sample.seq = 42;
  sample.uptimeMs = 12345;
  sample.sample.state = PresenceState::kCooldown;
  return sample;
}

std::string validAck(bool duplicate = false, bool anchored = false) {
  return std::string(
             "{\"feedback_id\":\"f:0123456789abcdef0123456789abcdef:"
             "000000000000002a\",\"device_id\":\"core2-0123456789ab\","
             "\"boot_id\":\"0123456789abcdef0123456789abcdef\",\"seq\":42,"
             "\"created_at_ms\":1800000000123,\"occurred_at_ms\":") +
         (anchored ? "1799999999999" : "null") +
         ",\"occurred_uptime_ms\":12345,\"time_quality\":\"" +
         (anchored ? "anchor_sntp" : "receive_only") +
         "\",\"actual_presence\":\"present\",\"observed_state\":"
         "\"cooldown\",\"source\":\"touch\",\"note\":null,"
         "\"duplicate\":" +
         (duplicate ? "true" : "false") + "}";
}

std::string replaceOnce(std::string input, const std::string& from,
                        const std::string& to) {
  const size_t position = input.find(from);
  assert(position != std::string::npos);
  input.replace(position, from.size(), to);
  return input;
}

FeedbackAckParseResult parse(const std::string& body,
                             const FeedbackRecord& record) {
  return parseFeedbackAck(body.data(), body.size(), kDeviceId, record);
}

void expectError(const std::string& body, FeedbackAckParseError error,
                 const FeedbackRecord& record) {
  const FeedbackAckParseResult result = parse(body, record);
  assert(!result.ok());
  assert(result.error == error);
  assert(std::strcmp(feedbackAckParseErrorName(error), "unknown") != 0);
}

}  // namespace

int main() {
  static_assert(std::is_trivial<QueuedSampleReference>::value,
                "reference must remain trivial");
  static_assert(std::is_standard_layout<QueuedSampleReference>::value,
                "reference must remain standard-layout");
  static_assert(std::is_trivial<FeedbackRecord>::value,
                "record must remain trivial");
  static_assert(std::is_standard_layout<FeedbackRecord>::value,
                "record must remain standard-layout");

  const TelemetryRecord sample = preTouchSample();
  QueuedSampleReference reference = {};
  assert(bindQueuedSampleForTouchFeedback(
      kBootId, sample, QueuePushResult::kStored, reference));
  assert(reference.seq == sample.seq);
  assert(reference.uptimeMs == sample.uptimeMs);
  assert(reference.preTouchObservedState == PresenceState::kCooldown);

  QueuedSampleReference unchanged = reference;
  TelemetryRecord transition = sample;
  transition.kind = TelemetryKind::kTransition;
  assert(!bindQueuedSampleForTouchFeedback(
      kBootId, transition, QueuePushResult::kStored, unchanged));
  assert(unchanged.seq == reference.seq);
  assert(!bindQueuedSampleForTouchFeedback(
      kBootId, sample, QueuePushResult::kSampleDropped, unchanged));
  assert(!bindQueuedSampleForTouchFeedback(
      "bad", sample, QueuePushResult::kStored, unchanged));

  FeedbackRecord present = {};
  assert(buildTouchFeedbackRecord(
      reference, TouchPresenceChoice::kPersonWasPresent, present));
  assert(feedbackRecordIsValid(present));
  assert(std::strcmp(
             present.feedbackId,
             "f:0123456789abcdef0123456789abcdef:000000000000002a") ==
         0);
  assert(std::strcmp(present.bootId, kBootId) == 0);
  assert(present.seq == 42);
  assert(present.linkedSampleUptimeMs == 12345);
  assert(present.actualPresence == ActualPresence::kPresent);
  assert(present.observedState == PresenceState::kCooldown);

  FeedbackRecord absent = {};
  assert(buildTouchFeedbackRecord(
      reference, TouchPresenceChoice::kRoomWasAbsent, absent));
  assert(absent.actualPresence == ActualPresence::kAbsent);
  assert(std::strcmp(actualPresenceWireName(absent.actualPresence), "absent") ==
         0);
  ActualPresence mapped = ActualPresence::kAbsent;
  assert(actualPresenceForTouchChoice(
      TouchPresenceChoice::kPersonWasPresent, mapped));
  assert(mapped == ActualPresence::kPresent);
  assert(!actualPresenceForTouchChoice(
      static_cast<TouchPresenceChoice>(255), mapped));

  FeedbackRecord unchangedRecord = present;
  QueuedSampleReference invalidReference = reference;
  invalidReference.preTouchObservedState = static_cast<PresenceState>(255);
  assert(!buildTouchFeedbackRecord(
      invalidReference, TouchPresenceChoice::kPersonWasPresent,
      unchangedRecord));
  assert(std::memcmp(&unchangedRecord, &present, sizeof(present)) == 0);

  std::string request;
  const FeedbackJsonSink sink = {&request, appendString};
  assert(writeTouchFeedbackJson(present, sink));
  assert(request ==
         "{\"feedback_id\":\"f:0123456789abcdef0123456789abcdef:"
         "000000000000002a\",\"boot_id\":"
         "\"0123456789abcdef0123456789abcdef\",\"seq\":42,"
         "\"actual_presence\":\"present\",\"observed_state\":"
         "\"cooldown\",\"source\":\"touch\"}");
  if (std::getenv("EMIT_FEEDBACK_JSON") != nullptr) {
    std::cout << request;
  }
  const FeedbackJsonSink rejectingSink = {nullptr, rejectWrite};
  assert(!writeTouchFeedbackJson(present, rejectingSink));
  FeedbackRecord mutated = present;
  mutated.feedbackId[2] = '1';
  assert(!feedbackRecordIsValid(mutated));
  assert(!writeTouchFeedbackJson(mutated, sink));

  {
    const FeedbackAckParseResult result = parse(validAck(), present);
    assert(result.ok());
    assert(result.ack.createdAtMs == 1800000000123ULL);
    assert(!result.ack.hasOccurredAtMs);
    assert(result.ack.occurredAtMs == 0);
    assert(result.ack.occurredUptimeMs == 12345);
    assert(std::strcmp(result.ack.timeQuality, "receive_only") == 0);
    assert(!result.ack.duplicate);
  }
  {
    const FeedbackAckParseResult result = parse(validAck(true, true), present);
    assert(result.ok());
    assert(result.ack.hasOccurredAtMs);
    assert(result.ack.occurredAtMs == 1799999999999ULL);
    assert(std::strcmp(result.ack.timeQuality, "anchor_sntp") == 0);
    assert(result.ack.duplicate);
  }
  {
    const std::string reordered =
        " \n{\"duplicate\":false,\"note\":null,\"source\":\"touch\","
        "\"observed_state\":\"cooldown\",\"actual_presence\":\"present\","
        "\"time_quality\":\"receive_only\",\"occurred_uptime_ms\":12345,"
        "\"occurred_at_ms\":null,\"created_at_ms\":9,\"seq\":42,"
        "\"boot_id\":\"0123456789abcdef0123456789abcdef\","
        "\"device_id\":\"core2-0123456789ab\","
        "\"feedback_id\":\"f:0123456789abcdef0123456789abcdef:"
        "000000000000002a\"}\r\n";
    assert(parse(reordered, present).ok());
  }

  expectError(replaceOnce(validAck(), "000000000000002a\",\"device_id",
                          "000000000000002b\",\"device_id"),
              FeedbackAckParseError::kFeedbackIdMismatch, present);
  expectError(replaceOnce(validAck(), "core2-0123456789ab",
                          "core2-0123456789ac"),
              FeedbackAckParseError::kDeviceIdMismatch, present);
  expectError(replaceOnce(validAck(), kBootId,
                          "1123456789abcdef0123456789abcdef"),
              FeedbackAckParseError::kFeedbackIdMismatch, present);
  expectError(replaceOnce(validAck(), "\"seq\":42", "\"seq\":43"),
              FeedbackAckParseError::kRecordReferenceMismatch, present);
  expectError(replaceOnce(validAck(), "\"occurred_uptime_ms\":12345",
                          "\"occurred_uptime_ms\":12346"),
              FeedbackAckParseError::kRecordReferenceMismatch, present);
  expectError(replaceOnce(validAck(), "\"occurred_uptime_ms\":12345",
                          "\"occurred_uptime_ms\":null"),
              FeedbackAckParseError::kRecordReferenceMismatch, present);
  expectError(replaceOnce(validAck(), "\"actual_presence\":\"present\"",
                          "\"actual_presence\":\"absent\""),
              FeedbackAckParseError::kPayloadMismatch, present);
  expectError(replaceOnce(validAck(), "\"observed_state\":\"cooldown\"",
                          "\"observed_state\":\"present\""),
              FeedbackAckParseError::kPayloadMismatch, present);
  expectError(replaceOnce(validAck(), "\"source\":\"touch\"",
                          "\"source\":\"web\""),
              FeedbackAckParseError::kPayloadMismatch, present);
  expectError(replaceOnce(validAck(), "\"note\":null",
                          "\"note\":\"changed\""),
              FeedbackAckParseError::kPayloadMismatch, present);
  expectError(replaceOnce(validAck(), "\"duplicate\":false",
                          "\"duplicate\":\"false\""),
              FeedbackAckParseError::kWrongType, present);
  expectError(replaceOnce(validAck(), "\"created_at_ms\":1800000000123",
                          "\"created_at_ms\":-1"),
              FeedbackAckParseError::kNegativeInteger, present);
  expectError(replaceOnce(validAck(), "\"created_at_ms\":1800000000123",
                          "\"created_at_ms\":18446744073709551616"),
              FeedbackAckParseError::kIntegerOverflow, present);
  expectError(replaceOnce(validAck(), "\"created_at_ms\":1800000000123",
                          "\"created_at_ms\":9223372036854775808"),
              FeedbackAckParseError::kIntegerOutOfRange, present);
  expectError(replaceOnce(validAck(), "\"duplicate\":false",
                          "\"extra\":1,\"duplicate\":false"),
              FeedbackAckParseError::kUnknownField, present);
  expectError(replaceOnce(validAck(), "\"duplicate\":false",
                          "\"duplicate\":false,\"duplicate\":false"),
              FeedbackAckParseError::kDuplicateField, present);
  expectError(replaceOnce(validAck(), ",\"duplicate\":false", ""),
              FeedbackAckParseError::kMissingField, present);
  expectError(validAck() + " garbage", FeedbackAckParseError::kTrailingData,
              present);
  expectError("[]", FeedbackAckParseError::kTopLevelNotObject, present);
  expectError(replaceOnce(validAck(), "f:012345", "f\\u003a012345"),
              FeedbackAckParseError::kStringEscapeNotAllowed, present);
  expectError(replaceOnce(validAck(), "receive_only", std::string(33, 'a')),
              FeedbackAckParseError::kStringTooLong, present);
  expectError(replaceOnce(validAck(), "receive_only",
                          std::string("receive_") + '\x80' + "only"),
              FeedbackAckParseError::kMalformedJson, present);
  expectError(std::string(2049, ' '), FeedbackAckParseError::kBodyTooLarge,
              present);

  FeedbackRecord invalidExpected = present;
  invalidExpected.feedbackId[0] = 'x';
  const FeedbackAckParseResult invalidRecord = parseFeedbackAck(
      validAck().data(), validAck().size(), kDeviceId, invalidExpected);
  assert(invalidRecord.error == FeedbackAckParseError::kInvalidExpectation);
  const std::string body = validAck();
  const FeedbackAckParseResult invalidDevice = parseFeedbackAck(
      body.data(), body.size(), "wrong", present);
  assert(invalidDevice.error == FeedbackAckParseError::kInvalidExpectation);

  return 0;
}
