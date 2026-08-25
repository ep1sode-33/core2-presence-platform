#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <thread>
#include <type_traits>

#include "dashboard_mailbox.h"

namespace {

EnvironmentReading environmentAt(uint32_t id) {
  EnvironmentReading reading = {};
  reading.valid = true;
  reading.temperatureC = static_cast<float>(id);
  reading.humidityPct = static_cast<float>(id) + 0.25f;
  reading.pressureHpa = static_cast<float>(id) + 0.5f;
  reading.qmpTemperatureC = static_cast<float>(id) + 0.75f;
  std::strcpy(reading.timestamp, "2026-08-24T12:34:56Z");
  reading.status = id % 2 == 0 ? EnvironmentStatus::kOk
                              : EnvironmentStatus::kOutDated;
  return reading;
}

WeatherReading weatherAt(uint32_t id) {
  WeatherReading reading = {};
  reading.valid = true;
  reading.currentTemperatureF = static_cast<float>(id);
  reading.currentHumidityPct = static_cast<float>(id) + 0.125f;
  reading.apparentTemperatureF = static_cast<float>(id) + 0.25f;
  reading.currentWeatherCode = static_cast<uint8_t>(id % 100);
  std::strcpy(reading.currentTime, "2026-08-24T12:30");
  std::strcpy(reading.forecastDate, "2026-08-24");
  reading.forecastWeatherCode = static_cast<uint8_t>(id % 90);
  reading.temperatureMaxF = static_cast<float>(id) + 0.5f;
  reading.temperatureMinF = static_cast<float>(id) - 0.5f;
  reading.precipitationProbabilityMaxPct =
      static_cast<float>(id) + 0.625f;
  reading.rainSumIn = static_cast<float>(id) + 0.75f;
  reading.showersSumIn = static_cast<float>(id) + 0.875f;
  reading.snowfallSumIn = static_cast<float>(id) + 1.0f;
  return reading;
}

void assertEnvironmentEquals(const EnvironmentReading& actual,
                             const EnvironmentReading& expected) {
  assert(actual.valid == expected.valid);
  assert(actual.temperatureC == expected.temperatureC);
  assert(actual.humidityPct == expected.humidityPct);
  assert(actual.pressureHpa == expected.pressureHpa);
  assert(actual.qmpTemperatureC == expected.qmpTemperatureC);
  assert(std::strcmp(actual.timestamp, expected.timestamp) == 0);
  assert(actual.status == expected.status);
}

void assertWeatherEquals(const WeatherReading& actual,
                         const WeatherReading& expected) {
  assert(actual.valid == expected.valid);
  assert(actual.currentTemperatureF == expected.currentTemperatureF);
  assert(actual.currentHumidityPct == expected.currentHumidityPct);
  assert(actual.apparentTemperatureF == expected.apparentTemperatureF);
  assert(actual.currentWeatherCode == expected.currentWeatherCode);
  assert(std::strcmp(actual.currentTime, expected.currentTime) == 0);
  assert(std::strcmp(actual.forecastDate, expected.forecastDate) == 0);
  assert(actual.forecastWeatherCode == expected.forecastWeatherCode);
  assert(actual.temperatureMaxF == expected.temperatureMaxF);
  assert(actual.temperatureMinF == expected.temperatureMinF);
  assert(actual.precipitationProbabilityMaxPct ==
         expected.precipitationProbabilityMaxPct);
  assert(actual.rainSumIn == expected.rainSumIn);
  assert(actual.showersSumIn == expected.showersSumIn);
  assert(actual.snowfallSumIn == expected.snowfallSumIn);
}

void testInitialSnapshotIsEmpty() {
  static_assert(std::is_trivially_copyable<DashboardSnapshot>::value,
                "callers retain dashboard snapshots by value");

  DashboardMailbox mailbox;
  const DashboardSnapshot initial = mailbox.snapshot();
  assert(!initial.environment.valid);
  assert(!initial.weather.valid);
  assert(initial.environmentHealth.fetchedAtUptimeMs == 0);
  assert(initial.environmentHealth.lastAttemptUptimeMs == 0);
  assert(initial.environmentHealth.consecutiveFailures == 0);
  assert(initial.weatherHealth.fetchedAtUptimeMs == 0);
  assert(initial.weatherHealth.lastAttemptUptimeMs == 0);
  assert(initial.weatherHealth.consecutiveFailures == 0);
  assert(initial.version == 0);

  DashboardSnapshot copied = {};
  assert(mailbox.copySnapshot(&copied));
  assert(copied.version == 0);
  assert(!mailbox.copySnapshot(nullptr));
}

void testPartialUpdatesPreserveOtherSource() {
  DashboardMailbox mailbox;
  const EnvironmentReading environment = environmentAt(7);
  assert(mailbox.publishEnvironment(environment, 110, 100));

  DashboardSnapshot afterEnvironment = mailbox.snapshot();
  assertEnvironmentEquals(afterEnvironment.environment, environment);
  assert(!afterEnvironment.weather.valid);
  assert(afterEnvironment.environmentHealth.fetchedAtUptimeMs == 110);
  assert(afterEnvironment.environmentHealth.lastAttemptUptimeMs == 100);
  assert(afterEnvironment.environmentHealth.consecutiveFailures == 0);
  assert(afterEnvironment.version == 1);

  const WeatherReading weather = weatherAt(9);
  assert(mailbox.publishWeather(weather, 220));

  const DashboardSnapshot afterWeather = mailbox.snapshot();
  assertEnvironmentEquals(afterWeather.environment, environment);
  assertWeatherEquals(afterWeather.weather, weather);
  assert(afterWeather.environmentHealth.fetchedAtUptimeMs == 110);
  assert(afterWeather.environmentHealth.lastAttemptUptimeMs == 100);
  assert(afterWeather.weatherHealth.fetchedAtUptimeMs == 220);
  assert(afterWeather.weatherHealth.lastAttemptUptimeMs == 220);
  assert(afterWeather.version == 2);
}

void testFailuresPreserveLastGoodAcrossLargeUptimeValues() {
  DashboardMailbox mailbox;
  const EnvironmentReading lastGood = environmentAt(11);
  const WeatherReading lastGoodWeather = weatherAt(21);
  constexpr uint64_t kLargeUptime =
      std::numeric_limits<uint64_t>::max() - UINT64_C(1);
  assert(mailbox.publishEnvironment(lastGood, kLargeUptime));
  assert(mailbox.publishWeather(lastGoodWeather, 40, 39));

  mailbox.recordEnvironmentFailure(2);
  EnvironmentReading invalid = {};
  assert(!mailbox.publishEnvironment(invalid, 999, 3));
  mailbox.recordWeatherFailure(45);
  WeatherReading invalidWeather = {};
  assert(!mailbox.publishWeather(invalidWeather, 999, 46));

  DashboardSnapshot failed = mailbox.snapshot();
  assertEnvironmentEquals(failed.environment, lastGood);
  assertWeatherEquals(failed.weather, lastGoodWeather);
  assert(failed.environmentHealth.fetchedAtUptimeMs == kLargeUptime);
  assert(failed.environmentHealth.lastAttemptUptimeMs == 3);
  assert(failed.environmentHealth.consecutiveFailures == 2);
  assert(failed.weatherHealth.fetchedAtUptimeMs == 40);
  assert(failed.weatherHealth.lastAttemptUptimeMs == 46);
  assert(failed.weatherHealth.consecutiveFailures == 2);
  assert(failed.version == 6);

  const EnvironmentReading recovered = environmentAt(12);
  assert(mailbox.publishEnvironment(recovered, 8, 7));
  const DashboardSnapshot healthy = mailbox.snapshot();
  assertEnvironmentEquals(healthy.environment, recovered);
  assert(healthy.environmentHealth.fetchedAtUptimeMs == 8);
  assert(healthy.environmentHealth.lastAttemptUptimeMs == 7);
  assert(healthy.environmentHealth.consecutiveFailures == 0);
  assertWeatherEquals(healthy.weather, lastGoodWeather);
  assert(healthy.weatherHealth.consecutiveFailures == 2);
  assert(healthy.version == 7);
}

void testVersionTokenWrapIsDefined() {
  static_assert(nextDashboardSnapshotVersion(0) == 1,
                "first mutation advances the token");
  static_assert(nextDashboardSnapshotVersion(
                    std::numeric_limits<uint32_t>::max()) == 0,
                "version token uses defined unsigned wrap");
}

void assertCoherentConcurrentSnapshot(const DashboardSnapshot& snapshot) {
  if (snapshot.environment.valid) {
    const uint32_t id =
        static_cast<uint32_t>(snapshot.environment.temperatureC);
    assertEnvironmentEquals(snapshot.environment, environmentAt(id));
    assert(snapshot.environmentHealth.fetchedAtUptimeMs == id);
    assert(snapshot.environmentHealth.lastAttemptUptimeMs == id + 100000);
    assert(snapshot.environmentHealth.consecutiveFailures == 0);
  }

  if (snapshot.weather.valid) {
    const uint32_t id =
        static_cast<uint32_t>(snapshot.weather.currentTemperatureF);
    assertWeatherEquals(snapshot.weather, weatherAt(id));
    assert(snapshot.weatherHealth.fetchedAtUptimeMs == id + 200000);
    assert(snapshot.weatherHealth.lastAttemptUptimeMs == id + 300000);
    assert(snapshot.weatherHealth.consecutiveFailures == 0);
  }
}

void testConcurrentPublishAndCopyRemainCoherent() {
  constexpr uint32_t kIterations = 5000;
  DashboardMailbox mailbox;
  std::atomic<bool> start{false};
  std::atomic<bool> environmentDone{false};
  std::atomic<bool> weatherDone{false};

  const auto awaitStart = [&start]() {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  };

  std::thread environmentWriter([&]() {
    awaitStart();
    for (uint32_t id = 1; id <= kIterations; ++id) {
      assert(mailbox.publishEnvironment(environmentAt(id), id, id + 100000));
    }
    environmentDone.store(true, std::memory_order_release);
  });

  std::thread weatherWriter([&]() {
    awaitStart();
    for (uint32_t id = 1; id <= kIterations; ++id) {
      assert(mailbox.publishWeather(weatherAt(id), id + 200000,
                                    id + 300000));
    }
    weatherDone.store(true, std::memory_order_release);
  });

  std::thread reader([&]() {
    awaitStart();
    do {
      assertCoherentConcurrentSnapshot(mailbox.snapshot());
    } while (!environmentDone.load(std::memory_order_acquire) ||
             !weatherDone.load(std::memory_order_acquire));
  });

  start.store(true, std::memory_order_release);
  environmentWriter.join();
  weatherWriter.join();
  reader.join();

  const DashboardSnapshot final = mailbox.snapshot();
  assertCoherentConcurrentSnapshot(final);
  assertEnvironmentEquals(final.environment, environmentAt(kIterations));
  assertWeatherEquals(final.weather, weatherAt(kIterations));
  assert(final.version == kIterations * 2);
}

}  // namespace

int main() {
  testInitialSnapshotIsEmpty();
  testPartialUpdatesPreserveOtherSource();
  testFailuresPreserveLastGoodAcrossLargeUptimeValues();
  testVersionTokenWrapIsDefined();
  testConcurrentPublishAndCopyRemainCoherent();
  return 0;
}
