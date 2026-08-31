#include "ota_control_validation.h"

#include <cstring>

namespace {

bool validReleaseId(const char* value) {
  if (value == nullptr || std::strlen(value) != 36 ||
      std::memcmp(value, "rel-", 4) != 0) {
    return false;
  }
  for (size_t index = 4; index < 36; ++index) {
    if (!((value[index] >= '0' && value[index] <= '9') ||
          (value[index] >= 'a' && value[index] <= 'f'))) {
      return false;
    }
  }
  return true;
}
bool digestMatches(const char* hexadecimal,
                   const uint8_t digest[kOtaSha256Size]) {
  if (hexadecimal == nullptr || digest == nullptr ||
      std::strlen(hexadecimal) != kOtaSha256Size * 2U) {
    return false;
  }
  for (size_t index = 0; index < kOtaSha256Size; ++index) {
    const char high = hexadecimal[index * 2U];
    const char low = hexadecimal[index * 2U + 1U];
    const auto nibble = [](char value) -> int {
      if (value >= '0' && value <= '9') return value - '0';
      if (value >= 'a' && value <= 'f') return value - 'a' + 10;
      return -1;
    };
    const int highValue = nibble(high);
    const int lowValue = nibble(low);
    if (highValue < 0 || lowValue < 0 ||
        digest[index] != static_cast<uint8_t>((highValue << 4) | lowValue)) {
      return false;
    }
  }
  return true;
}

}  // namespace

OtaControlValidationError validateOtaControlClaims(
    const DesiredFirmwareRelease& desired, const OtaManifest& manifest) {
  if (!validReleaseId(desired.releaseId)) {
    return OtaControlValidationError::kReleaseId;
  }
  if (std::strcmp(desired.hardwareModel, manifest.hardware) != 0) {
    return OtaControlValidationError::kHardware;
  }
  if (std::strcmp(desired.firmwareVersion, manifest.firmwareVersion) != 0) {
    return OtaControlValidationError::kFirmwareVersion;
  }
  if (desired.releaseCounter != manifest.releaseCounter) {
    return OtaControlValidationError::kReleaseCounter;
  }
  if (std::strcmp(desired.buildId, manifest.buildId) != 0) {
    return OtaControlValidationError::kBuildId;
  }
  if (desired.imageSize != manifest.firmwareSize) {
    return OtaControlValidationError::kImageSize;
  }
  if (!digestMatches(desired.imageSha256, manifest.firmwareSha256)) {
    return OtaControlValidationError::kImageDigest;
  }
  if (!digestMatches(desired.elfSha256, manifest.elfSha256)) {
    return OtaControlValidationError::kElfDigest;
  }
  if (std::strcmp(desired.signingKeyId, manifest.signingKeyId) != 0) {
    return OtaControlValidationError::kSigningKeyId;
  }
  return OtaControlValidationError::kNone;
}

const char* otaControlValidationErrorName(OtaControlValidationError error) {
  switch (error) {
    case OtaControlValidationError::kNone:
      return "none";
    case OtaControlValidationError::kReleaseId:
      return "release_id";
    case OtaControlValidationError::kHardware:
      return "hardware";
    case OtaControlValidationError::kFirmwareVersion:
      return "firmware_version";
    case OtaControlValidationError::kReleaseCounter:
      return "release_counter";
    case OtaControlValidationError::kBuildId:
      return "build_id";
    case OtaControlValidationError::kImageSize:
      return "image_size";
    case OtaControlValidationError::kImageDigest:
      return "image_digest";
    case OtaControlValidationError::kElfDigest:
      return "elf_digest";
    case OtaControlValidationError::kSigningKeyId:
      return "signing_key_id";
  }
  return "unknown";
}
