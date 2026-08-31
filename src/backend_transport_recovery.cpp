#include "backend_transport_recovery.h"

#include <limits>

BackendTransportRecoveryDecision
BackendTransportRecoveryPolicy::recordFailure(uint64_t nowMs, int status) {
  if (consecutiveFailureCount_ != std::numeric_limits<uint8_t>::max()) {
    ++consecutiveFailureCount_;
  }

  recoveryPending_ =
      recoveryPending_ || status == kBackendTransportImmediateRecycleStatus ||
      consecutiveFailureCount_ >= kBackendTransportFailureThreshold;

  BackendTransportRecoveryDecision decision = {};
  decision.failureCount = consecutiveFailureCount_;
  decision.cooldownRemainingMs = cooldownRemainingMs(nowMs);
  decision.blockedByCooldown =
      recoveryPending_ && decision.cooldownRemainingMs != 0;
  if (!recoveryPending_ || decision.blockedByCooldown) {
    return decision;
  }

  decision.shouldRecycle = true;
  lastRecycleMs_ = nowMs;
  hasRecycled_ = true;
  consecutiveFailureCount_ = 0;
  recoveryPending_ = false;
  return decision;
}

void BackendTransportRecoveryPolicy::recordSuccess() {
  consecutiveFailureCount_ = 0;
  recoveryPending_ = false;
}

uint8_t BackendTransportRecoveryPolicy::consecutiveFailureCount() const {
  return consecutiveFailureCount_;
}

bool BackendTransportRecoveryPolicy::recoveryPending() const {
  return recoveryPending_;
}

uint64_t BackendTransportRecoveryPolicy::cooldownRemainingMs(
    uint64_t nowMs) const {
  if (!hasRecycled_) {
    return 0;
  }
  // A monotonic clock must not move backwards, but retaining the full cooldown
  // is safer than allowing an unexpected timestamp regression to bypass it.
  if (nowMs < lastRecycleMs_) {
    return kBackendTransportRecycleCooldownMs;
  }
  const uint64_t elapsedMs = nowMs - lastRecycleMs_;
  return elapsedMs >= kBackendTransportRecycleCooldownMs
             ? 0
             : kBackendTransportRecycleCooldownMs - elapsedMs;
}
