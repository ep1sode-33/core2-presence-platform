#include <cassert>
#include <cstdint>
#include <limits>

#include "backend_transport_recovery.h"

namespace {

void testThresholdAndSuccessReset() {
  BackendTransportRecoveryPolicy policy;

  BackendTransportRecoveryDecision decision = policy.recordFailure(1000, -1);
  assert(!decision.shouldRecycle);
  assert(!decision.blockedByCooldown);
  assert(decision.failureCount == 1);

  decision = policy.recordFailure(2000, -5);
  assert(!decision.shouldRecycle);
  assert(decision.failureCount == 2);

  policy.recordSuccess();
  assert(policy.consecutiveFailureCount() == 0);
  assert(!policy.recoveryPending());

  decision = policy.recordFailure(3000, -1);
  assert(!decision.shouldRecycle);
  assert(decision.failureCount == 1);
  decision = policy.recordFailure(4000, -1);
  assert(!decision.shouldRecycle);
  decision = policy.recordFailure(5000, -1);
  assert(decision.shouldRecycle);
  assert(!decision.blockedByCooldown);
  assert(decision.failureCount == kBackendTransportFailureThreshold);
  assert(policy.consecutiveFailureCount() == 0);
  assert(policy.cooldownRemainingMs(5000) ==
         kBackendTransportRecycleCooldownMs);
}

void testImmediateStatusAndCooldown() {
  BackendTransportRecoveryPolicy policy;

  BackendTransportRecoveryDecision decision = policy.recordFailure(
      100, kBackendTransportImmediateRecycleStatus);
  assert(decision.shouldRecycle);
  assert(decision.failureCount == 1);

  decision = policy.recordFailure(
      200, kBackendTransportImmediateRecycleStatus);
  assert(!decision.shouldRecycle);
  assert(decision.blockedByCooldown);
  assert(decision.failureCount == 1);
  assert(decision.cooldownRemainingMs ==
         kBackendTransportRecycleCooldownMs - 100);
  assert(policy.recoveryPending());

  decision = policy.recordFailure(1000, -1);
  assert(!decision.shouldRecycle);
  assert(decision.blockedByCooldown);
  assert(decision.failureCount == 2);

  decision = policy.recordFailure(
      100 + kBackendTransportRecycleCooldownMs - 1, -1);
  assert(!decision.shouldRecycle);
  assert(decision.blockedByCooldown);
  assert(decision.cooldownRemainingMs == 1);

  decision = policy.recordFailure(
      100 + kBackendTransportRecycleCooldownMs, -1);
  assert(decision.shouldRecycle);
  assert(!decision.blockedByCooldown);
  assert(decision.cooldownRemainingMs == 0);
}

void testSuccessClearsPendingButNotCooldown() {
  BackendTransportRecoveryPolicy policy;
  assert(policy.recordFailure(10, kBackendTransportImmediateRecycleStatus)
             .shouldRecycle);
  assert(policy.recordFailure(20, kBackendTransportImmediateRecycleStatus)
             .blockedByCooldown);

  policy.recordSuccess();
  assert(!policy.recoveryPending());
  assert(policy.consecutiveFailureCount() == 0);
  assert(policy.cooldownRemainingMs(20) ==
         kBackendTransportRecycleCooldownMs - 10);

  const BackendTransportRecoveryDecision decision =
      policy.recordFailure(30, -1);
  assert(!decision.shouldRecycle);
  assert(!decision.blockedByCooldown);
  assert(decision.failureCount == 1);
}

void testFailureCountSaturates() {
  BackendTransportRecoveryPolicy policy;
  assert(policy.recordFailure(0, kBackendTransportImmediateRecycleStatus)
             .shouldRecycle);
  BackendTransportRecoveryDecision decision = {};
  for (unsigned index = 0;
       index < static_cast<unsigned>(std::numeric_limits<uint8_t>::max()) +
                   20U;
       ++index) {
    decision = policy.recordFailure(1, -1);
  }
  assert(!decision.shouldRecycle);
  assert(decision.blockedByCooldown);
  assert(decision.failureCount == std::numeric_limits<uint8_t>::max());
}

void testTimestampRegressionRetainsCooldown() {
  BackendTransportRecoveryPolicy policy;
  assert(policy.recordFailure(1000, kBackendTransportImmediateRecycleStatus)
             .shouldRecycle);
  assert(policy.cooldownRemainingMs(999) ==
         kBackendTransportRecycleCooldownMs);
}

}  // namespace

int main() {
  static_assert(kBackendTransportFailureThreshold == 3);
  static_assert(kBackendTransportImmediateRecycleStatus == -3);
  static_assert(kBackendTransportRecycleCooldownMs == 60000);

  testThresholdAndSuccessReset();
  testImmediateStatusAndCooldown();
  testSuccessClearsPendingButNotCooldown();
  testFailureCountSaturates();
  testTimestampRegressionRetainsCooldown();
}
