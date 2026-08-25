#include "dashboard_data.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {

constexpr size_t kMaximumJsonNesting = 32;
constexpr double kMinimumTemperatureC = -100.0;
constexpr double kMaximumTemperatureC = 100.0;
constexpr double kMinimumTemperatureF = -150.0;
constexpr double kMaximumTemperatureF = 160.0;
constexpr double kMinimumHumidityPct = 0.0;
constexpr double kMaximumHumidityPct = 100.0;
constexpr double kMinimumPressureHpa = 300.0;
constexpr double kMaximumPressureHpa = 1200.0;
constexpr double kMinimumWeatherCode = 0.0;
constexpr double kMaximumWeatherCode = 99.0;
constexpr double kMinimumPrecipitationIn = 0.0;
constexpr double kMaximumPrecipitationIn = 100.0;

struct DecodedString {
  char bytes[96];
  size_t length;
  bool truncated;
  bool containsNull;
};

struct NumberToken {
  size_t start;
  size_t end;
  bool integer;
};

bool bytesEqual(const char* actual, size_t actualLength,
                const char* expected) {
  const size_t expectedLength = std::strlen(expected);
  return actualLength == expectedLength &&
         (actualLength == 0 ||
          std::memcmp(actual, expected, actualLength) == 0);
}

class DashboardJsonParser {
 public:
  DashboardJsonParser(const char* input, size_t inputLength)
      : input_(input), inputLength_(inputLength) {}

  EnvironmentParseResult parseEnvironment() {
    EnvironmentReading reading = {};
    if (input_ == nullptr) {
      return environmentFailure(DashboardParseError::kNullArgument);
    }
    if (!beginTopLevelObject()) {
      return environmentFailure(error_);
    }

    uint8_t seen = 0;
    skipWhitespace();
    if (!consume('}')) {
      while (true) {
        DecodedString key = {};
        if (!parseString(key)) {
          return environmentFailure(error_);
        }
        skipWhitespace();
        if (!consume(':')) {
          return environmentFailure(DashboardParseError::kMalformedJson);
        }
        skipWhitespace();
        if (!parseEnvironmentField(key, seen, reading)) {
          return environmentFailure(error_);
        }
        skipWhitespace();
        if (consume('}')) {
          break;
        }
        if (!consume(',')) {
          return environmentFailure(DashboardParseError::kMalformedJson);
        }
        skipWhitespace();
      }
    }

    if (!finishDocument()) {
      return environmentFailure(error_);
    }
    constexpr uint8_t kAllFields = (1U << 6U) - 1U;
    if (seen != kAllFields) {
      return environmentFailure(DashboardParseError::kMissingField);
    }
    reading.valid = true;
    return {reading, DashboardParseError::kNone};
  }

  WeatherParseResult parseWeather() {
    WeatherReading reading = {};
    if (input_ == nullptr) {
      return weatherFailure(DashboardParseError::kNullArgument);
    }
    if (!beginTopLevelObject()) {
      return weatherFailure(error_);
    }

    uint8_t seen = 0;
    skipWhitespace();
    if (!consume('}')) {
      while (true) {
        DecodedString key = {};
        if (!parseString(key)) {
          return weatherFailure(error_);
        }
        skipWhitespace();
        if (!consume(':')) {
          return weatherFailure(DashboardParseError::kMalformedJson);
        }
        skipWhitespace();
        if (!parseWeatherTopLevelField(key, seen, reading)) {
          return weatherFailure(error_);
        }
        skipWhitespace();
        if (consume('}')) {
          break;
        }
        if (!consume(',')) {
          return weatherFailure(DashboardParseError::kMalformedJson);
        }
        skipWhitespace();
      }
    }

    if (!finishDocument()) {
      return weatherFailure(error_);
    }
    constexpr uint8_t kAllFields = (1U << 2U) - 1U;
    if (seen != kAllFields) {
      return weatherFailure(DashboardParseError::kMissingField);
    }
    reading.valid = true;
    return {reading, DashboardParseError::kNone};
  }

