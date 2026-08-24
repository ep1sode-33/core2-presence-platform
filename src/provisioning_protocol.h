#pragma once

#include <cstddef>
#include <cstdint>

#include "device_settings.h"

enum class Base64UrlError : uint8_t {
  kOk,
  kNullArgument,
  kInvalidLength,
  kInvalidCharacter,
  kNonCanonicalTrailingBits,
  kOutputTooSmall,
};

enum class ProvisioningError : uint8_t {
  kOk,
  kNullArgument,
  kInvalidCommand,
  kInvalidFieldCount,
  kInvalidChallenge,
  kInvalidBase64Url,
  kDecodedFieldTooLong,
  kEmbeddedNul,
  kInvalidSsid,
  kInvalidBaseUrl,
  kInvalidToken,
  kInvalidSettings,
};

// The encoder writes an unpadded base64url string and a trailing NUL. The
// output capacity must therefore be at least encoded_length + 1.
Base64UrlError encodeBase64UrlNoPadding(const uint8_t* input,
                                        size_t inputLength, char* output,
                                        size_t outputCapacity,
                                        size_t* outputLength);

// The decoder writes raw bytes and never appends a NUL.
Base64UrlError decodeBase64UrlNoPadding(const char* input,
                                        size_t inputLength, uint8_t* output,
                                        size_t outputCapacity,
                                        size_t* outputLength);

// Validates all fixed buffers and canonicalizes baseUrl by removing trailing
// slashes. SSID and token must be non-empty; an empty password is allowed.
// This firmware currently supports LAN HTTP only.
ProvisioningError normalizeAndValidateDeviceSettings(
    DeviceSettings* settings);

// Strictly parses:
// PROVISION,SET,<8 lowercase hex challenge>,<ssid>,<password>,<base_url>,<token>
// The last four fields are unpadded base64url. output is unchanged on failure.
ProvisioningError parseProvisioningSetCommand(
    const char* command, size_t commandLength, const char* expectedChallenge,
    size_t expectedChallengeLength, DeviceSettings* output);
