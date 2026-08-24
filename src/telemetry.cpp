#include "telemetry.h"

void TelemetryQueue::lock() const {
#ifdef ARDUINO_ARCH_ESP32
  portENTER_CRITICAL(&mutex_);
#endif
}

void TelemetryQueue::unlock() const {
#ifdef ARDUINO_ARCH_ESP32
  portEXIT_CRITICAL(&mutex_);
#endif
}

QueuePushResult TelemetryQueue::push(const TelemetryRecord& record) {
  lock();
  const bool isSample = record.kind == TelemetryKind::kSample;
  const size_t sampleLimit = kCapacity - kReservedTransitionSlots;
  if (isSample && size_ >= sampleLimit) {
    ++droppedSamples_;
    unlock();
    return QueuePushResult::kSampleDropped;
  }
  if (size_ >= kCapacity) {
    ++droppedCritical_;
    unlock();
    return QueuePushResult::kCriticalDropped;
  }

  records_[(head_ + size_) % kCapacity] = record;
  ++size_;
  unlock();
  return QueuePushResult::kStored;
}

bool TelemetryQueue::peek(TelemetryRecord& output) const {
  lock();
  if (size_ == 0) {
    unlock();
    return false;
  }
  output = records_[head_];
  unlock();
  return true;
}

size_t TelemetryQueue::copyPrefix(TelemetryRecord* output,
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

bool TelemetryQueue::commitPrefix(const TelemetryRecord* expected,
                                  size_t count) {
  if (expected == nullptr || count == 0) {
    return false;
  }

  lock();
  if (count > size_) {
    unlock();
    return false;
  }
  for (size_t index = 0; index < count; ++index) {
    const TelemetryRecord& queued = records_[(head_ + index) % kCapacity];
    if (queued.kind != expected[index].kind ||
        queued.seq != expected[index].seq ||
        queued.uptimeMs != expected[index].uptimeMs) {
      unlock();
      return false;
    }
  }

  head_ = (head_ + count) % kCapacity;
  size_ -= count;
  unlock();
  return true;
}

size_t TelemetryQueue::size() const {
  lock();
  const size_t result = size_;
  unlock();
  return result;
}

uint32_t TelemetryQueue::droppedSamples() const {
  lock();
  const uint32_t result = droppedSamples_;
  unlock();
  return result;
}

uint32_t TelemetryQueue::droppedCritical() const {
  lock();
  const uint32_t result = droppedCritical_;
  unlock();
  return result;
}
