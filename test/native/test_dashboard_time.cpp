#include <cassert>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "dashboard_time.h"

int main() {
  assert(setenv("TZ", kDashboardEasternTimeZone, 1) == 0);
  tzset();

  char output[kDashboardDateTimeCapacity] = {};
  assert(!formatDashboardEasternDateTime(0, output, sizeof(output)));
  assert(output[0] == '\0');
  assert(!formatDashboardEasternDateTime(1768499100, nullptr, 0));

  assert(formatDashboardEasternDateTime(1768499100, output,
                                        sizeof(output)));
  assert(std::strcmp(output, "2026-01-15 12:45 EST") == 0);

  assert(formatDashboardEasternDateTime(1784131500, output,
                                        sizeof(output)));
  assert(std::strcmp(output, "2026-07-15 12:05 EDT") == 0);

  // The repeated autumn hour must change both its clock value and suffix.
  assert(formatDashboardEasternDateTime(1793512740, output,
                                        sizeof(output)));
  assert(std::strcmp(output, "2026-11-01 01:59 EDT") == 0);
  assert(formatDashboardEasternDateTime(1793512800, output,
                                        sizeof(output)));
  assert(std::strcmp(output, "2026-11-01 01:00 EST") == 0);

  char tooSmall[20] = {};
  assert(!formatDashboardEasternDateTime(1784131500, tooSmall,
                                         sizeof(tooSmall)));
  assert(tooSmall[0] == '\0');
  return 0;
}
