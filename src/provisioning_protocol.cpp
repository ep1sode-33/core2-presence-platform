#include "provisioning_protocol.h"

#include <cstring>
#include <limits>

namespace {

constexpr char kBase64UrlAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

struct FieldSpan {
  const char* data = nullptr;
  size_t length = 0;
};

int decodeBase64UrlCharacter(char value) {
  if (value >= 'A' && value <= 'Z') {
    return value - 'A';
  }
  if (value >= 'a' && value <= 'z') {
    return value - 'a' + 26;
  }
  if (value >= '0' && value <= '9') {
    return value - '0' + 52;
  }
  if (value == '-') {
    return 62;
  }
  if (value == '_') {
    return 63;
  }
  return -1;
}

bool spanEquals(const FieldSpan& span, const char* literal,
                size_t literalLength) {
  return span.length == literalLength &&
         std::memcmp(span.data, literal, literalLength) == 0;
}

bool splitCommand(const char* command, size_t commandLength, FieldSpan* fields,
                  size_t fieldCapacity, size_t* fieldCount) {
  size_t count = 0;
  size_t start = 0;
  for (size_t index = 0; index <= commandLength; ++index) {
    if (index != commandLength && command[index] != ',') {
      continue;
    }
    if (count >= fieldCapacity) {
      return false;
    }
    fields[count++] = {command + start, index - start};
    start = index + 1;
  }
  *fieldCount = count;
  return true;
}

bool isLowerHex(char value) {
  return (value >= '0' && value <= '9') ||
         (value >= 'a' && value <= 'f');
}

bool challengeMatches(const FieldSpan& supplied, const char* expected,
                      size_t expectedLength) {
  uint8_t mismatch =
      static_cast<uint8_t>((supplied.length == 8 && expectedLength == 8) ? 0
                                                                         : 1);
  for (size_t index = 0; index < 8; ++index) {
    const char suppliedByte =
        index < supplied.length ? supplied.data[index] : '\0';
    const char expectedByte = index < expectedLength ? expected[index] : '\0';
    mismatch |= static_cast<uint8_t>(suppliedByte ^ expectedByte);
    mismatch |= static_cast<uint8_t>(isLowerHex(suppliedByte) ? 0 : 1);
    mismatch |= static_cast<uint8_t>(isLowerHex(expectedByte) ? 0 : 1);
  }
  return mismatch == 0;
}

bool boundedStringLength(const char* value, size_t capacity, size_t* length) {
  const void* terminator = std::memchr(value, '\0', capacity);
  if (terminator == nullptr) {
    return false;
  }
  *length = static_cast<const char*>(terminator) - value;
  return true;
}

bool startsWith(const char* value, size_t valueLength, const char* prefix,
                size_t prefixLength) {
  return valueLength >= prefixLength &&
         std::memcmp(value, prefix, prefixLength) == 0;
}

ProvisioningError validateAndNormalizeBaseUrl(char* value, size_t capacity) {
  size_t length = 0;
  if (!boundedStringLength(value, capacity, &length)) {
    return ProvisioningError::kInvalidSettings;
  }

  size_t schemeLength = 0;
  if (startsWith(value, length, "http://", 7)) {
    schemeLength = 7;
  } else {
    return ProvisioningError::kInvalidBaseUrl;
  }

  if (length <= schemeLength) {
    return ProvisioningError::kInvalidBaseUrl;
  }

  size_t authorityEnd = length;
  for (size_t index = schemeLength; index < length; ++index) {
    const unsigned char byte = static_cast<unsigned char>(value[index]);
    if (byte <= 0x20 || byte == 0x7f || value[index] == '?' ||
        value[index] == '#' || value[index] == '\\') {
      return ProvisioningError::kInvalidBaseUrl;
    }
    if (value[index] == '/' && authorityEnd == length) {
      authorityEnd = index;
    }
  }
  if (authorityEnd == schemeLength) {
    return ProvisioningError::kInvalidBaseUrl;
  }
  for (size_t index = schemeLength; index < authorityEnd; ++index) {
    if (value[index] == '@') {
      return ProvisioningError::kInvalidBaseUrl;
    }
  }

  while (length > schemeLength && value[length - 1] == '/') {
    --length;
  }
  if (length <= schemeLength) {
    return ProvisioningError::kInvalidBaseUrl;
  }
  value[length] = '\0';
  return ProvisioningError::kOk;
}

ProvisioningError decodeField(const FieldSpan& field, char* destination,
                              size_t maxDecodedLength) {
  size_t decodedLength = 0;
  const Base64UrlError decodeResult = decodeBase64UrlNoPadding(
      field.data, field.length, reinterpret_cast<uint8_t*>(destination),
      maxDecodedLength, &decodedLength);
  if (decodeResult == Base64UrlError::kOutputTooSmall) {
    return ProvisioningError::kDecodedFieldTooLong;
  }
  if (decodeResult != Base64UrlError::kOk) {
    return ProvisioningError::kInvalidBase64Url;
  }
  if (std::memchr(destination, '\0', decodedLength) != nullptr) {
    return ProvisioningError::kEmbeddedNul;
  }
  destination[decodedLength] = '\0';
  return ProvisioningError::kOk;
}

}  // namespace