 private:
  static EnvironmentParseResult environmentFailure(
      DashboardParseError error) {
    return {{}, error};
  }

  static WeatherParseResult weatherFailure(DashboardParseError error) {
    return {{}, error};
  }

  bool beginTopLevelObject() {
    skipWhitespace();
    if (position_ >= inputLength_ || input_[position_] != '{') {
      return fail(DashboardParseError::kTopLevelNotObject);
    }
    ++position_;
    return true;
  }

  bool finishDocument() {
    skipWhitespace();
    if (position_ != inputLength_) {
      return fail(DashboardParseError::kTrailingData);
    }
    return true;
  }

  bool parseEnvironmentField(const DecodedString& key, uint8_t& seen,
                             EnvironmentReading& reading) {
    uint8_t bit = 0;
    if (keyEquals(key, "temperature_c")) {
      bit = 1U << 0U;
    } else if (keyEquals(key, "humidity_pct")) {
      bit = 1U << 1U;
    } else if (keyEquals(key, "pressure_hpa")) {
      bit = 1U << 2U;
    } else if (keyEquals(key, "qmp_temp_c")) {
      bit = 1U << 3U;
    } else if (keyEquals(key, "ts")) {
      bit = 1U << 4U;
    } else if (keyEquals(key, "status")) {
      bit = 1U << 5U;
    } else {
      return skipValue(0);
    }

    if (!markSeen(bit, seen)) {
      return false;
    }
    if (bit == (1U << 0U)) {
      return parseBoundedFloat(reading.temperatureC, kMinimumTemperatureC,
                               kMaximumTemperatureC);
    }
    if (bit == (1U << 1U)) {
      return parseBoundedFloat(reading.humidityPct, kMinimumHumidityPct,
                               kMaximumHumidityPct);
    }
    if (bit == (1U << 2U)) {
      return parseBoundedFloat(reading.pressureHpa, kMinimumPressureHpa,
                               kMaximumPressureHpa);
    }
    if (bit == (1U << 3U)) {
      return parseBoundedFloat(reading.qmpTemperatureC,
                               kMinimumTemperatureC,
                               kMaximumTemperatureC);
    }
    if (bit == (1U << 4U)) {
      return parseRetainedString(reading.timestamp,
                                 sizeof(reading.timestamp));
    }

    DecodedString status = {};
    if (!parseTypedString(status)) {
      return false;
    }
    if (keyEquals(status, "OK")) {
      reading.status = EnvironmentStatus::kOk;
      return true;
    }
    if (keyEquals(status, "OUT_DATED")) {
      reading.status = EnvironmentStatus::kOutDated;
      return true;
    }
    return fail(DashboardParseError::kInvalidStatus);
  }

  bool parseWeatherTopLevelField(const DecodedString& key, uint8_t& seen,
                                 WeatherReading& reading) {
    uint8_t bit = 0;
    if (keyEquals(key, "current")) {
      bit = 1U << 0U;
    } else if (keyEquals(key, "daily")) {
      bit = 1U << 1U;
    } else {
      return skipValue(0);
    }
    if (!markSeen(bit, seen)) {
      return false;
    }
    return bit == (1U << 0U) ? parseCurrentObject(reading)
                             : parseDailyObject(reading);
  }

  bool parseCurrentObject(WeatherReading& reading) {
    if (!consume('{')) {
      return fail(DashboardParseError::kWrongType);
    }
    uint8_t seen = 0;
    skipWhitespace();
    if (!consume('}')) {
      while (true) {
        DecodedString key = {};
        if (!parseString(key)) {
          return false;
        }
        skipWhitespace();
        if (!consume(':')) {
          return fail(DashboardParseError::kMalformedJson);
        }
        skipWhitespace();
        if (!parseCurrentField(key, seen, reading)) {
          return false;
        }
        skipWhitespace();
        if (consume('}')) {
          break;
        }
        if (!consume(',')) {
          return fail(DashboardParseError::kMalformedJson);
        }
        skipWhitespace();
      }
    }
    constexpr uint8_t kAllFields = (1U << 5U) - 1U;
    return seen == kAllFields || fail(DashboardParseError::kMissingField);
  }

