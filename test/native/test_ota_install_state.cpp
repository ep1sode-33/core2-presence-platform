#include <cassert>
#include <cstring>

#include "ota_install_state.h"

namespace {

OtaManifest manifest(uint64_t counter, const char* build) {
  OtaManifest value = {};
  value.releaseCounter = counter;
  std::strcpy(value.firmwareVersion, "0.7.0");
  std::strcpy(value.buildId, build);
  return value;
}

uint32_t crc32(const uint8_t* bytes, size_t length) {
  uint32_t crc = 0xffffffffU;
  for (size_t index = 0; index < length; ++index) {
    crc ^= bytes[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      const uint32_t mask = static_cast<uint32_t>(0U - (crc & 1U));
      crc = (crc >> 1U) ^ (0xedb88320U & mask);
    }
  }
  return crc ^ 0xffffffffU;
}

void putU32(uint8_t* output, uint32_t value) {
  for (size_t index = 0; index < 4; ++index) {
    output[index] = static_cast<uint8_t>(value >> (index * 8U));
  }
}

void makeV2Blob(const OtaInstallState& state,
                uint8_t output[kOtaInstallStateV2BlobSize]) {
  uint8_t v3[kOtaInstallStateBlobSize] = {};
  assert(encodeOtaInstallState(state, v3, sizeof(v3)) ==
         OtaInstallStateBlobError::kNone);
  std::memcpy(output, v3, 355);
  output[4] = 2;
  output[5] = 0;
  output[6] = 347 & 0xffU;
  output[7] = 347 >> 8U;
  output[8] &= ~16U;
  putU32(output + 355, crc32(output, 355));
}

}  // namespace