Base64UrlError encodeBase64UrlNoPadding(const uint8_t* input,
                                        size_t inputLength, char* output,
                                        size_t outputCapacity,
                                        size_t* outputLength) {
  if (outputLength == nullptr || output == nullptr ||
      (inputLength != 0 && input == nullptr)) {
    return Base64UrlError::kNullArgument;
  }
  if (inputLength > (std::numeric_limits<size_t>::max() - 2) / 4 * 3) {
    return Base64UrlError::kOutputTooSmall;
  }
  const size_t encodedLength =
      (inputLength / 3) * 4 + (inputLength % 3 == 0 ? 0 : inputLength % 3 + 1);
  if (outputCapacity <= encodedLength) {
    return Base64UrlError::kOutputTooSmall;
  }

  size_t inputIndex = 0;
  size_t outputIndex = 0;
  while (inputLength - inputIndex >= 3) {
    const uint32_t value =
        (static_cast<uint32_t>(input[inputIndex]) << 16) |
        (static_cast<uint32_t>(input[inputIndex + 1]) << 8) |
        static_cast<uint32_t>(input[inputIndex + 2]);
    output[outputIndex++] = kBase64UrlAlphabet[(value >> 18) & 0x3f];
    output[outputIndex++] = kBase64UrlAlphabet[(value >> 12) & 0x3f];
    output[outputIndex++] = kBase64UrlAlphabet[(value >> 6) & 0x3f];
    output[outputIndex++] = kBase64UrlAlphabet[value & 0x3f];
    inputIndex += 3;
  }

  const size_t remaining = inputLength - inputIndex;
  if (remaining == 1) {
    const uint32_t value = static_cast<uint32_t>(input[inputIndex]) << 16;
    output[outputIndex++] = kBase64UrlAlphabet[(value >> 18) & 0x3f];
    output[outputIndex++] = kBase64UrlAlphabet[(value >> 12) & 0x3f];
  } else if (remaining == 2) {
    const uint32_t value =
        (static_cast<uint32_t>(input[inputIndex]) << 16) |
        (static_cast<uint32_t>(input[inputIndex + 1]) << 8);
    output[outputIndex++] = kBase64UrlAlphabet[(value >> 18) & 0x3f];
    output[outputIndex++] = kBase64UrlAlphabet[(value >> 12) & 0x3f];
    output[outputIndex++] = kBase64UrlAlphabet[(value >> 6) & 0x3f];
  }

  output[outputIndex] = '\0';
  *outputLength = outputIndex;
  return Base64UrlError::kOk;
}

Base64UrlError decodeBase64UrlNoPadding(const char* input,
                                        size_t inputLength, uint8_t* output,
                                        size_t outputCapacity,
                                        size_t* outputLength) {
  if (outputLength == nullptr || (inputLength != 0 && input == nullptr)) {
    return Base64UrlError::kNullArgument;
  }
  if (inputLength % 4 == 1) {
    return Base64UrlError::kInvalidLength;
  }

  const size_t decodedLength =
      (inputLength / 4) * 3 +
      (inputLength % 4 == 2 ? 1 : inputLength % 4 == 3 ? 2 : 0);
  if (decodedLength > outputCapacity) {
    return Base64UrlError::kOutputTooSmall;
  }
  if (decodedLength != 0 && output == nullptr) {
    return Base64UrlError::kNullArgument;
  }

  size_t inputIndex = 0;
  size_t outputIndex = 0;
  while (inputLength - inputIndex >= 4) {
    const int a = decodeBase64UrlCharacter(input[inputIndex]);
    const int b = decodeBase64UrlCharacter(input[inputIndex + 1]);
    const int c = decodeBase64UrlCharacter(input[inputIndex + 2]);
    const int d = decodeBase64UrlCharacter(input[inputIndex + 3]);
    if (a < 0 || b < 0 || c < 0 || d < 0) {
      return Base64UrlError::kInvalidCharacter;
    }
    output[outputIndex++] = static_cast<uint8_t>((a << 2) | (b >> 4));
    output[outputIndex++] = static_cast<uint8_t>((b << 4) | (c >> 2));
    output[outputIndex++] = static_cast<uint8_t>((c << 6) | d);
    inputIndex += 4;
  }

  const size_t remaining = inputLength - inputIndex;
  if (remaining == 2) {
    const int a = decodeBase64UrlCharacter(input[inputIndex]);
    const int b = decodeBase64UrlCharacter(input[inputIndex + 1]);
    if (a < 0 || b < 0) {
      return Base64UrlError::kInvalidCharacter;
    }
    if ((b & 0x0f) != 0) {
      return Base64UrlError::kNonCanonicalTrailingBits;
    }
    output[outputIndex++] = static_cast<uint8_t>((a << 2) | (b >> 4));
  } else if (remaining == 3) {
    const int a = decodeBase64UrlCharacter(input[inputIndex]);
    const int b = decodeBase64UrlCharacter(input[inputIndex + 1]);
    const int c = decodeBase64UrlCharacter(input[inputIndex + 2]);
    if (a < 0 || b < 0 || c < 0) {
      return Base64UrlError::kInvalidCharacter;
    }
    if ((c & 0x03) != 0) {
      return Base64UrlError::kNonCanonicalTrailingBits;
    }
    output[outputIndex++] = static_cast<uint8_t>((a << 2) | (b >> 4));
    output[outputIndex++] = static_cast<uint8_t>((b << 4) | (c >> 2));
  }

  *outputLength = outputIndex;
  return Base64UrlError::kOk;
}

