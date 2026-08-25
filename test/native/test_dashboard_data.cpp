#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

#include "dashboard_data.h"

namespace {

std::string validEnvironment() {
  return R"({
    "temperature_c":21.5,
    "humidity_pct":48.25,
    "pressure_hpa":1008.7,
    "qmp_temp_c":22.125,
    "ts":"2026-08-24T12:34:56+00:00",
    "status":"OK"
  })";
}

std::string validWeather() {
  return R"({
    "latitude":37.23,
    "current":{
      "time":"2026-08-24T08:15",
      "temperature_2m":72.5,
      "relative_humidity_2m":61,
      "apparent_temperature":73.25,
      "weather_code":3
    },
    "daily":{
      "time":["2026-08-24"],
      "weather_code":[61],
      "temperature_2m_max":[81.5],
      "temperature_2m_min":[59.25],
      "precipitation_probability_max":[70],
      "rain_sum":[0.18],
      "showers_sum":[0.04],
      "snowfall_sum":[0]
    }
  })";
}

void replaceOnce(std::string& text, std::string_view before,
                 std::string_view after) {
  const size_t position = text.find(before);
  assert(position != std::string::npos);
  text.replace(position, before.size(), after);
}

bool allZero(const char* bytes, size_t size) {
  return std::all_of(bytes, bytes + size,
                     [](char value) { return value == '\0'; });
}

EnvironmentParseResult parseEnvironment(std::string_view body) {
  return parseEnvironmentReading(body.data(), body.size());
}

WeatherParseResult parseWeather(std::string_view body) {
  return parseWeatherReading(body.data(), body.size());
}

void expectEnvironmentError(std::string_view body,
                            DashboardParseError expected) {
  const EnvironmentParseResult result = parseEnvironment(body);
  if (result.error != expected) {
    std::fprintf(stderr, "environment: expected %s, got %s\n",
                 dashboardParseErrorName(expected),
                 dashboardParseErrorName(result.error));
  }
  assert(!result.ok());
  assert(result.error == expected);
  assert(!result.reading.valid);
  assert(result.reading.temperatureC == 0.0f);
  assert(result.reading.humidityPct == 0.0f);
  assert(result.reading.pressureHpa == 0.0f);
  assert(result.reading.qmpTemperatureC == 0.0f);
  assert(allZero(result.reading.timestamp, sizeof(result.reading.timestamp)));
  assert(result.reading.status == EnvironmentStatus::kUnknown);
}

void expectWeatherError(std::string_view body,
                        DashboardParseError expected) {
  const WeatherParseResult result = parseWeather(body);
  if (result.error != expected) {
    std::fprintf(stderr, "weather: expected %s, got %s\n",
                 dashboardParseErrorName(expected),
                 dashboardParseErrorName(result.error));
  }
  assert(!result.ok());
  assert(result.error == expected);
  assert(!result.reading.valid);
  assert(result.reading.currentTemperatureF == 0.0f);
  assert(result.reading.currentHumidityPct == 0.0f);
  assert(result.reading.currentWeatherCode == 0);
  assert(allZero(result.reading.currentTime,
                 sizeof(result.reading.currentTime)));
  assert(allZero(result.reading.forecastDate,
                 sizeof(result.reading.forecastDate)));
  assert(result.reading.rainSumIn == 0.0f);
  assert(result.reading.snowfallSumIn == 0.0f);
}

void testPodContract() {
  static_assert(std::is_trivial<EnvironmentReading>::value,
                "EnvironmentReading must remain trivial");
  static_assert(std::is_standard_layout<EnvironmentReading>::value,
                "EnvironmentReading must remain standard-layout");
  static_assert(std::is_trivially_copyable<EnvironmentReading>::value,
                "EnvironmentReading must remain directly copyable");
  static_assert(std::is_trivial<WeatherReading>::value,
                "WeatherReading must remain trivial");
  static_assert(std::is_standard_layout<WeatherReading>::value,
                "WeatherReading must remain standard-layout");
  static_assert(std::is_trivially_copyable<WeatherReading>::value,
                "WeatherReading must remain directly copyable");
  static_assert(kDashboardTimestampCapacity == 33);
  static_assert(kDashboardDateCapacity == 17);
}