int main() {
  uint8_t digest[kOtaSha256Size] = {};
  digest[0] = 0xa5;
  OtaInstallState state = {};
  assert(otaInstallStateIsValid(state));
  assert(otaStagePendingRelease(
      &state, "rel-0123456789abcdef0123456789abcdef",
      manifest(7, "git.0123456789ab"), UINT32_C(0x190000)));
  assert(state.pending);
  assert(!state.pendingImageAccepted);
  assert(!state.pendingValidated);
  assert(state.pendingReleaseCounter == 7);

  uint8_t blob[kOtaInstallStateBlobSize] = {};
  assert(encodeOtaInstallState(state, blob, sizeof(blob)) ==
         OtaInstallStateBlobError::kNone);
  OtaInstallState decoded = {};
  assert(decodeOtaInstallState(blob, sizeof(blob), &decoded) ==
         OtaInstallStateBlobError::kNone);
  assert(std::strcmp(decoded.pendingBuildId, "git.0123456789ab") == 0);
  assert(!decoded.pendingValidated);

  uint8_t damaged[kOtaInstallStateBlobSize] = {};
  std::memcpy(damaged, blob, sizeof(damaged));
  damaged[80] ^= 1;
  assert(decodeOtaInstallState(damaged, sizeof(damaged), &decoded) ==
         OtaInstallStateBlobError::kChecksumMismatch);

  assert(!otaMarkPendingValidated(&state, "git.0123456789ab"));
  assert(!otaMarkPendingImageAccepted(&state, UINT32_C(0x1a0000), digest));
  assert(otaMarkPendingImageAccepted(&state, UINT32_C(0x190000), digest));
  assert(state.pendingImageAccepted);
  assert(otaPendingImageIdentityMatches(state, UINT32_C(0x190000), digest));
  assert(!otaConfirmPendingRelease(&state, "wrong-build"));
  assert(!otaConfirmPendingRelease(&state, "git.0123456789ab"));
  assert(!otaMarkPendingValidated(&state, "wrong-build"));
  assert(otaMarkPendingValidated(&state, "git.0123456789ab"));
  assert(state.pendingValidated);

  // The prepared bit is durable and retains the exact pending identity, so a
  // reboot after bootloader confirmation can finish this promotion.
  assert(encodeOtaInstallState(state, blob, sizeof(blob)) ==
         OtaInstallStateBlobError::kNone);
  assert(decodeOtaInstallState(blob, sizeof(blob), &decoded) ==
         OtaInstallStateBlobError::kNone);
  assert(decoded.pendingValidated);
  state = decoded;
  assert(otaConfirmPendingRelease(&state, "git.0123456789ab"));
  assert(!state.pending);
  assert(state.confirmedReleaseCounter == 7);
  assert(std::strcmp(state.confirmedReleaseId,
                     "rel-0123456789abcdef0123456789abcdef") == 0);
  assert(state.previousReleaseId[0] == '\0');
  assert(!state.runningDevelopmentImage);
  assert(otaConfirmedProductionMatchesRunningImage(
      state, "0.7.0", "git.0123456789ab"));
  assert(!otaConfirmedProductionMatchesRunningImage(
      state, "0.7.1", "git.0123456789ab"));
  assert(!otaStagePendingRelease(
      &state, "rel-1123456789abcdef0123456789abcdef",
      manifest(7, "git.same-counter"), UINT32_C(0x290000)));
  assert(otaStagePendingRelease(
      &state, "rel-1123456789abcdef0123456789abcdef",
      manifest(8, "git.next-build"), UINT32_C(0x290000)));
  assert(!state.pendingValidated);

  assert(otaMarkPendingImageAccepted(&state, UINT32_C(0x290000), digest));
  assert(otaMarkPendingValidated(&state, "git.next-build"));
  assert(otaConfirmPendingRelease(&state, "git.next-build"));
  assert(std::strcmp(state.previousReleaseId,
                     "rel-0123456789abcdef0123456789abcdef") == 0);
  assert(!state.runningDevelopmentImage);

  OtaInstallState development = state;
  assert(otaStagePendingDevelopmentImage(&development,
                                         UINT32_C(0x190000)));
  assert(development.developmentPending);
  assert(!development.pendingImageAccepted);
  assert(otaMarkPendingImageAccepted(&development, UINT32_C(0x190000),
                                     digest));
  assert(otaMarkPendingValidated(&development, nullptr));
  assert(otaConfirmPendingDevelopmentImage(&development));
  assert(!development.developmentPending);
  assert(development.confirmedReleaseCounter == 8);
  assert(development.runningDevelopmentImage);
  assert(!otaConfirmedProductionMatchesRunningImage(
      development, "0.7.0", "git.next-build"));
  assert(std::strcmp(development.previousReleaseId,
                     "rel-0123456789abcdef0123456789abcdef") == 0);

  // A later counter-authenticated production confirmation is the only path
  // that clears the durable development-running marker.
  assert(otaStagePendingRelease(
      &development, "rel-2123456789abcdef0123456789abcdef",
      manifest(9, "git.production-after-dev"), UINT32_C(0x290000)));
  assert(otaMarkPendingImageAccepted(&development, UINT32_C(0x290000),
                                     digest));
  assert(otaMarkPendingValidated(&development, "git.production-after-dev"));
  assert(otaConfirmPendingRelease(&development, "git.production-after-dev"));
  assert(!development.runningDevelopmentImage);
  assert(std::strcmp(development.previousReleaseId,
                     "rel-1123456789abcdef0123456789abcdef") == 0);

  // v3 preserves both lineage fields through a normal wire round-trip.
  assert(encodeOtaInstallState(development, blob, sizeof(blob)) ==
         OtaInstallStateBlobError::kNone);
  assert(decodeOtaInstallState(blob, sizeof(blob), &decoded) ==
         OtaInstallStateBlobError::kNone);
  assert(!decoded.runningDevelopmentImage);
  assert(std::strcmp(decoded.previousReleaseId,
                     "rel-1123456789abcdef0123456789abcdef") == 0);

  // Installed v2 data has neither field. It remains valid and decodes to the
  // conservative defaults before the persistence layer rewrites it as v3.
  uint8_t v2[kOtaInstallStateV2BlobSize] = {};
  makeV2Blob(state, v2);
  assert(decodeOtaInstallState(v2, sizeof(v2), &decoded) ==
         OtaInstallStateBlobError::kNone);
  assert(!decoded.runningDevelopmentImage);
  assert(decoded.previousReleaseId[0] == '\0');

  assert(otaInstallStateIsValid(state));
  assert(!state.pending);
  assert(!state.pendingValidated);
}