  bool parseCurrentField(const DecodedString& key, uint8_t& seen,
                         WeatherReading& reading) {
    uint8_t bit = 0;
    if (keyEquals(key, "time")) {
      bit = 1U << 0U;
    } else if (keyEquals(key, "temperature_2m")) {
      bit = 1U << 1U;
    } else if (keyEquals(key, "relative_humidity_2m")) {
      bit = 1U << 2U;
    } else if (keyEquals(key, "apparent_temperature")) {
      bit = 1U << 3U;
    } else if (keyEquals(key, "weather_code")) {
      bit = 1U << 4U;
    } else {
      return skipValue(0);
    }
    if (!markSeen(bit, seen)) {
      return false;
    }
    if (bit == (1U << 0U)) {
      return parseRetainedString(reading.currentTime,
                                 sizeof(reading.currentTime));
    }
    if (bit == (1U << 1U)) {
      return parseBoundedFloat(reading.currentTemperatureF,
                               kMinimumTemperatureF,
                               kMaximumTemperatureF);
    }
    if (bit == (1U << 2U)) {
      return parseBoundedFloat(reading.currentHumidityPct,
                               kMinimumHumidityPct, kMaximumHumidityPct);
    }
    if (bit == (1U << 3U)) {
      return parseBoundedFloat(reading.apparentTemperatureF,
                               kMinimumTemperatureF,
                               kMaximumTemperatureF);
    }
    return parseBoundedUint8(reading.currentWeatherCode,
                             kMinimumWeatherCode, kMaximumWeatherCode);
  }

  bool parseDailyObject(WeatherReading& reading) {
    if (!consume('{')) {
      return fail(DashboardParseError::kWrongType);
    }
    uint8_t seen = 0;
    skipWhitespace();
    if (!consume('}')) {
      while (true) {
        DecodedString key = {};
        if (!parseString(key)) {
          return false;
        }
        skipWhitespace();
        if (!consume(':')) {
          return fail(DashboardParseError::kMalformedJson);
        }
        skipWhitespace();
        if (!parseDailyField(key, seen, reading)) {
          return false;
        }
        skipWhitespace();
        if (consume('}')) {
          break;
        }
        if (!consume(',')) {
          return fail(DashboardParseError::kMalformedJson);
        }
        skipWhitespace();
      }
    }
    constexpr uint16_t kAllFields = (1U << 8U) - 1U;
    return seen == kAllFields || fail(DashboardParseError::kMissingField);
  }

