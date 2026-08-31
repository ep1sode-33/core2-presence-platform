#include "operational_log.h"

#include <cstring>
#include <limits>

namespace {

constexpr uint64_t kMaxSigned64 = 0x7fffffffffffffffULL;

bool validLevel(OperationalLogLevel level) {
  switch (level) {
    case OperationalLogLevel::kError:
    case OperationalLogLevel::kWarning:
    case OperationalLogLevel::kInfo:
    case OperationalLogLevel::kDebug:
    case OperationalLogLevel::kSensorDetail:
      return true;
  }
  return false;
}

bool validCode(OperationalLogCode code) {
  switch (code) {
    case OperationalLogCode::kBoot:
    case OperationalLogCode::kHealthChanged:
    case OperationalLogCode::kPresenceTransition:
    case OperationalLogCode::kWifiChanged:
    case OperationalLogCode::kBackendRequest:
    case OperationalLogCode::kStorageChanged:
    case OperationalLogCode::kSensorChanged:
    case OperationalLogCode::kRecoveryAction:
    case OperationalLogCode::kOtaChanged:
    case OperationalLogCode::kDebugSessionChanged:
    case OperationalLogCode::kCommandChanged:
      return true;
  }
  return false;
}

void incrementSaturated(uint32_t& value) {
  if (value != std::numeric_limits<uint32_t>::max()) {
    ++value;
  }
}

bool sameEvent(const OperationalLogEvent& left,
               const OperationalLogEvent& right) {
  return left.sequence == right.sequence && left.uptimeMs == right.uptimeMs &&
         left.level == right.level && left.code == right.code &&
         left.value0 == right.value0 && left.value1 == right.value1;
}

}  // namespace

bool operationalLogEventIsValid(const OperationalLogEvent& event) {
  return event.sequence <= kMaxSigned64 && event.uptimeMs <= kMaxSigned64 &&
         validLevel(event.level) && validCode(event.code);
}

bool operationalLogEventIsCritical(const OperationalLogEvent& event) {
  return event.level == OperationalLogLevel::kError ||
         event.level == OperationalLogLevel::kWarning;
}

void OperationalLogRing::lock() const {
#ifdef ARDUINO_ARCH_ESP32
  portENTER_CRITICAL(&mutex_);
#endif
}

void OperationalLogRing::unlock() const {
#ifdef ARDUINO_ARCH_ESP32
  portEXIT_CRITICAL(&mutex_);
#endif
}

OperationalLogPushResult OperationalLogRing::push(
    const OperationalLogEvent& event) {
  if (!operationalLogEventIsValid(event)) {
    lock();
    incrementSaturated(rejectedInvalid_);
    unlock();
    return OperationalLogPushResult::kInvalid;
  }

  const bool critical = operationalLogEventIsCritical(event);
  lock();
  const size_t ordinaryLimit = kCapacity - kReservedCriticalSlots;
  if ((!critical && size_ >= ordinaryLimit) || size_ == kCapacity) {
    if (critical) {
      incrementSaturated(droppedCritical_);
      unlock();
      return OperationalLogPushResult::kDroppedCritical;
    }
    incrementSaturated(droppedVerbose_);
    unlock();
    return OperationalLogPushResult::kDroppedVerbose;
  }

  events_[(head_ + size_) % kCapacity] = event;
  ++size_;
  unlock();
  return OperationalLogPushResult::kStored;
}

size_t OperationalLogRing::copyPrefix(OperationalLogEvent* output,
                                      size_t outputCapacity) const {
  if (output == nullptr || outputCapacity == 0) {
    return 0;
  }
  lock();
  const size_t count = size_ < outputCapacity ? size_ : outputCapacity;
  for (size_t index = 0; index < count; ++index) {
    output[index] = events_[(head_ + index) % kCapacity];
  }
  unlock();
  return count;
}

bool OperationalLogRing::commitPrefix(const OperationalLogEvent* expected,
                                      size_t count) {
  if (expected == nullptr || count == 0 || count > kCapacity) {
    return false;
  }
  for (size_t index = 0; index < count; ++index) {
    if (!operationalLogEventIsValid(expected[index])) {
      return false;
    }
  }

  lock();
  if (count > size_) {
    unlock();
    return false;
  }
  for (size_t index = 0; index < count; ++index) {
    if (!sameEvent(events_[(head_ + index) % kCapacity], expected[index])) {
      unlock();
      return false;
    }
  }
  head_ = (head_ + count) % kCapacity;
  size_ -= count;
  unlock();
  return true;
}

size_t OperationalLogRing::size() const {
  lock();
  const size_t result = size_;
  unlock();
  return result;
}

uint32_t OperationalLogRing::droppedVerbose() const {
  lock();
  const uint32_t result = droppedVerbose_;
  unlock();
  return result;
}

uint32_t OperationalLogRing::droppedCritical() const {
  lock();
  const uint32_t result = droppedCritical_;
  unlock();
  return result;
}

uint32_t OperationalLogRing::rejectedInvalid() const {
  lock();
  const uint32_t result = rejectedInvalid_;
  unlock();
  return result;
}

bool RemoteLogSession::beginDetailed(uint64_t nowMs,
                                     uint64_t requestedDurationMs) {
  if (nowMs > kMaxSigned64 || requestedDurationMs == 0) {
    return false;
  }
  const uint64_t duration = requestedDurationMs < kMaximumDurationMs
                                ? requestedDurationMs
                                : kMaximumDurationMs;
  if (duration > kMaxSigned64 - nowMs) {
    return false;
  }
  detailed_ = true;
  expiresAtMs_ = nowMs + duration;
  return true;
}

void RemoteLogSession::stop() {
  detailed_ = false;
  expiresAtMs_ = 0;
}

RemoteLogMode RemoteLogSession::mode(uint64_t nowMs) {
  if (detailed_ && nowMs >= expiresAtMs_) {
    stop();
  }
  return detailed_ ? RemoteLogMode::kDetailed
                   : RemoteLogMode::kOperational;
}

uint64_t RemoteLogSession::remainingMs(uint64_t nowMs) {
  return mode(nowMs) == RemoteLogMode::kDetailed ? expiresAtMs_ - nowMs : 0;
}

bool RemoteLogSession::accepts(OperationalLogLevel level, uint64_t nowMs) {
  if (!validLevel(level)) {
    return false;
  }
  if (level == OperationalLogLevel::kSensorDetail) {
    return mode(nowMs) == RemoteLogMode::kDetailed;
  }
  return true;
}