ProvisioningError normalizeAndValidateDeviceSettings(
    DeviceSettings* settings) {
  if (settings == nullptr) {
    return ProvisioningError::kNullArgument;
  }

  size_t ssidLength = 0;
  size_t passwordLength = 0;
  size_t tokenLength = 0;
  if (!boundedStringLength(settings->ssid, sizeof(settings->ssid),
                           &ssidLength) ||
      !boundedStringLength(settings->password, sizeof(settings->password),
                           &passwordLength) ||
      !boundedStringLength(settings->token, sizeof(settings->token),
                           &tokenLength)) {
    return ProvisioningError::kInvalidSettings;
  }
  if (ssidLength == 0 || ssidLength > DeviceSettings::kMaxSsidBytes) {
    return ProvisioningError::kInvalidSsid;
  }
  if (passwordLength > DeviceSettings::kMaxPasswordBytes) {
    return ProvisioningError::kInvalidSettings;
  }
  if (tokenLength == 0 || tokenLength > DeviceSettings::kMaxTokenBytes) {
    return ProvisioningError::kInvalidToken;
  }
  return validateAndNormalizeBaseUrl(settings->baseUrl,
                                     sizeof(settings->baseUrl));
}

ProvisioningError parseProvisioningSetCommand(
    const char* command, size_t commandLength, const char* expectedChallenge,
    size_t expectedChallengeLength, DeviceSettings* output) {
  if (command == nullptr || expectedChallenge == nullptr || output == nullptr) {
    return ProvisioningError::kNullArgument;
  }
  if (std::memchr(command, '\0', commandLength) != nullptr) {
    return ProvisioningError::kEmbeddedNul;
  }

  FieldSpan fields[7];
  size_t fieldCount = 0;
  if (!splitCommand(command, commandLength, fields, 7, &fieldCount) ||
      fieldCount != 7) {
    return ProvisioningError::kInvalidFieldCount;
  }
  if (!spanEquals(fields[0], "PROVISION", 9) ||
      !spanEquals(fields[1], "SET", 3)) {
    return ProvisioningError::kInvalidCommand;
  }
  if (!challengeMatches(fields[2], expectedChallenge,
                        expectedChallengeLength)) {
    return ProvisioningError::kInvalidChallenge;
  }

  DeviceSettings candidate;
  ProvisioningError result =
      decodeField(fields[3], candidate.ssid, DeviceSettings::kMaxSsidBytes);
  if (result != ProvisioningError::kOk) {
    return result;
  }
  result = decodeField(fields[4], candidate.password,
                       DeviceSettings::kMaxPasswordBytes);
  if (result != ProvisioningError::kOk) {
    return result;
  }
  result = decodeField(fields[5], candidate.baseUrl,
                       DeviceSettings::kMaxBaseUrlBytes);
  if (result != ProvisioningError::kOk) {
    return result;
  }
  result =
      decodeField(fields[6], candidate.token, DeviceSettings::kMaxTokenBytes);
  if (result != ProvisioningError::kOk) {
    return result;
  }

  result = normalizeAndValidateDeviceSettings(&candidate);
  if (result != ProvisioningError::kOk) {
    return result;
  }
  *output = candidate;
  return ProvisioningError::kOk;
}