  bool parseDailyField(const DecodedString& key, uint8_t& seen,
                       WeatherReading& reading) {
    uint8_t bit = 0;
    if (keyEquals(key, "time")) {
      bit = 1U << 0U;
    } else if (keyEquals(key, "weather_code")) {
      bit = 1U << 1U;
    } else if (keyEquals(key, "temperature_2m_max")) {
      bit = 1U << 2U;
    } else if (keyEquals(key, "temperature_2m_min")) {
      bit = 1U << 3U;
    } else if (keyEquals(key, "precipitation_probability_max")) {
      bit = 1U << 4U;
    } else if (keyEquals(key, "rain_sum")) {
      bit = 1U << 5U;
    } else if (keyEquals(key, "showers_sum")) {
      bit = 1U << 6U;
    } else if (keyEquals(key, "snowfall_sum")) {
      bit = 1U << 7U;
    } else {
      return skipValue(0);
    }
    if (!markSeen(bit, seen)) {
      return false;
    }
    if (bit == (1U << 0U)) {
      return parseFirstString(reading.forecastDate,
                              sizeof(reading.forecastDate));
    }
    if (bit == (1U << 1U)) {
      return parseFirstUint8(reading.forecastWeatherCode,
                             kMinimumWeatherCode, kMaximumWeatherCode);
    }
    if (bit == (1U << 2U)) {
      return parseFirstFloat(reading.temperatureMaxF,
                             kMinimumTemperatureF, kMaximumTemperatureF);
    }
    if (bit == (1U << 3U)) {
      return parseFirstFloat(reading.temperatureMinF,
                             kMinimumTemperatureF, kMaximumTemperatureF);
    }
    if (bit == (1U << 4U)) {
      return parseFirstFloat(reading.precipitationProbabilityMaxPct,
                             kMinimumHumidityPct, kMaximumHumidityPct);
    }
    if (bit == (1U << 5U)) {
      return parseFirstFloat(reading.rainSumIn, kMinimumPrecipitationIn,
                             kMaximumPrecipitationIn);
    }
    if (bit == (1U << 6U)) {
      return parseFirstFloat(reading.showersSumIn, kMinimumPrecipitationIn,
                             kMaximumPrecipitationIn);
    }
    return parseFirstFloat(reading.snowfallSumIn, kMinimumPrecipitationIn,
                           kMaximumPrecipitationIn);
  }

  bool parseFirstString(char* destination, size_t capacity) {
    if (!beginNonemptyArray()) {
      return false;
    }
    if (!parseRetainedString(destination, capacity)) {
      return false;
    }
    return finishArray();
  }

  bool parseFirstFloat(float& destination, double minimum, double maximum) {
    if (!beginNonemptyArray()) {
      return false;
    }
    if (!parseBoundedFloat(destination, minimum, maximum)) {
      return false;
    }
    return finishArray();
  }

  bool parseFirstUint8(uint8_t& destination, double minimum,
                       double maximum) {
    if (!beginNonemptyArray()) {
      return false;
    }
    if (!parseBoundedUint8(destination, minimum, maximum)) {
      return false;
    }
    return finishArray();
  }

  bool beginNonemptyArray() {
    if (!consume('[')) {
      return fail(DashboardParseError::kWrongType);
    }
    skipWhitespace();
    if (consume(']')) {
      return fail(DashboardParseError::kMissingField);
    }
    return true;
  }

  bool finishArray() {
    skipWhitespace();
    while (consume(',')) {
      skipWhitespace();
      if (position_ < inputLength_ && input_[position_] == ']') {
        return fail(DashboardParseError::kMalformedJson);
      }
      if (!skipValue(0)) {
        return false;
      }
      skipWhitespace();
    }
    if (!consume(']')) {
      return fail(DashboardParseError::kMalformedJson);
    }
    return true;
  }

  bool parseBoundedFloat(float& destination, double minimum,
                         double maximum) {
    double value = 0.0;
    if (!parseNumber(value)) {
      return false;
    }
    if (value < minimum || value > maximum) {
      return fail(DashboardParseError::kValueOutOfRange);
    }
    destination = static_cast<float>(value);
    return true;
  }

  bool parseBoundedUint8(uint8_t& destination, double minimum,
                         double maximum) {
    NumberToken token = {};
    if (!scanNumber(token)) {
      return false;
    }
    if (!token.integer) {
      return fail(DashboardParseError::kWrongType);
    }
    double value = 0.0;
    if (!convertNumber(token, value)) {
      return false;
    }
    if (value < minimum || value > maximum) {
      return fail(DashboardParseError::kValueOutOfRange);
    }
    destination = static_cast<uint8_t>(value);
    return true;
  }

  bool parseNumber(double& destination) {
    NumberToken token = {};
    return scanNumber(token) && convertNumber(token, destination);
  }

