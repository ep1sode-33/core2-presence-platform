#pragma once

#include <cstddef>
#include <ctime>

// newlib uses a POSIX TZ rule rather than an IANA zoneinfo database. This rule
// is the modern America/New_York schedule: UTC-5 in winter, UTC-4 between the
// second Sunday in March and the first Sunday in November.
constexpr char kDashboardEasternTimeZone[] =
    "EST5EDT,M3.2.0/2,M11.1.0/2";
constexpr std::time_t kMinimumDashboardEpochSeconds = 1700000000;
constexpr size_t kDashboardDateTimeCapacity = 32;

// Formats a trusted system epoch using the process-wide TZ already installed
// by configTzTime(). The output is fixed-width enough for the M5GO header, for
// example "2026-08-25 09:42 EDT".
bool formatDashboardEasternDateTime(std::time_t epochSeconds, char* output,
                                    size_t capacity);
