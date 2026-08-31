#pragma once

#include <cstdint>

inline constexpr uint8_t kBackendTransportFailureThreshold = 3;
inline constexpr int kBackendTransportImmediateRecycleStatus = -3;
inline constexpr uint64_t kBackendTransportRecycleCooldownMs = 60ULL * 1000ULL;

struct BackendTransportRecoveryDecision {
  bool shouldRecycle = false;
  bool blockedByCooldown = false;
  uint8_t failureCount = 0;
  uint64_t cooldownRemainingMs = 0;
};

// Tracks path-level recovery from bounded-operation transport failures. Any
// proven backend response clears the current failure streak, including one on
// the independently serialized telemetry session. Recycles are rate-limited so
// a real backend outage cannot make the device continuously reset its Wi-Fi
// connection.
class BackendTransportRecoveryPolicy {
 public:
  BackendTransportRecoveryDecision recordFailure(uint64_t nowMs,
                                                  int status);
  void recordSuccess();

  uint8_t consecutiveFailureCount() const;
  bool recoveryPending() const;
  uint64_t cooldownRemainingMs(uint64_t nowMs) const;

 private:
  uint8_t consecutiveFailureCount_ = 0;
  uint64_t lastRecycleMs_ = 0;
  bool hasRecycled_ = false;
  bool recoveryPending_ = false;
};