  bool scanNumber(NumberToken& token) {
    if (startsWith("NaN") || startsWith("Infinity") ||
        startsWith("-Infinity")) {
      return fail(DashboardParseError::kNonFiniteNumber);
    }
    if (position_ >= inputLength_ ||
        (input_[position_] != '-' && !isDigit(input_[position_]))) {
      return fail(DashboardParseError::kWrongType);
    }

    token.start = position_;
    token.integer = true;
    consume('-');
    if (position_ >= inputLength_ || !isDigit(input_[position_])) {
      return fail(DashboardParseError::kMalformedJson);
    }
    if (input_[position_] == '0') {
      ++position_;
      if (position_ < inputLength_ && isDigit(input_[position_])) {
        return fail(DashboardParseError::kMalformedJson);
      }
    } else {
      while (position_ < inputLength_ && isDigit(input_[position_])) {
        ++position_;
      }
    }

    if (position_ < inputLength_ && input_[position_] == '.') {
      token.integer = false;
      ++position_;
      if (position_ >= inputLength_ || !isDigit(input_[position_])) {
        return fail(DashboardParseError::kMalformedJson);
      }
      while (position_ < inputLength_ && isDigit(input_[position_])) {
        ++position_;
      }
    }
    if (position_ < inputLength_ &&
        (input_[position_] == 'e' || input_[position_] == 'E')) {
      token.integer = false;
      ++position_;
      if (position_ < inputLength_ &&
          (input_[position_] == '+' || input_[position_] == '-')) {
        ++position_;
      }
      if (position_ >= inputLength_ || !isDigit(input_[position_])) {
        return fail(DashboardParseError::kMalformedJson);
      }
      while (position_ < inputLength_ && isDigit(input_[position_])) {
        ++position_;
      }
    }
    token.end = position_;
    return true;
  }

  bool convertNumber(const NumberToken& token, double& destination) {
    const size_t tokenLength = token.end - token.start;
    char buffer[128] = {};
    if (tokenLength >= sizeof(buffer)) {
      return fail(DashboardParseError::kNumberOverflow);
    }
    std::memcpy(buffer, input_ + token.start, tokenLength);
    buffer[tokenLength] = '\0';

    errno = 0;
    char* end = nullptr;
    const double value = std::strtod(buffer, &end);
    if (end != buffer + tokenLength) {
      return fail(DashboardParseError::kMalformedJson);
    }
    if (!std::isfinite(value) || (errno == ERANGE && value != 0.0)) {
      return fail(DashboardParseError::kNumberOverflow);
    }
    destination = value;
    return true;
  }

  bool parseRetainedString(char* destination, size_t capacity) {
    DecodedString decoded = {};
    if (!parseTypedString(decoded)) {
      return false;
    }
    if (decoded.length >= capacity || decoded.truncated) {
      return fail(DashboardParseError::kStringTooLong);
    }
    if (decoded.containsNull) {
      return fail(DashboardParseError::kInvalidString);
    }
    if (decoded.length > 0) {
      std::memcpy(destination, decoded.bytes, decoded.length);
    }
    destination[decoded.length] = '\0';
    return true;
  }

  bool parseTypedString(DecodedString& decoded) {
    if (position_ >= inputLength_ || input_[position_] != '"') {
      return fail(DashboardParseError::kWrongType);
    }
    return parseString(decoded);
  }

