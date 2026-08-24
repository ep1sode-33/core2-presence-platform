#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

#include "touch_feedback_queue.h"

namespace {

constexpr char kBootId[] = "0123456789abcdef0123456789abcdef";

TelemetryRecord sampleFor(uint64_t seq) {
  TelemetryRecord sample = {};
  sample.kind = TelemetryKind::kSample;
  sample.seq = seq;
  sample.uptimeMs = 1000 + seq;
  sample.appliedConfigRevision = 7;
  sample.sample.pir = (seq % 2U) != 0U;
  sample.sample.micRms = 10.0f + static_cast<float>(seq);
  sample.sample.micEnvelope = 8.5f;
  sample.sample.micMin = -100;
  sample.sample.micMax = 200;
  sample.sample.noiseFloor = 7.0f;
  sample.sample.soundThreshold = 9.0f;
  sample.sample.soundActive = true;
  sample.sample.state = PresenceState::kCooldown;
  sample.sample.brightness = 123;
  return sample;
}

TouchFeedbackEvidence evidenceFor(uint64_t seq) {
  TouchFeedbackEvidence evidence = {};
  const TouchPresenceChoice choice =
      (seq % 2U) == 0U ? TouchPresenceChoice::kPersonWasPresent
                       : TouchPresenceChoice::kRoomWasAbsent;
  assert(buildTouchFeedbackEvidence(kBootId, sampleFor(seq), choice,
                                    evidence));
  return evidence;
}

}  // namespace

int main() {
  static_assert(std::is_standard_layout<TouchFeedbackEvidence>::value,
                "evidence must remain standard-layout");
  static_assert(std::is_trivially_copyable<TouchFeedbackEvidence>::value,
                "evidence must remain trivially copyable");

  const TouchFeedbackEvidence valid = evidenceFor(42);
  assert(touchFeedbackEvidenceIsValid(valid));
  assert(valid.preTouchSample.seq == valid.feedback.seq);
  assert(valid.preTouchSample.uptimeMs ==
         valid.feedback.linkedSampleUptimeMs);
  assert(valid.preTouchSample.sample.state == valid.feedback.observedState);

  TouchFeedbackEvidence unchanged = valid;
  TelemetryRecord invalidSample = sampleFor(43);
  invalidSample.kind = TelemetryKind::kTransition;
  assert(!buildTouchFeedbackEvidence(
      kBootId, invalidSample, TouchPresenceChoice::kPersonWasPresent,
      unchanged));
  assert(unchanged.preTouchSample.seq == valid.preTouchSample.seq);

  invalidSample = sampleFor(43);
  invalidSample.sample.micRms = std::numeric_limits<float>::quiet_NaN();
  assert(!buildTouchFeedbackEvidence(
      kBootId, invalidSample, TouchPresenceChoice::kPersonWasPresent,
      unchanged));
  assert(!buildTouchFeedbackEvidence(
      "bad", sampleFor(43), TouchPresenceChoice::kPersonWasPresent,
      unchanged));
  assert(!buildTouchFeedbackEvidence(
      kBootId, sampleFor(43), static_cast<TouchPresenceChoice>(255),
      unchanged));

  TouchFeedbackEvidence mismatched = valid;
  ++mismatched.feedback.seq;
  assert(!touchFeedbackEvidenceIsValid(mismatched));
  mismatched = valid;
  ++mismatched.feedback.linkedSampleUptimeMs;
  assert(!touchFeedbackEvidenceIsValid(mismatched));
  mismatched = valid;
  mismatched.feedback.observedState = PresenceState::kPresent;
  assert(!touchFeedbackEvidenceIsValid(mismatched));

  TouchFeedbackQueue queue;
  assert(queue.capacity() == 16);
  assert(queue.size() == 0);
  assert(queue.copyPrefix(nullptr, 1) == 0);
  assert(queue.copyPrefix(&unchanged, 0) == 0);
  assert(!queue.commitPrefix(nullptr, 1));
  assert(!queue.commitPrefix(&unchanged, 0));

  for (uint64_t seq = 0; seq < TouchFeedbackQueue::kCapacity; ++seq) {
    const TouchFeedbackEvidence evidence = evidenceFor(seq);
    assert(queue.push(evidence) == TouchFeedbackQueuePushResult::kStored);
  }
  assert(queue.size() == TouchFeedbackQueue::kCapacity);
  assert(queue.push(evidenceFor(16)) == TouchFeedbackQueuePushResult::kFull);
  assert(queue.droppedFull() == 1);

  TouchFeedbackEvidence firstFive[5] = {};
  assert(queue.copyPrefix(firstFive, 5) == 5);
  for (size_t index = 0; index < 5; ++index) {
    assert(firstFive[index].preTouchSample.seq == index);
    assert(firstFive[index].feedback.seq == index);
  }

  TouchFeedbackEvidence mutatedPrefix[5] = {};
  std::memcpy(mutatedPrefix, firstFive, sizeof(firstFive));
  ++mutatedPrefix[2].preTouchSample.sample.brightness;
  assert(queue.commitPrefix(mutatedPrefix, 5) == false);
  assert(queue.size() == TouchFeedbackQueue::kCapacity);
  assert(queue.commitPrefix(firstFive, 5));
  assert(queue.size() == TouchFeedbackQueue::kCapacity - 5);

  // These writes cross the physical end of the ring. FIFO copying must still
  // return each complete sample+feedback pair in order.
  for (uint64_t seq = 16; seq < 21; ++seq) {
    assert(queue.push(evidenceFor(seq)) ==
           TouchFeedbackQueuePushResult::kStored);
  }
  assert(queue.size() == TouchFeedbackQueue::kCapacity);

  TouchFeedbackEvidence wrapped[TouchFeedbackQueue::kCapacity] = {};
  assert(queue.copyPrefix(wrapped, TouchFeedbackQueue::kCapacity) ==
         TouchFeedbackQueue::kCapacity);
  for (size_t index = 0; index < TouchFeedbackQueue::kCapacity; ++index) {
    const uint64_t expectedSeq = static_cast<uint64_t>(index + 5);
    assert(wrapped[index].preTouchSample.seq == expectedSeq);
    assert(wrapped[index].feedback.seq == expectedSeq);
    assert(touchFeedbackEvidenceIsValid(wrapped[index]));
  }
  assert(queue.commitPrefix(wrapped, TouchFeedbackQueue::kCapacity));
  assert(queue.size() == 0);
  assert(!queue.commitPrefix(wrapped, TouchFeedbackQueue::kCapacity));

  mismatched = evidenceFor(100);
  mismatched.feedback.feedbackId[2] = '1';
  assert(queue.push(mismatched) == TouchFeedbackQueuePushResult::kInvalid);
  assert(queue.rejectedInvalid() == 1);
  assert(queue.size() == 0);

  return 0;
}
