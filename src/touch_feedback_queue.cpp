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

bool validPreTouchSample(const TelemetryRecord& sample) {
  return sample.kind == TelemetryKind::kSample &&
         sample.seq <= kMaxSigned64 && sample.uptimeMs <= kMaxSigned64 &&
         sample.appliedConfigRevision <= kMaxSigned64 &&
         validMetric(sample.sample.micRms) &&
         validMetric(sample.sample.micEnvelope) &&
         validMetric(sample.sample.noiseFloor) &&
         validMetric(sample.sample.soundThreshold) &&
         sample.sample.micMin <= sample.sample.micMax &&
         validState(sample.sample.state);
}

bool sameTelemetryRecord(const TelemetryRecord& left,
                         const TelemetryRecord& right) {
  return left.kind == right.kind && left.seq == right.seq &&
         left.uptimeMs == right.uptimeMs &&
         left.appliedConfigRevision == right.appliedConfigRevision &&
         left.sample.pir == right.sample.pir &&
         left.sample.micRms == right.sample.micRms &&
         left.sample.micEnvelope == right.sample.micEnvelope &&
         left.sample.micMin == right.sample.micMin &&
         left.sample.micMax == right.sample.micMax &&
         left.sample.noiseFloor == right.sample.noiseFloor &&
         left.sample.soundThreshold == right.sample.soundThreshold &&
         left.sample.soundActive == right.sample.soundActive &&
         left.sample.state == right.sample.state &&
         left.sample.brightness == right.sample.brightness &&
         left.transition.hasFromState == right.transition.hasFromState &&
         left.transition.fromState == right.transition.fromState &&
         left.transition.toState == right.transition.toState &&
         left.transition.reason == right.transition.reason;
}

bool sameFeedbackRecord(const FeedbackRecord& left,
                        const FeedbackRecord& right) {
  return std::memcmp(left.feedbackId, right.feedbackId,
                     sizeof(left.feedbackId)) == 0 &&
         std::memcmp(left.bootId, right.bootId, sizeof(left.bootId)) == 0 &&
         left.seq == right.seq &&
         left.linkedSampleUptimeMs == right.linkedSampleUptimeMs &&
         left.actualPresence == right.actualPresence &&
         left.observedState == right.observedState;
}

bool sameEvidence(const TouchFeedbackEvidence& left,
                  const TouchFeedbackEvidence& right) {
  return sameTelemetryRecord(left.preTouchSample, right.preTouchSample) &&
         sameFeedbackRecord(left.feedback, right.feedback);
}

void incrementSaturated(uint32_t& value) {
  if (value != std::numeric_limits<uint32_t>::max()) {
    ++value;
  }
}

}  // namespace

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
  records_[(head_ + size_) % kCapacity] = evidence;
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
    output[index] = records_[(head_ + index) % kCapacity];
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
    if (!sameEvidence(records_[(head_ + index) % kCapacity],
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
