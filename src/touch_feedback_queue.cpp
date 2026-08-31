#include "touch_feedback_queue.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace {

constexpr uint64_t kMaxSigned64 = 0x7fffffffffffffffULL;
constexpr float kMaximumSensorMetric = 1000000000.0f;

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

bool validBootId(const char* bootId) {
  if (bootId == nullptr) {
    return false;
  }
  for (size_t index = 0; index < 32; ++index) {
    const char value = bootId[index];
    if (!((value >= '0' && value <= '9') ||
          (value >= 'a' && value <= 'f'))) {
      return false;
    }
  }
  return bootId[32] == '\0';
}

bool validMetric(float value) {
  return std::isfinite(value) && value >= 0.0f &&
         value <= kMaximumSensorMetric;
}

bool canonicalUnusedTransition(const TransitionTelemetry& transition) {
  const TransitionTelemetry empty = {};
  return transition.hasFromState == empty.hasFromState &&
         transition.fromState == empty.fromState &&
         transition.toState == empty.toState &&
         transition.reason == empty.reason && transition.pir == empty.pir &&
         transition.pirAgeMs == empty.pirAgeMs &&
         transition.soundActive == empty.soundActive &&
         transition.soundAgeMs == empty.soundAgeMs &&
         transition.micEnvelope == empty.micEnvelope &&
         transition.noiseFloor == empty.noiseFloor &&
         transition.soundThreshold == empty.soundThreshold &&
         transition.brightnessBefore == empty.brightnessBefore &&
         transition.brightnessAfter == empty.brightnessAfter;
}

bool validPreTouchSample(const TelemetryRecord& sample) {
  return sample.kind == TelemetryKind::kSample &&
         sample.seq <= kMaxSigned64 && sample.uptimeMs <= kMaxSigned64 &&
         sample.appliedConfigRevision <= kMaxSigned64 &&
         validMetric(sample.sample.micRms) &&
         validMetric(sample.sample.micEnvelope) &&
         validMetric(sample.sample.noiseFloor) &&
         validMetric(sample.sample.soundThreshold) &&
         sample.sample.micMin <= sample.sample.micMax &&
         validState(sample.sample.state) &&
         canonicalUnusedTransition(sample.transition);
}

void incrementSaturated(uint32_t& value) {
  if (value != std::numeric_limits<uint32_t>::max()) {
    ++value;
  }
}

}  // namespace

TouchFeedbackQueue::CompactSlot TouchFeedbackQueue::compact(
    const TouchFeedbackEvidence& evidence) {
  CompactSlot slot = {};
  slot.seq = evidence.feedback.seq;
  slot.uptimeMs = evidence.feedback.linkedSampleUptimeMs;
  slot.appliedConfigRevision = evidence.preTouchSample.appliedConfigRevision;
  slot.sample = evidence.preTouchSample.sample;
  std::memcpy(slot.bootId, evidence.feedback.bootId, sizeof(slot.bootId));
  slot.actualPresence = evidence.feedback.actualPresence;
  return slot;
}

TouchFeedbackEvidence TouchFeedbackQueue::expand(const CompactSlot& slot) {
  static constexpr char kLowerHex[] = "0123456789abcdef";
  TouchFeedbackEvidence evidence = {};
  evidence.preTouchSample.kind = TelemetryKind::kSample;
  evidence.preTouchSample.seq = slot.seq;
  evidence.preTouchSample.uptimeMs = slot.uptimeMs;
  evidence.preTouchSample.appliedConfigRevision =
      slot.appliedConfigRevision;
  evidence.preTouchSample.sample = slot.sample;
  evidence.feedback.feedbackId[0] = 'f';
  evidence.feedback.feedbackId[1] = ':';
  std::memcpy(evidence.feedback.feedbackId + 2, slot.bootId, 32);
  evidence.feedback.feedbackId[34] = ':';
  for (size_t index = 0; index < 16; ++index) {
    const unsigned shift = static_cast<unsigned>((15 - index) * 4);
    evidence.feedback.feedbackId[35 + index] =
        kLowerHex[(slot.seq >> shift) & UINT64_C(0x0f)];
  }
  evidence.feedback.feedbackId[51] = '\0';
  std::memcpy(evidence.feedback.bootId, slot.bootId,
              sizeof(evidence.feedback.bootId));
  evidence.feedback.seq = slot.seq;
  evidence.feedback.linkedSampleUptimeMs = slot.uptimeMs;
  evidence.feedback.actualPresence = slot.actualPresence;
  evidence.feedback.observedState = slot.sample.state;
  return evidence;
}