void testEnvironmentSuccessAndOrdering() {
  const EnvironmentParseResult basic = parseEnvironment(validEnvironment());
  assert(basic.ok());
  assert(basic.reading.valid);
  assert(std::fabs(basic.reading.temperatureC - 21.5f) < 0.001f);
  assert(std::fabs(basic.reading.humidityPct - 48.25f) < 0.001f);
  assert(std::fabs(basic.reading.pressureHpa - 1008.7f) < 0.001f);
  assert(std::fabs(basic.reading.qmpTemperatureC - 22.125f) < 0.001f);
  assert(std::strcmp(basic.reading.timestamp,
                     "2026-08-24T12:34:56+00:00") == 0);
  assert(basic.reading.status == EnvironmentStatus::kOk);

  const std::string reordered =
      " {\"future\":{\"nested\":[true,null,{\"label\":\"caf\u00e9\"}]},"
      "\"status\":\"OUT_DATED\",\"ts\":\"2026-08-24T12:34:56Z\","
      "\"qmp_temp_c\":-100,\"pressure_hpa\":1200,"
      "\"humidity_pct\":100,\"temperature\\u005fc\":100,"
      "\"future\":false} \n";
  const EnvironmentParseResult flexible = parseEnvironment(reordered);
  assert(flexible.ok());
  assert(flexible.reading.temperatureC == 100.0f);
  assert(flexible.reading.qmpTemperatureC == -100.0f);
  assert(flexible.reading.humidityPct == 100.0f);
  assert(flexible.reading.pressureHpa == 1200.0f);
  assert(flexible.reading.status == EnvironmentStatus::kOutDated);
}

void testEnvironmentContractFailures() {
  expectEnvironmentError("[]", DashboardParseError::kTopLevelNotObject);
  expectEnvironmentError("{}", DashboardParseError::kMissingField);
  expectEnvironmentError(validEnvironment() + " garbage",
                         DashboardParseError::kTrailingData);

  std::string missing = validEnvironment();
  replaceOnce(missing, "\"pressure_hpa\":1008.7,", "");
  expectEnvironmentError(missing, DashboardParseError::kMissingField);

  std::string duplicate = validEnvironment();
  replaceOnce(duplicate, "\"temperature_c\":21.5,",
              "\"temperature_c\":21.5,\"temperature\\u005fc\":22,");
  expectEnvironmentError(duplicate, DashboardParseError::kDuplicateField);

  std::string wrongType = validEnvironment();
  replaceOnce(wrongType, "\"humidity_pct\":48.25",
              "\"humidity_pct\":\"48.25\"");
  expectEnvironmentError(wrongType, DashboardParseError::kWrongType);

  std::string invalidStatus = validEnvironment();
  replaceOnce(invalidStatus, "\"status\":\"OK\"",
              "\"status\":\"STALE\"");
  expectEnvironmentError(invalidStatus,
                         DashboardParseError::kInvalidStatus);

  std::string nonFinite = validEnvironment();
  replaceOnce(nonFinite, "\"temperature_c\":21.5",
              "\"temperature_c\":NaN");
  expectEnvironmentError(nonFinite,
                         DashboardParseError::kNonFiniteNumber);

  std::string overflow = validEnvironment();
  replaceOnce(overflow, "\"pressure_hpa\":1008.7",
              "\"pressure_hpa\":1e999");
  expectEnvironmentError(overflow, DashboardParseError::kNumberOverflow);

  std::string outOfRange = validEnvironment();
  replaceOnce(outOfRange, "\"humidity_pct\":48.25",
              "\"humidity_pct\":100.001");
  expectEnvironmentError(outOfRange,
                         DashboardParseError::kValueOutOfRange);

  std::string leadingZero = validEnvironment();
  replaceOnce(leadingZero, "\"temperature_c\":21.5",
              "\"temperature_c\":021.5");
  expectEnvironmentError(leadingZero,
                         DashboardParseError::kMalformedJson);

  std::string longTimestamp = validEnvironment();
  replaceOnce(longTimestamp, "2026-08-24T12:34:56+00:00",
              std::string(33, 'x'));
  expectEnvironmentError(longTimestamp,
                         DashboardParseError::kStringTooLong);

  std::string embeddedNull = validEnvironment();
  replaceOnce(embeddedNull, "2026-08-24T12:34:56+00:00",
              "2026-08-24T12:34:56\\u0000Z");
  expectEnvironmentError(embeddedNull,
                         DashboardParseError::kInvalidString);

  std::string invalidUtf8 = validEnvironment();
  replaceOnce(invalidUtf8, "2026-08-24T12:34:56+00:00", "bad");
  const size_t badPosition = invalidUtf8.find("bad");
  assert(badPosition != std::string::npos);
  invalidUtf8[badPosition] = static_cast<char>(0xc0);
  expectEnvironmentError(invalidUtf8,
                         DashboardParseError::kInvalidUtf8);

  const EnvironmentParseResult nullResult =
      parseEnvironmentReading(nullptr, 0);
  assert(!nullResult.ok());
  assert(nullResult.error == DashboardParseError::kNullArgument);
  assert(!nullResult.reading.valid);
}

