#include "dashboard_time.h"

#include <cstdio>

bool formatDashboardEasternDateTime(std::time_t epochSeconds, char* output,
                                    size_t capacity) {
  if (output == nullptr || capacity == 0) {
    return false;
  }
  output[0] = '\0';
  if (epochSeconds < kMinimumDashboardEpochSeconds) {
    return false;
  }

  std::tm localTime = {};
  if (localtime_r(&epochSeconds, &localTime) == nullptr) {
    return false;
  }
  const char* abbreviation = localTime.tm_isdst > 0 ? "EDT" : "EST";
  const int written = std::snprintf(
      output, capacity, "%04d-%02d-%02d %02d:%02d %s",
      localTime.tm_year + 1900, localTime.tm_mon + 1, localTime.tm_mday,
      localTime.tm_hour, localTime.tm_min, abbreviation);
  if (written < 0 || static_cast<size_t>(written) >= capacity) {
    output[0] = '\0';
    return false;
  }
  return true;
}