  bool parseString(DecodedString& decoded) {
    if (!consume('"')) {
      return fail(DashboardParseError::kMalformedJson);
    }
    decoded = {};

    while (position_ < inputLength_) {
      const uint8_t byte = static_cast<uint8_t>(input_[position_]);
      if (byte == static_cast<uint8_t>('"')) {
        ++position_;
        return true;
      }
      if (byte == static_cast<uint8_t>('\\')) {
        ++position_;
        if (position_ >= inputLength_) {
          return fail(DashboardParseError::kMalformedJson);
        }
        const char escape = input_[position_++];
        switch (escape) {
          case '"':
          case '\\':
          case '/':
            appendDecodedByte(static_cast<uint8_t>(escape), decoded);
            break;
          case 'b':
            appendDecodedByte(0x08U, decoded);
            break;
          case 'f':
            appendDecodedByte(0x0cU, decoded);
            break;
          case 'n':
            appendDecodedByte(0x0aU, decoded);
            break;
          case 'r':
            appendDecodedByte(0x0dU, decoded);
            break;
          case 't':
            appendDecodedByte(0x09U, decoded);
            break;
          case 'u':
            if (!parseUnicodeEscape(decoded)) {
              return false;
            }
            break;
          default:
            return fail(DashboardParseError::kMalformedJson);
        }
        continue;
      }
      if (byte < 0x20U) {
        return fail(DashboardParseError::kMalformedJson);
      }
      if (byte < 0x80U) {
        ++position_;
        appendDecodedByte(byte, decoded);
        continue;
      }
      if (!parseRawUtf8(decoded)) {
        return false;
      }
    }
    return fail(DashboardParseError::kMalformedJson);
  }

  bool parseUnicodeEscape(DecodedString& decoded) {
    uint16_t first = 0;
    if (!parseHexQuad(first)) {
      return false;
    }
    uint32_t codePoint = first;
    if (first >= 0xd800U && first <= 0xdbffU) {
      if (position_ + 2U > inputLength_ || input_[position_] != '\\' ||
          input_[position_ + 1U] != 'u') {
        return fail(DashboardParseError::kInvalidUtf8);
      }
      position_ += 2U;
      uint16_t second = 0;
      if (!parseHexQuad(second)) {
        return false;
      }
      if (second < 0xdc00U || second > 0xdfffU) {
        return fail(DashboardParseError::kInvalidUtf8);
      }
      codePoint = 0x10000U +
                  ((static_cast<uint32_t>(first) - 0xd800U) << 10U) +
                  (static_cast<uint32_t>(second) - 0xdc00U);
    } else if (first >= 0xdc00U && first <= 0xdfffU) {
      return fail(DashboardParseError::kInvalidUtf8);
    }
    appendCodePoint(codePoint, decoded);
    return true;
  }

  bool parseHexQuad(uint16_t& value) {
    if (position_ + 4U > inputLength_) {
      return fail(DashboardParseError::kMalformedJson);
    }
    uint16_t parsed = 0;
    for (size_t index = 0; index < 4U; ++index) {
      const int nibble = hexValue(input_[position_ + index]);
      if (nibble < 0) {
        return fail(DashboardParseError::kMalformedJson);
      }
      parsed = static_cast<uint16_t>((parsed << 4U) |
                                     static_cast<uint16_t>(nibble));
    }
    position_ += 4U;
    value = parsed;
    return true;
  }

  bool parseRawUtf8(DecodedString& decoded) {
    const uint8_t first = static_cast<uint8_t>(input_[position_]);
    size_t length = 0;
    uint8_t secondMinimum = 0x80U;
    uint8_t secondMaximum = 0xbfU;
    if (first >= 0xc2U && first <= 0xdfU) {
      length = 2;
    } else if (first >= 0xe0U && first <= 0xefU) {
      length = 3;
      if (first == 0xe0U) {
        secondMinimum = 0xa0U;
      } else if (first == 0xedU) {
        secondMaximum = 0x9fU;
      }
    } else if (first >= 0xf0U && first <= 0xf4U) {
      length = 4;
      if (first == 0xf0U) {
        secondMinimum = 0x90U;
      } else if (first == 0xf4U) {
        secondMaximum = 0x8fU;
      }
    } else {
      return fail(DashboardParseError::kInvalidUtf8);
    }
    if (position_ + length > inputLength_) {
      return fail(DashboardParseError::kInvalidUtf8);
    }
    const uint8_t second = static_cast<uint8_t>(input_[position_ + 1U]);
    if (second < secondMinimum || second > secondMaximum) {
      return fail(DashboardParseError::kInvalidUtf8);
    }
    for (size_t index = 2; index < length; ++index) {
      const uint8_t continuation =
          static_cast<uint8_t>(input_[position_ + index]);
      if (continuation < 0x80U || continuation > 0xbfU) {
        return fail(DashboardParseError::kInvalidUtf8);
      }
    }
    for (size_t index = 0; index < length; ++index) {
      appendDecodedByte(static_cast<uint8_t>(input_[position_ + index]),
                        decoded);
    }
    position_ += length;
    return true;
  }