void testUnknownValuesAreStillValidated() {
  std::string malformed = validEnvironment();
  malformed.insert(malformed.rfind('}'), ",\"future\":[1,]");
  expectEnvironmentError(malformed, DashboardParseError::kMalformedJson);

  std::string overflow = validEnvironment();
  overflow.insert(overflow.rfind('}'), ",\"future\":1e999");
  expectEnvironmentError(overflow, DashboardParseError::kNumberOverflow);

  std::string invalidUtf8 = validEnvironment();
  std::string unknown = ",\"future\":\"";
  unknown.push_back(static_cast<char>(0xed));
  unknown.push_back(static_cast<char>(0xa0));
  unknown.push_back(static_cast<char>(0x80));
  unknown += "\"";
  invalidUtf8.insert(invalidUtf8.rfind('}'), unknown);
  expectEnvironmentError(invalidUtf8,
                         DashboardParseError::kInvalidUtf8);

  std::string nested = validEnvironment();
  std::string value;
  for (size_t index = 0; index < 33; ++index) {
    value += '[';
  }
  value += '0';
  for (size_t index = 0; index < 33; ++index) {
    value += ']';
  }
  nested.insert(nested.rfind('}'), ",\"future\":" + value);
  expectEnvironmentError(nested,
                         DashboardParseError::kNestingTooDeep);
}

void testWeatherSuccessAndArrayHandling() {
  const WeatherParseResult basic = parseWeather(validWeather());
  assert(basic.ok());
  assert(std::strcmp(basic.reading.currentTime, "2026-08-24T08:15") == 0);
  assert(std::strcmp(basic.reading.forecastDate, "2026-08-24") == 0);
  assert(std::fabs(basic.reading.currentTemperatureF - 72.5f) < 0.001f);
  assert(basic.reading.currentWeatherCode == 3);
  assert(basic.reading.forecastWeatherCode == 61);
  assert(std::fabs(basic.reading.temperatureMaxF - 81.5f) < 0.001f);
  assert(std::fabs(basic.reading.rainSumIn - 0.18f) < 0.001f);

  std::string flexible = validWeather();
  replaceOnce(flexible, "\"time\":[\"2026-08-24\"]",
              "\"time\":[\"2026-08-24\",\"2026-08-25\"]");
  replaceOnce(flexible, "\"weather_code\":[61]",
              "\"weather_code\":[61,80]");
  replaceOnce(flexible, "\"temperature_2m_max\":[81.5]",
              "\"temperature_2m_max\":[81.5,82]");
  replaceOnce(flexible, "\"temperature_2m_min\":[59.25]",
              "\"temperature_2m_min\":[59.25,60]");
  replaceOnce(flexible, "\"precipitation_probability_max\":[70]",
              "\"precipitation_probability_max\":[70,50]");
  replaceOnce(flexible, "\"rain_sum\":[0.18]",
              "\"rain_sum\":[0.18,0]");
  replaceOnce(flexible, "\"showers_sum\":[0.04]",
              "\"showers_sum\":[0.04,{\"future\":true}]");
  replaceOnce(flexible, "\"snowfall_sum\":[0]",
              "\"snowfall_sum\":[0,null],\"future_daily\":[]");
  replaceOnce(flexible, "\"time\":\"2026-08-24T08:15\"",
              "\"future_current\":\"ok\","
              "\"time\":\"2026-08-24T08:15\"");
  const WeatherParseResult multipleDays = parseWeather(flexible);
  assert(multipleDays.ok());
  assert(multipleDays.reading.forecastWeatherCode == 61);
  assert(multipleDays.reading.showersSumIn == 0.04f);
}