bool TouchFeedbackQueue::slotMatchesEvidence(
    const CompactSlot& slot, const TouchFeedbackEvidence& evidence) {
  const SampleTelemetry& expected = evidence.preTouchSample.sample;
  return evidence.preTouchSample.kind == TelemetryKind::kSample &&
         slot.seq == evidence.preTouchSample.seq &&
         slot.uptimeMs == evidence.preTouchSample.uptimeMs &&
         slot.appliedConfigRevision ==
             evidence.preTouchSample.appliedConfigRevision &&
         slot.sample.pir == expected.pir &&
         slot.sample.micRms == expected.micRms &&
         slot.sample.micEnvelope == expected.micEnvelope &&
         slot.sample.micMin == expected.micMin &&
         slot.sample.micMax == expected.micMax &&
         slot.sample.noiseFloor == expected.noiseFloor &&
         slot.sample.soundThreshold == expected.soundThreshold &&
         slot.sample.soundActive == expected.soundActive &&
         slot.sample.state == expected.state &&
         slot.sample.brightness == expected.brightness &&
         std::memcmp(slot.bootId, evidence.feedback.bootId,
                     sizeof(slot.bootId)) == 0 &&
         slot.seq == evidence.feedback.seq &&
         slot.uptimeMs == evidence.feedback.linkedSampleUptimeMs &&
         slot.actualPresence == evidence.feedback.actualPresence &&
         slot.sample.state == evidence.feedback.observedState;
}

bool buildTouchFeedbackEvidence(const char* bootId,
                                const TelemetryRecord& preTouchSample,
                                TouchPresenceChoice choice,
                                TouchFeedbackEvidence& output) {
  if (!validBootId(bootId) || !validPreTouchSample(preTouchSample)) {
    return false;
  }

  QueuedSampleReference reference = {};
  std::memcpy(reference.bootId, bootId, sizeof(reference.bootId));
  reference.seq = preTouchSample.seq;
  reference.uptimeMs = preTouchSample.uptimeMs;
  reference.preTouchObservedState = preTouchSample.sample.state;

  TouchFeedbackEvidence candidate = {};
  candidate.preTouchSample = preTouchSample;
  if (!buildTouchFeedbackRecord(reference, choice, candidate.feedback) ||
      !touchFeedbackEvidenceIsValid(candidate)) {
    return false;
  }
  output = candidate;
  return true;
}

bool touchFeedbackEvidenceIsValid(const TouchFeedbackEvidence& evidence) {
  return validPreTouchSample(evidence.preTouchSample) &&
         feedbackRecordIsValid(evidence.feedback) &&
         evidence.feedback.seq == evidence.preTouchSample.seq &&
         evidence.feedback.linkedSampleUptimeMs ==
             evidence.preTouchSample.uptimeMs &&
         evidence.feedback.observedState ==
             evidence.preTouchSample.sample.state;
}

void TouchFeedbackQueue::lock() const {
#ifdef ARDUINO_ARCH_ESP32
  portENTER_CRITICAL(&mutex_);
#endif
}

void TouchFeedbackQueue::unlock() const {
#ifdef ARDUINO_ARCH_ESP32
  portEXIT_CRITICAL(&mutex_);
#endif
}

TouchFeedbackQueuePushResult TouchFeedbackQueue::push(
    const TouchFeedbackEvidence& evidence) {
  if (!touchFeedbackEvidenceIsValid(evidence)) {
    lock();
    incrementSaturated(rejectedInvalid_);
    unlock();
    return TouchFeedbackQueuePushResult::kInvalid;
  }

  lock();
  if (size_ == kCapacity) {
    incrementSaturated(droppedFull_);
    unlock();
    return TouchFeedbackQueuePushResult::kFull;
  }
  records_[(head_ + size_) % kCapacity] = compact(evidence);
  ++size_;
  unlock();
  return TouchFeedbackQueuePushResult::kStored;
}

size_t TouchFeedbackQueue::copyPrefix(TouchFeedbackEvidence* output,
                                      size_t outputCapacity) const {
  if (output == nullptr || outputCapacity == 0) {
    return 0;
  }

  lock();
  const size_t copyCount = size_ < outputCapacity ? size_ : outputCapacity;
  for (size_t index = 0; index < copyCount; ++index) {
    output[index] = expand(records_[(head_ + index) % kCapacity]);
  }
  unlock();
  return copyCount;
}

bool TouchFeedbackQueue::commitPrefix(const TouchFeedbackEvidence* expected,
                                      size_t count) {
  if (expected == nullptr || count == 0 || count > kCapacity) {
    return false;
  }
  for (size_t index = 0; index < count; ++index) {
    if (!touchFeedbackEvidenceIsValid(expected[index])) {
      return false;
    }
  }

  lock();
  if (count > size_) {
    unlock();
    return false;
  }
  for (size_t index = 0; index < count; ++index) {
    if (!slotMatchesEvidence(records_[(head_ + index) % kCapacity],
                             expected[index])) {
      unlock();
      return false;
    }
  }

  head_ = (head_ + count) % kCapacity;
  size_ -= count;
  unlock();
  return true;
}

size_t TouchFeedbackQueue::size() const {
  lock();
  const size_t result = size_;
  unlock();
  return result;
}

uint32_t TouchFeedbackQueue::droppedFull() const {
  lock();
  const uint32_t result = droppedFull_;
  unlock();
  return result;
}

uint32_t TouchFeedbackQueue::rejectedInvalid() const {
  lock();
  const uint32_t result = rejectedInvalid_;
  unlock();
  return result;
}