  static void appendCodePoint(uint32_t codePoint, DecodedString& decoded) {
    if (codePoint <= 0x7fU) {
      appendDecodedByte(static_cast<uint8_t>(codePoint), decoded);
    } else if (codePoint <= 0x7ffU) {
      appendDecodedByte(static_cast<uint8_t>(0xc0U | (codePoint >> 6U)),
                        decoded);
      appendDecodedByte(static_cast<uint8_t>(0x80U | (codePoint & 0x3fU)),
                        decoded);
    } else if (codePoint <= 0xffffU) {
      appendDecodedByte(static_cast<uint8_t>(0xe0U | (codePoint >> 12U)),
                        decoded);
      appendDecodedByte(
          static_cast<uint8_t>(0x80U | ((codePoint >> 6U) & 0x3fU)),
          decoded);
      appendDecodedByte(static_cast<uint8_t>(0x80U | (codePoint & 0x3fU)),
                        decoded);
    } else {
      appendDecodedByte(static_cast<uint8_t>(0xf0U | (codePoint >> 18U)),
                        decoded);
      appendDecodedByte(
          static_cast<uint8_t>(0x80U | ((codePoint >> 12U) & 0x3fU)),
          decoded);
      appendDecodedByte(
          static_cast<uint8_t>(0x80U | ((codePoint >> 6U) & 0x3fU)),
          decoded);
      appendDecodedByte(static_cast<uint8_t>(0x80U | (codePoint & 0x3fU)),
                        decoded);
    }
  }

  static void appendDecodedByte(uint8_t byte, DecodedString& decoded) {
    if (byte == 0) {
      decoded.containsNull = true;
    }
    if (decoded.length < sizeof(decoded.bytes)) {
      decoded.bytes[decoded.length] = static_cast<char>(byte);
    } else {
      decoded.truncated = true;
    }
    ++decoded.length;
  }

  bool skipValue(size_t depth) {
    if (depth >= kMaximumJsonNesting) {
      return fail(DashboardParseError::kNestingTooDeep);
    }
    if (position_ >= inputLength_) {
      return fail(DashboardParseError::kMalformedJson);
    }
    const char character = input_[position_];
    if (character == '"') {
      DecodedString ignored = {};
      return parseString(ignored);
    }
    if (character == '{') {
      ++position_;
      skipWhitespace();
      if (consume('}')) {
        return true;
      }
      while (true) {
        DecodedString key = {};
        if (!parseString(key)) {
          return false;
        }
        skipWhitespace();
        if (!consume(':')) {
          return fail(DashboardParseError::kMalformedJson);
        }
        skipWhitespace();
        if (!skipValue(depth + 1U)) {
          return false;
        }
        skipWhitespace();
        if (consume('}')) {
          return true;
        }
        if (!consume(',')) {
          return fail(DashboardParseError::kMalformedJson);
        }
        skipWhitespace();
      }
    }
    if (character == '[') {
      ++position_;
      skipWhitespace();
      if (consume(']')) {
        return true;
      }
      while (true) {
        if (!skipValue(depth + 1U)) {
          return false;
        }
        skipWhitespace();
        if (consume(']')) {
          return true;
        }
        if (!consume(',')) {
          return fail(DashboardParseError::kMalformedJson);
        }
        skipWhitespace();
      }
    }
    if (character == 't') {
      return consumeRequiredLiteral("true");
    }
    if (character == 'f') {
      return consumeRequiredLiteral("false");
    }
    if (character == 'n' && !startsWith("NaN")) {
      return consumeRequiredLiteral("null");
    }
    if (character == '-' || isDigit(character) || startsWith("NaN") ||
        startsWith("Infinity")) {
      double ignored = 0.0;
      return parseNumber(ignored);
    }
    return fail(DashboardParseError::kMalformedJson);
  }

