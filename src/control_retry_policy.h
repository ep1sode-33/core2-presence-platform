#pragma once

#include <cstdint>

struct ControlRetrySchedule {
  uint64_t nextAttemptMs = 0;
  uint8_t nextFailureExponent = 0;
};

// Successful polls honor the exact five-second server contract. Failures use
// 5s exponential backoff plus bounded positive jitter, with the entire delay
// capped at five minutes.
ControlRetrySchedule scheduleControlPoll(uint64_t nowMs, bool success,
                                         uint8_t failureExponent,
                                         uint32_t entropy);
