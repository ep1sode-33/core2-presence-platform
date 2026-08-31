#include <cassert>
#include <cstdint>
#include <limits>

#include "control_retry_policy.h"

int main() {
  const ControlRetrySchedule success = scheduleControlPoll(1000, true, 7, 99);
  assert(success.nextAttemptMs == 6000);
  assert(success.nextFailureExponent == 0);

  const ControlRetrySchedule first = scheduleControlPoll(1000, false, 0, 0);
  assert(first.nextAttemptMs == 6000);
  assert(first.nextFailureExponent == 1);

  const ControlRetrySchedule jittered =
      scheduleControlPoll(1000, false, 1, UINT32_MAX);
  assert(jittered.nextAttemptMs >= 11000);
  assert(jittered.nextAttemptMs <= 13500);

  const ControlRetrySchedule capped =
      scheduleControlPoll(1000, false, 8, UINT32_MAX);
  assert(capped.nextAttemptMs <= 301000);
  assert(capped.nextFailureExponent == 8);

  const ControlRetrySchedule saturated = scheduleControlPoll(
      std::numeric_limits<uint64_t>::max() - 2, true, 0, 0);
  assert(saturated.nextAttemptMs == std::numeric_limits<uint64_t>::max());
}
