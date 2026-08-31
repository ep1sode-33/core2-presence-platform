#include <cassert>
#include <cstdio>
#include <cstring>

#include "ota_control_validation.h"

namespace {

void digestHex(const uint8_t* digest, char* output) {
  for (size_t index = 0; index < kOtaSha256Size; ++index) {
    std::snprintf(output + index * 2, 3, "%02x", digest[index]);
  }
}

}  // namespace

int main() {
  OtaManifest manifest = {};
  std::strcpy(manifest.hardware, "m5go-classic-esp32-16m");
  std::strcpy(manifest.firmwareVersion, "0.7.0");
  std::strcpy(manifest.buildId, "git.0123456789ab");
  std::strcpy(manifest.signingKeyId, "release-2026-a");
  manifest.releaseCounter = 7;
  manifest.firmwareSize = 123456;
  for (size_t index = 0; index < kOtaSha256Size; ++index) {
    manifest.firmwareSha256[index] = static_cast<uint8_t>(index);
    manifest.elfSha256[index] = static_cast<uint8_t>(255 - index);
  }

  DesiredFirmwareRelease desired = {};
  std::strcpy(desired.releaseId,
              "rel-0123456789abcdef0123456789abcdef");
  std::strcpy(desired.hardwareModel, manifest.hardware);
  std::strcpy(desired.firmwareVersion, manifest.firmwareVersion);
  std::strcpy(desired.buildId, manifest.buildId);
  std::strcpy(desired.signingKeyId, manifest.signingKeyId);
  desired.releaseCounter = manifest.releaseCounter;
  desired.imageSize = manifest.firmwareSize;
  digestHex(manifest.firmwareSha256, desired.imageSha256);
  digestHex(manifest.elfSha256, desired.elfSha256);
  assert(validateOtaControlClaims(desired, manifest) ==
         OtaControlValidationError::kNone);

  desired.imageSize++;
  assert(validateOtaControlClaims(desired, manifest) ==
         OtaControlValidationError::kImageSize);
  desired.imageSize--;
  desired.imageSha256[0] = 'f';
  assert(validateOtaControlClaims(desired, manifest) ==
         OtaControlValidationError::kImageDigest);
}
