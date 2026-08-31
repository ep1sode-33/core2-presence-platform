#pragma once

#include <cstdint>

#include "control_protocol.h"
#include "ota_manifest.h"

enum class OtaControlValidationError : uint8_t {
  kNone = 0,
  kReleaseId,
  kHardware,
  kFirmwareVersion,
  kReleaseCounter,
  kBuildId,
  kImageSize,
  kImageDigest,
  kElfDigest,
  kSigningKeyId,
};

OtaControlValidationError validateOtaControlClaims(
    const DesiredFirmwareRelease& desired, const OtaManifest& manifest);
const char* otaControlValidationErrorName(OtaControlValidationError error);