  bool consumeRequiredLiteral(const char* literal) {
    if (!consumeLiteral(literal)) {
      return fail(DashboardParseError::kMalformedJson);
    }
    return true;
  }

  bool markSeen(uint8_t bit, uint8_t& seen) {
    if ((seen & bit) != 0) {
      return fail(DashboardParseError::kDuplicateField);
    }
    seen |= bit;
    return true;
  }

  static bool keyEquals(const DecodedString& key, const char* expected) {
    return !key.truncated && bytesEqual(key.bytes, key.length, expected);
  }

  static bool isDigit(char character) {
    return character >= '0' && character <= '9';
  }

  static int hexValue(char character) {
    if (character >= '0' && character <= '9') {
      return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
      return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
      return character - 'A' + 10;
    }
    return -1;
  }

  bool startsWith(const char* literal) const {
    const size_t length = std::strlen(literal);
    return position_ + length <= inputLength_ &&
           std::memcmp(input_ + position_, literal, length) == 0;
  }

  bool consumeLiteral(const char* literal) {
    if (!startsWith(literal)) {
      return false;
    }
    position_ += std::strlen(literal);
    return true;
  }

  void skipWhitespace() {
    while (position_ < inputLength_) {
      const char character = input_[position_];
      if (character != ' ' && character != '\t' && character != '\n' &&
          character != '\r') {
        break;
      }
      ++position_;
    }
  }

  bool consume(char expected) {
    if (position_ >= inputLength_ || input_[position_] != expected) {
      return false;
    }
    ++position_;
    return true;
  }

  bool fail(DashboardParseError error) {
    if (error_ == DashboardParseError::kNone) {
      error_ = error;
    }
    return false;
  }

  const char* input_;
  size_t inputLength_;
  size_t position_ = 0;
  DashboardParseError error_ = DashboardParseError::kNone;
};

}  // namespace

EnvironmentParseResult parseEnvironmentReading(const char* json,
                                               size_t jsonLength) {
  DashboardJsonParser parser(json, jsonLength);
  return parser.parseEnvironment();
}

WeatherParseResult parseWeatherReading(const char* json, size_t jsonLength) {
  DashboardJsonParser parser(json, jsonLength);
  return parser.parseWeather();
}

const char* dashboardParseErrorName(DashboardParseError error) {
  switch (error) {
    case DashboardParseError::kNone:
      return "none";
    case DashboardParseError::kNullArgument:
      return "null_argument";
    case DashboardParseError::kTopLevelNotObject:
      return "top_level_not_object";
    case DashboardParseError::kMalformedJson:
      return "malformed_json";
    case DashboardParseError::kInvalidUtf8:
      return "invalid_utf8";
    case DashboardParseError::kMissingField:
      return "missing_field";
    case DashboardParseError::kDuplicateField:
      return "duplicate_field";
    case DashboardParseError::kWrongType:
      return "wrong_type";
    case DashboardParseError::kNonFiniteNumber:
      return "non_finite_number";
    case DashboardParseError::kNumberOverflow:
      return "number_overflow";
    case DashboardParseError::kValueOutOfRange:
      return "value_out_of_range";
    case DashboardParseError::kStringTooLong:
      return "string_too_long";
    case DashboardParseError::kInvalidString:
      return "invalid_string";
    case DashboardParseError::kInvalidStatus:
      return "invalid_status";
    case DashboardParseError::kTrailingData:
      return "trailing_data";
    case DashboardParseError::kNestingTooDeep:
      return "nesting_too_deep";
  }
  return "unknown";
}
