#pragma once

#include <cstddef>
#include <cstdint>

#include "presence_types.h"
#include "telemetry.h"

enum class ActualPresence : uint8_t {
  kPresent,
  kAbsent,
};

// This is a semantic correction chosen by the UI layer. Deliberately no touch
// coordinate, button, click, or long-press is mapped here.
enum class TouchPresenceChoice : uint8_t {
  kPersonWasPresent,
  kRoomWasAbsent,
};

const char* actualPresenceWireName(ActualPresence value);
bool actualPresenceForTouchChoice(TouchPresenceChoice choice,
                                  ActualPresence& output);

// The only supported way to obtain this reference is after TelemetryQueue has
// accepted the exact sample record. The boot identity and sequence are copied,
// so later boot changes cannot retarget feedback that is already durable.
struct QueuedSampleReference {
  char bootId[33];
  uint64_t seq;
  uint64_t uptimeMs;
  PresenceState preTouchObservedState;
};

bool bindQueuedSampleForTouchFeedback(const char* bootId,
                                      const TelemetryRecord& sample,
                                      QueuePushResult enqueueResult,
                                      QueuedSampleReference& output);

// Plain, fixed-size record suitable for copying into a static queue or binary
// spool metadata. feedbackId is deterministically bound to (bootId, seq):
//     f:<32-lower-hex-boot-id>:<16-lower-hex-seq>
// Because each correction snapshots and queues a new sample first, this pair
// identifies one feedback record globally for the lifetime of the deployment.
struct FeedbackRecord {
  char feedbackId[52];
  char bootId[33];
  uint64_t seq;
  uint64_t linkedSampleUptimeMs;
  ActualPresence actualPresence;
  PresenceState observedState;
};

// The observed state comes only from the already-queued pre-touch sample.
// Gesture-to-choice mapping remains the caller's responsibility.
bool buildTouchFeedbackRecord(const QueuedSampleReference& reference,
                              TouchPresenceChoice choice,
                              FeedbackRecord& output);

bool feedbackRecordIsValid(const FeedbackRecord& record);

using FeedbackJsonWrite = bool (*)(void* context, const char* data,
                                   size_t size);

struct FeedbackJsonSink {
  void* context;
  FeedbackJsonWrite write;
};

// Emits exactly the device-originated FeedbackCreate shape. `source` is always
// "touch" and `note` is omitted, so an exact response must echo note=null.
// The record is fully validated before the sink is called.
bool writeTouchFeedbackJson(const FeedbackRecord& record,
                            const FeedbackJsonSink& sink);

enum class FeedbackAckParseError : uint8_t {
  kNone,
  kInvalidExpectation,
  kBodyTooLarge,
  kTopLevelNotObject,
  kMalformedJson,
  kMissingField,
  kDuplicateField,
  kUnknownField,
  kWrongType,
  kNegativeInteger,
  kIntegerOverflow,
  kIntegerOutOfRange,
  kStringEscapeNotAllowed,
  kStringTooLong,
  kTrailingData,
  kFeedbackIdMismatch,
  kDeviceIdMismatch,
  kRecordReferenceMismatch,
  kPayloadMismatch,
};

struct FeedbackAck {
  uint64_t createdAtMs;
  bool hasOccurredAtMs;
  uint64_t occurredAtMs;
  uint64_t occurredUptimeMs;
  char timeQuality[33];
  bool duplicate;
};

struct FeedbackAckParseResult {
  FeedbackAck ack;
  FeedbackAckParseError error;

  bool ok() const { return error == FeedbackAckParseError::kNone; }
  explicit operator bool() const { return ok(); }
};

// Parses FeedbackResponse with additionalProperties=false semantics: every
// response field must appear exactly once, unknown/duplicate fields and
// trailing bytes are rejected, and all identity/reference/payload fields must
// exactly match the immutable request. JSON escapes are intentionally not part
// of this generated-ASCII wire subset.
FeedbackAckParseResult parseFeedbackAck(const char* responseBody,
                                        size_t responseBodyLength,
                                        const char* expectedDeviceId,
                                        const FeedbackRecord& expectedRecord);

const char* feedbackAckParseErrorName(FeedbackAckParseError error);
