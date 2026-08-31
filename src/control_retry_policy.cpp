#include "control_retry_policy.h"

#include <algorithm>
#include <limits>

namespace {

constexpr uint64_t kSuccessIntervalMs = 5000;
constexpr uint64_t kMaximumDelayMs = 5ULL * 60ULL * 1000ULL;
constexpr uint64_t kMaximumBaseMs = 4ULL * 60ULL * 1000ULL;

uint64_t saturatingAdd(uint64_t left, uint64_t right) {
  return right > std::numeric_limits<uint64_t>::max() - left
             ? std::numeric_limits<uint64_t>::max()
             : left + right;
}
}  // namespace

ControlRetrySchedule scheduleControlPoll(uint64_t nowMs, bool success,
                                         uint8_t failureExponent,
                                         uint32_t entropy) {
  if (success) {
    return {saturatingAdd(nowMs, kSuccessIntervalMs), 0};
  }
  const uint8_t boundedExponent = std::min<uint8_t>(failureExponent, 8);
  const uint64_t shifted = kSuccessIntervalMs << boundedExponent;
  const uint64_t base = std::min<uint64_t>(shifted, kMaximumBaseMs);
  const uint64_t jitterRange = base / 4U + 1U;
  const uint64_t delay =
      std::min<uint64_t>(base + entropy % jitterRange, kMaximumDelayMs);
  return {saturatingAdd(nowMs, delay),
          static_cast<uint8_t>(std::min<unsigned>(failureExponent + 1U, 8U))};
}
