#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>

#include "uploader_watchdog.h"

int main() {
  assert(uploaderWatchdogPolicyIsValid(kUploaderWatchdogPolicy));
  assert(kUploaderWatchdogPolicy.timeoutSeconds == 30);
  assert(uploaderWatchdogMinimumTimeoutMs(kUploaderWatchdogPolicy) == 23000);
  assert(static_cast<uint64_t>(kUploaderWatchdogPolicy.timeoutSeconds) * 1000U >
         uploaderWatchdogMinimumTimeoutMs(kUploaderWatchdogPolicy));

  UploaderWatchdogPolicy tooShort = kUploaderWatchdogPolicy;
  tooShort.timeoutSeconds = 23;
  assert(!uploaderWatchdogPolicyIsValid(tooShort));
  tooShort.timeoutSeconds = 24;
  assert(uploaderWatchdogPolicyIsValid(tooShort));

  UploaderWatchdogPolicy noMargin = kUploaderWatchdogPolicy;
  noMargin.schedulingMarginMs = 0;
  assert(!uploaderWatchdogPolicyIsValid(noMargin));

  UploaderWatchdogPolicy overflow = {};
  overflow.timeoutSeconds = 60;
  overflow.maximumConnectTimeoutMs = std::numeric_limits<uint32_t>::max();
  overflow.maximumIoTimeoutMs = std::numeric_limits<uint32_t>::max();
  overflow.schedulingMarginMs = 1;
  assert(uploaderWatchdogMinimumTimeoutMs(overflow) ==
         std::numeric_limits<uint32_t>::max());
  assert(!uploaderWatchdogPolicyIsValid(overflow));

  assert(startUploaderTaskWatchdog() ==
         UploaderWatchdogStartResult::kUnsupportedPlatform);
  assert(!feedUploaderTaskWatchdog());
  assert(std::strcmp(uploaderWatchdogStartResultName(
                         UploaderWatchdogStartResult::kStarted),
                     "started") == 0);
}
