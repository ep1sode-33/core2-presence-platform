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
  assert(std::strcmp(output, "2026-01-15 12:45:00 EST") == 0);

  assert(formatDashboardEasternDateTime(1784131500, output,
                                        sizeof(output)));
  assert(std::strcmp(output, "2026-07-15 12:05:00 EDT") == 0);

  // The repeated autumn hour must change both its clock value and suffix.
  assert(formatDashboardEasternDateTime(1793512740, output,
                                        sizeof(output)));
  assert(std::strcmp(output, "2026-11-01 01:59:00 EDT") == 0);
  assert(formatDashboardEasternDateTime(1793512800, output,
                                        sizeof(output)));
  assert(std::strcmp(output, "2026-11-01 01:00:00 EST") == 0);

  char tooSmall[20] = {};
  assert(!formatDashboardEasternDateTime(1784131500, tooSmall,
                                         sizeof(tooSmall)));
  assert(tooSmall[0] == '\0');

  char freshness[32] = {};
  assert(formatDashboardFeedFreshness(false, 1000, 0, freshness,
                                      sizeof(freshness)));
  assert(std::strcmp(freshness, "waiting") == 0);
  assert(formatDashboardFeedFreshness(true, 1000, 1000, freshness,
                                      sizeof(freshness)));
  assert(std::strcmp(freshness, "0s ago") == 0);
  assert(formatDashboardFeedFreshness(true, 60000, 1000, freshness,
                                      sizeof(freshness)));
  assert(std::strcmp(freshness, "59s ago") == 0);
  assert(formatDashboardFeedFreshness(true, 61000, 1000, freshness,
                                      sizeof(freshness)));
  assert(std::strcmp(freshness, "1m ago") == 0);
  assert(formatDashboardFeedFreshness(true, 3601000, 1000, freshness,
                                      sizeof(freshness)));
  assert(std::strcmp(freshness, "1h ago") == 0);
  assert(formatDashboardFeedFreshness(true, 1000, 2000, freshness,
                                      sizeof(freshness)));
  assert(std::strcmp(freshness, "waiting") == 0);
  assert(!formatDashboardFeedFreshness(true, 1000, 1000, nullptr, 0));
  char freshnessTooSmall[7] = {};
  assert(!formatDashboardFeedFreshness(false, 1000, 0, freshnessTooSmall,
                                       sizeof(freshnessTooSmall)));
  assert(freshnessTooSmall[0] == '\0');
  return 0;
}
