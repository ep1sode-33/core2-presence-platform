#include <cassert>

#include "backlog_policy.h"

int main() {
  assert(adaptiveTelemetryIntervalMs(1000, 0) == 1000);
  assert(adaptiveTelemetryIntervalMs(1000, 64) == 5000);
  assert(adaptiveTelemetryIntervalMs(1000, 128) == 15000);
  assert(adaptiveTelemetryIntervalMs(1000, 192) == 60000);
  assert(adaptiveTelemetryIntervalMs(60000, 64) == 60000);

  assert(mayFreezeTelemetry(0, kCriticalFilesystemReserveBytes, false));
  assert(!mayFreezeTelemetry(kTelemetrySpoolSoftFileLimit,
                             kCriticalFilesystemReserveBytes, false));
  assert(mayFreezeTelemetry(kTelemetrySpoolSoftFileLimit,
                            kCriticalFilesystemReserveBytes, true));
  assert(mayFreezeTelemetry(0, kCriticalFilesystemReserveBytes - 1, true));
  assert(!mayFreezeTelemetry(kTelemetrySpoolHardFileLimit,
                             kCriticalFilesystemReserveBytes * 2, true));
  assert(shouldDrainCriticalReserve(kTelemetrySpoolSoftFileLimit,
                                    kCriticalFilesystemReserveBytes, true));
  assert(!shouldDrainCriticalReserve(0, kCriticalFilesystemReserveBytes,
                                     true));
  return 0;
}