void testWeatherShapeAndFieldFailures() {
  expectWeatherError("null", DashboardParseError::kTopLevelNotObject);

  std::string missingTop = validWeather();
  const size_t dailyPosition = missingTop.find(",\n    \"daily\":");
  assert(dailyPosition != std::string::npos);
  missingTop.erase(dailyPosition, missingTop.rfind('}') - dailyPosition);
  expectWeatherError(missingTop, DashboardParseError::kMissingField);

  std::string wrongTopType = validWeather();
  const size_t currentStart = wrongTopType.find("\"current\":{");
  const size_t dailyStart = wrongTopType.find("    \"daily\":");
  assert(currentStart != std::string::npos);
  assert(dailyStart != std::string::npos);
  wrongTopType.replace(currentStart, dailyStart - currentStart - 1,
                       "\"current\":[],");
  expectWeatherError(wrongTopType, DashboardParseError::kWrongType);

  std::string missingCurrentField = validWeather();
  replaceOnce(missingCurrentField, "\"weather_code\":3", "\"future\":3");
  expectWeatherError(missingCurrentField,
                     DashboardParseError::kMissingField);

  std::string duplicateCurrentField = validWeather();
  replaceOnce(duplicateCurrentField, "\"weather_code\":3",
              "\"weather_code\":3,\"weather\\u005fcode\":4");
  expectWeatherError(duplicateCurrentField,
                     DashboardParseError::kDuplicateField);

  std::string duplicateTop = validWeather();
  duplicateTop.insert(duplicateTop.rfind('}'), ",\"current\":{}");
  expectWeatherError(duplicateTop, DashboardParseError::kDuplicateField);

  std::string missingDailyField = validWeather();
  replaceOnce(missingDailyField, "\"rain_sum\":[0.18]",
              "\"future_rain\":[0.18]");
  expectWeatherError(missingDailyField,
                     DashboardParseError::kMissingField);

  std::string duplicateDailyField = validWeather();
  replaceOnce(duplicateDailyField, "\"rain_sum\":[0.18]",
              "\"rain_sum\":[0.18],\"rain_sum\":[0]");
  expectWeatherError(duplicateDailyField,
                     DashboardParseError::kDuplicateField);

  std::string emptyDaily = validWeather();
  replaceOnce(emptyDaily, "\"rain_sum\":[0.18]", "\"rain_sum\":[]");
  expectWeatherError(emptyDaily, DashboardParseError::kMissingField);

  std::string wrongArrayType = validWeather();
  replaceOnce(wrongArrayType, "\"rain_sum\":[0.18]",
              "\"rain_sum\":0.18");
  expectWeatherError(wrongArrayType, DashboardParseError::kWrongType);

  std::string wrongElementType = validWeather();
  replaceOnce(wrongElementType, "\"rain_sum\":[0.18]",
              "\"rain_sum\":[\"0.18\"]");
  expectWeatherError(wrongElementType, DashboardParseError::kWrongType);

  std::string fractionalCode = validWeather();
  replaceOnce(fractionalCode, "\"weather_code\":3",
              "\"weather_code\":3.0");
  expectWeatherError(fractionalCode, DashboardParseError::kWrongType);
}

