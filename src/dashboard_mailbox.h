#pragma once

#include <cstdint>

#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#else
#include <mutex>
#endif

#include "dashboard_data.h"

// Fetch metadata is kept separately from the last good reading so a failed
// refresh can be rendered without discarding useful data.
struct DashboardSourceHealth {
  uint64_t fetchedAtUptimeMs = 0;
  uint64_t lastAttemptUptimeMs = 0;
  uint32_t consecutiveFailures = 0;
};

// Fixed-size worker-to-main snapshot. It deliberately contains no pointers or
// owning containers, so callers can safely retain a copy after releasing the
// mailbox lock.
struct DashboardSnapshot {
  EnvironmentReading environment = {};
  WeatherReading weather = {};
  DashboardSourceHealth environmentHealth = {};
  DashboardSourceHealth weatherHealth = {};
  uint32_t version = 0;
};

// Versions are equality tokens, not ordered revisions. Unsigned wrap is
// intentional; consumers should redraw whenever current != lastSeen.
constexpr uint32_t nextDashboardSnapshotVersion(uint32_t current) {
  return static_cast<uint32_t>(current + UINT32_C(1));
}

class DashboardMailbox {
 public:
  DashboardMailbox() = default;
  DashboardMailbox(const DashboardMailbox&) = delete;
  DashboardMailbox& operator=(const DashboardMailbox&) = delete;

  // A valid reading becomes the new last good value and resets the failure
  // count. An invalid reading is treated as a failed attempt and leaves the
  // last good value and fetched-at timestamp untouched.
  bool publishEnvironment(const EnvironmentReading& reading,
                          uint64_t fetchedAtUptimeMs,
                          uint64_t lastAttemptUptimeMs);
  bool publishEnvironment(const EnvironmentReading& reading,
                          uint64_t fetchedAtUptimeMs);
  bool publishWeather(const WeatherReading& reading,
                      uint64_t fetchedAtUptimeMs,
                      uint64_t lastAttemptUptimeMs);
  bool publishWeather(const WeatherReading& reading,
                      uint64_t fetchedAtUptimeMs);

  void recordEnvironmentFailure(uint64_t lastAttemptUptimeMs);
  void recordWeatherFailure(uint64_t lastAttemptUptimeMs);

  DashboardSnapshot snapshot() const;
  bool copySnapshot(DashboardSnapshot* output) const;

 private:
  void lock() const;
  void unlock() const;
  void markChangedLocked();

#if defined(ARDUINO_ARCH_ESP32)
  mutable portMUX_TYPE mutex_ = portMUX_INITIALIZER_UNLOCKED;
#else
  mutable std::mutex mutex_;
#endif
  DashboardSnapshot snapshot_ = {};
};
