#include <cassert>
#include <cstdint>
#include <limits>

#include "telemetry_write_deadline.h"

int main() {
  static_assert(kTelemetryWriteNoProgressTimeoutMs == 1500);
  static_assert(kTelemetryWriteAbsoluteTimeoutMs == 5000);

  TelemetryWriteDeadlineTracker tracker;
  tracker.begin(100);
  assert(tracker.expiry(1599) == TelemetryWriteDeadlineExpiry::kNone);
  assert(tracker.expiry(1600) ==
         TelemetryWriteDeadlineExpiry::kNoProgress);

  tracker.begin(100);
  tracker.noteProgress(1500);
  assert(tracker.expiry(2999) == TelemetryWriteDeadlineExpiry::kNone);
  assert(tracker.expiry(3000) ==
         TelemetryWriteDeadlineExpiry::kNoProgress);
  tracker.noteProgress(5000);
  assert(tracker.expiry(5100) ==
         TelemetryWriteDeadlineExpiry::kAbsolute);

  const uint32_t nearWrap = std::numeric_limits<uint32_t>::max() - 99;
  tracker.begin(nearWrap);
  tracker.noteProgress(nearWrap + 1000U);
  assert(tracker.expiry(nearWrap + 1499U) ==
         TelemetryWriteDeadlineExpiry::kNone);
  assert(tracker.expiry(nearWrap + 5000U) ==
         TelemetryWriteDeadlineExpiry::kAbsolute);
}