void testWeatherBoundsAndMalformedInput() {
  std::string minimums = validWeather();
  replaceOnce(minimums, "\"temperature_2m\":72.5",
              "\"temperature_2m\":-150");
  replaceOnce(minimums, "\"apparent_temperature\":73.25",
              "\"apparent_temperature\":160");
  replaceOnce(minimums, "\"relative_humidity_2m\":61",
              "\"relative_humidity_2m\":0");
  replaceOnce(minimums, "\"weather_code\":3",
              "\"weather_code\":99");
  replaceOnce(minimums, "\"temperature_2m_max\":[81.5]",
              "\"temperature_2m_max\":[160]");
  replaceOnce(minimums, "\"temperature_2m_min\":[59.25]",
              "\"temperature_2m_min\":[-150]");
  replaceOnce(minimums, "\"rain_sum\":[0.18]",
              "\"rain_sum\":[100]");
  assert(parseWeather(minimums).ok());

  std::string temperatureRange = validWeather();
  replaceOnce(temperatureRange, "\"temperature_2m\":72.5",
              "\"temperature_2m\":160.01");
  expectWeatherError(temperatureRange,
                     DashboardParseError::kValueOutOfRange);

  std::string weatherCodeRange = validWeather();
  replaceOnce(weatherCodeRange, "\"weather_code\":[61]",
              "\"weather_code\":[100]");
  expectWeatherError(weatherCodeRange,
                     DashboardParseError::kValueOutOfRange);

  std::string probabilityRange = validWeather();
  replaceOnce(probabilityRange,
              "\"precipitation_probability_max\":[70]",
              "\"precipitation_probability_max\":[-0.1]");
  expectWeatherError(probabilityRange,
                     DashboardParseError::kValueOutOfRange);

  std::string precipitationRange = validWeather();
  replaceOnce(precipitationRange, "\"snowfall_sum\":[0]",
              "\"snowfall_sum\":[100.1]");
  expectWeatherError(precipitationRange,
                     DashboardParseError::kValueOutOfRange);

  std::string overflow = validWeather();
  replaceOnce(overflow, "\"temperature_2m_max\":[81.5]",
              "\"temperature_2m_max\":[1e999]");
  expectWeatherError(overflow, DashboardParseError::kNumberOverflow);

  std::string trailingComma = validWeather();
  trailingComma.insert(trailingComma.rfind('}'), ",");
  expectWeatherError(trailingComma, DashboardParseError::kMalformedJson);

  std::string longDate = validWeather();
  replaceOnce(longDate, "\"time\":[\"2026-08-24\"]",
              "\"time\":[\"" + std::string(17, 'd') + "\"]");
  expectWeatherError(longDate, DashboardParseError::kStringTooLong);

  std::string invalidUtf8 = validWeather();
  replaceOnce(invalidUtf8, "2026-08-24T08:15", "bad");
  const size_t badPosition = invalidUtf8.find("bad");
  assert(badPosition != std::string::npos);
  invalidUtf8[badPosition] = static_cast<char>(0xf5);
  expectWeatherError(invalidUtf8, DashboardParseError::kInvalidUtf8);
}

void testErrorNames() {
  assert(std::strcmp(dashboardParseErrorName(DashboardParseError::kNone),
                     "none") == 0);
  assert(std::strcmp(
             dashboardParseErrorName(DashboardParseError::kValueOutOfRange),
             "value_out_of_range") == 0);
  assert(std::strcmp(dashboardParseErrorName(
                         static_cast<DashboardParseError>(255)),
                     "unknown") == 0);
}

}  // namespace

int main() {
  testPodContract();
  testEnvironmentSuccessAndOrdering();
  testEnvironmentContractFailures();
  testUnknownValuesAreStillValidated();
  testWeatherSuccessAndArrayHandling();
  testWeatherShapeAndFieldFailures();
  testWeatherBoundsAndMalformedInput();
  testErrorNames();
  return 0;
}
