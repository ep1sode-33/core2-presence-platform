#include "telemetry.h"

QueuePushResult TelemetryQueue::push(const TelemetryRecord& record) {
  const bool isSample = record.kind == TelemetryKind::kSample;
  const size_t sampleLimit = kCapacity - kReservedTransitionSlots;
  if (isSample && size_ >= sampleLimit) {
    ++droppedSamples_;
    return QueuePushResult::kSampleDropped;
  }
  if (size_ >= kCapacity) {
    ++droppedCritical_;
    return QueuePushResult::kCriticalDropped;
  }

  records_[(head_ + size_) % kCapacity] = record;
  ++size_;
  return QueuePushResult::kStored;
}

const TelemetryRecord* TelemetryQueue::front() const {
  return size_ == 0 ? nullptr : &records_[head_];
}

bool TelemetryQueue::pop() {
  if (size_ == 0) {
    return false;
  }
  head_ = (head_ + 1) % kCapacity;
  --size_;
  return true;
}
