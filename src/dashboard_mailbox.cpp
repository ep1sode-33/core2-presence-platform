#include "dashboard_mailbox.h"

#include <limits>
#include <type_traits>

namespace {

static_assert(std::is_trivially_copyable<EnvironmentReading>::value,
              "environment reading must remain POD-copyable");
static_assert(std::is_trivially_copyable<WeatherReading>::value,
              "weather reading must remain POD-copyable");
static_assert(std::is_trivially_copyable<DashboardSnapshot>::value,
              "dashboard snapshot must remain POD-copyable");

void recordFailure(DashboardSourceHealth* health,
                   uint64_t lastAttemptUptimeMs) {
  health->lastAttemptUptimeMs = lastAttemptUptimeMs;
  if (health->consecutiveFailures < std::numeric_limits<uint32_t>::max()) {
    ++health->consecutiveFailures;
  }
}

}  // namespace

void DashboardMailbox::lock() const {
#if defined(ARDUINO_ARCH_ESP32)
  portENTER_CRITICAL(&mutex_);
#else
  mutex_.lock();
#endif
}

void DashboardMailbox::unlock() const {
#if defined(ARDUINO_ARCH_ESP32)
  portEXIT_CRITICAL(&mutex_);
#else
  mutex_.unlock();
#endif
}

void DashboardMailbox::markChangedLocked() {
  snapshot_.version = nextDashboardSnapshotVersion(snapshot_.version);
}

bool DashboardMailbox::publishEnvironment(
    const EnvironmentReading& reading, uint64_t fetchedAtUptimeMs,
    uint64_t lastAttemptUptimeMs) {
  if (!reading.valid) {
    recordEnvironmentFailure(lastAttemptUptimeMs);
    return false;
  }

  lock();
  snapshot_.environment = reading;
  snapshot_.environmentHealth.fetchedAtUptimeMs = fetchedAtUptimeMs;
  snapshot_.environmentHealth.lastAttemptUptimeMs = lastAttemptUptimeMs;
  snapshot_.environmentHealth.consecutiveFailures = 0;
  markChangedLocked();
  unlock();
  return true;
}

bool DashboardMailbox::publishEnvironment(const EnvironmentReading& reading,
                                          uint64_t fetchedAtUptimeMs) {
  return publishEnvironment(reading, fetchedAtUptimeMs, fetchedAtUptimeMs);
}

bool DashboardMailbox::publishWeather(const WeatherReading& reading,
                                      uint64_t fetchedAtUptimeMs,
                                      uint64_t lastAttemptUptimeMs) {
  if (!reading.valid) {
    recordWeatherFailure(lastAttemptUptimeMs);
    return false;
  }

  lock();
  snapshot_.weather = reading;
  snapshot_.weatherHealth.fetchedAtUptimeMs = fetchedAtUptimeMs;
  snapshot_.weatherHealth.lastAttemptUptimeMs = lastAttemptUptimeMs;
  snapshot_.weatherHealth.consecutiveFailures = 0;
  markChangedLocked();
  unlock();
  return true;
}

bool DashboardMailbox::publishWeather(const WeatherReading& reading,
                                      uint64_t fetchedAtUptimeMs) {
  return publishWeather(reading, fetchedAtUptimeMs, fetchedAtUptimeMs);
}

void DashboardMailbox::recordEnvironmentFailure(
    uint64_t lastAttemptUptimeMs) {
  lock();
  recordFailure(&snapshot_.environmentHealth, lastAttemptUptimeMs);
  markChangedLocked();
  unlock();
}

void DashboardMailbox::recordWeatherFailure(uint64_t lastAttemptUptimeMs) {
  lock();
  recordFailure(&snapshot_.weatherHealth, lastAttemptUptimeMs);
  markChangedLocked();
  unlock();
}

void DashboardMailbox::markClockSynchronized() {
  lock();
  if (!snapshot_.clockSynchronized) {
    snapshot_.clockSynchronized = true;
    markChangedLocked();
  }
  unlock();
}

DashboardSnapshot DashboardMailbox::snapshot() const {
  DashboardSnapshot result = {};
  copySnapshot(&result);
  return result;
}

bool DashboardMailbox::copySnapshot(DashboardSnapshot* output) const {
  if (output == nullptr) {
    return false;
  }

  lock();
  *output = snapshot_;
  unlock();
  return true;
}
