#pragma once

#include <cstddef>
#include <cstdint>

// ISO-8601 timestamps returned by the dashboard sources are retained in
// fixed-size buffers so readings remain allocation-free and safely copyable.
constexpr size_t kDashboardTimestampCapacity = 33;
constexpr size_t kDashboardDateCapacity = 17;

enum class EnvironmentStatus : uint8_t {
  kUnknown = 0,
  kOk,
  kOutDated,
};

struct EnvironmentReading {
  bool valid;
  float temperatureC;
  float humidityPct;
  float pressureHpa;
  float qmpTemperatureC;
  char timestamp[kDashboardTimestampCapacity];
  EnvironmentStatus status;
};

struct WeatherReading {
  bool valid;
  float currentTemperatureF;
  float currentHumidityPct;
  float apparentTemperatureF;
  uint8_t currentWeatherCode;
  char currentTime[kDashboardTimestampCapacity];
  char forecastDate[kDashboardDateCapacity];
  uint8_t forecastWeatherCode;
  float temperatureMaxF;
  float temperatureMinF;
  float precipitationProbabilityMaxPct;
  float rainSumIn;
  float showersSumIn;
  float snowfallSumIn;
};

enum class DashboardParseError : uint8_t {
  kNone = 0,
  kNullArgument,
  kTopLevelNotObject,
  kMalformedJson,
  kInvalidUtf8,
  kMissingField,
  kDuplicateField,
  kWrongType,
  kNonFiniteNumber,
  kNumberOverflow,
  kValueOutOfRange,
  kStringTooLong,
  kInvalidString,
  kInvalidStatus,
  kTrailingData,
  kNestingTooDeep,
};

struct EnvironmentParseResult {
  EnvironmentReading reading;
  DashboardParseError error;

  bool ok() const {
    return error == DashboardParseError::kNone && reading.valid;
  }
  explicit operator bool() const { return ok(); }
};

struct WeatherParseResult {
  WeatherReading reading;
  DashboardParseError error;

  bool ok() const {
    return error == DashboardParseError::kNone && reading.valid;
  }
  explicit operator bool() const { return ok(); }
};

// Parses the complete /metrics response. Required fields are temperature_c,
// humidity_pct, pressure_hpa, qmp_temp_c, ts, and status. Unknown fields are
// accepted; duplicate required fields are rejected.
EnvironmentParseResult parseEnvironmentReading(const char* json,
                                               size_t jsonLength);

// Parses the complete Open-Meteo response. The required current fields are
// time, temperature_2m, relative_humidity_2m, apparent_temperature, and
// weather_code. The first element of each required daily array is retained.
// Unknown fields and additional daily array elements are accepted.
WeatherParseResult parseWeatherReading(const char* json, size_t jsonLength);

const char* dashboardParseErrorName(DashboardParseError error);
