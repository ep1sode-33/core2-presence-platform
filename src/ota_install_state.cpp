#include "ota_install_state.h"

#include <cstdio>
#include <cstring>

#if defined(ARDUINO_ARCH_ESP32)
#include <nvs.h>
#endif

namespace {

constexpr uint8_t kMagic[] = {'M', '5', 'O', 'S'};
constexpr uint16_t kFormatVersion = 3;
constexpr uint16_t kV2FormatVersion = 2;
constexpr uint16_t kV2PayloadLength = 347;
constexpr uint16_t kPayloadLength = 396;
constexpr size_t kFlagsOffset = 8;
constexpr size_t kConfirmedCounterOffset = 9;
constexpr size_t kPendingCounterOffset = 17;
constexpr size_t kConfirmedReleaseLengthOffset = 25;
constexpr size_t kConfirmedReleaseOffset = 26;
constexpr size_t kConfirmedVersionLengthOffset = 74;
constexpr size_t kConfirmedVersionOffset = 75;
constexpr size_t kConfirmedBuildLengthOffset = 107;
constexpr size_t kConfirmedBuildOffset = 108;
constexpr size_t kPendingReleaseLengthOffset = 172;
constexpr size_t kPendingReleaseOffset = 173;
constexpr size_t kPendingVersionLengthOffset = 221;
constexpr size_t kPendingVersionOffset = 222;
constexpr size_t kPendingBuildLengthOffset = 254;
constexpr size_t kPendingBuildOffset = 255;
constexpr size_t kPendingPartitionAddressOffset = 319;
constexpr size_t kPendingImageSha256Offset = 323;
constexpr size_t kPreviousReleaseLengthOffset = 355;
constexpr size_t kPreviousReleaseOffset = 356;
constexpr size_t kChecksumOffset = 404;
constexpr size_t kV2ChecksumOffset = 355;

size_t boundedLength(const char* value, size_t capacity) {
  const void* end = std::memchr(value, '\0', capacity);
  return end == nullptr ? capacity
                        : static_cast<const char*>(end) - value;
}

bool canonicalText(const char* value, size_t capacity, bool allowEmpty,
                   bool allowColon) {
  const size_t length = boundedLength(value, capacity);
  if (length >= capacity || (!allowEmpty && length == 0)) {
    return false;
  }
  for (size_t index = 0; index < length; ++index) {
    const char byte = value[index];
    if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
        (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' ||
        byte == '+' || byte == '-' || (allowColon && byte == ':')) {
      continue;
    }
    return false;
  }
  return true;
}

bool releaseId(const char* value, bool allowEmpty) {
  const size_t length = boundedLength(value, 49);
  if (allowEmpty && length == 0) {
    return true;
  }
  if (length != 36 || std::memcmp(value, "rel-", 4) != 0) {
    return false;
  }
  for (size_t index = 4; index < length; ++index) {
    if (!((value[index] >= '0' && value[index] <= '9') ||
          (value[index] >= 'a' && value[index] <= 'f'))) {
      return false;
    }
  }
  return true;
}

void putU16(uint8_t* output, uint16_t value) {
  output[0] = static_cast<uint8_t>(value);
  output[1] = static_cast<uint8_t>(value >> 8U);
}

void putU32(uint8_t* output, uint32_t value) {
  for (size_t index = 0; index < 4; ++index) {
    output[index] = static_cast<uint8_t>(value >> (index * 8U));
  }
}

void putU64(uint8_t* output, uint64_t value) {
  for (size_t index = 0; index < 8; ++index) {
    output[index] = static_cast<uint8_t>(value >> (index * 8U));
  }
}

uint16_t getU16(const uint8_t* input) {
  return static_cast<uint16_t>(input[0]) |
         static_cast<uint16_t>(input[1]) << 8U;
}

uint32_t getU32(const uint8_t* input) {
  uint32_t value = 0;
  for (size_t index = 0; index < 4; ++index) {
    value |= static_cast<uint32_t>(input[index]) << (index * 8U);
  }
  return value;
}

uint64_t getU64(const uint8_t* input) {
  uint64_t value = 0;
  for (size_t index = 0; index < 8; ++index) {
    value |= static_cast<uint64_t>(input[index]) << (index * 8U);
  }
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

template <size_t Capacity>
void writeText(uint8_t* blob, size_t lengthOffset, size_t textOffset,
               const char (&value)[Capacity]) {
  const size_t length = std::strlen(value);
  blob[lengthOffset] = static_cast<uint8_t>(length);
  std::memcpy(blob + textOffset, value, length);
}

template <size_t Capacity>
bool readText(const uint8_t* blob, size_t lengthOffset, size_t textOffset,
              char (&output)[Capacity]) {
  const size_t length = blob[lengthOffset];
  if (length >= Capacity) {
    return false;
  }
  std::memcpy(output, blob + textOffset, length);
  output[length] = '\0';
  return true;
}

void clearPending(OtaInstallState* state) {
  state->pending = false;
  state->developmentPending = false;
  state->pendingImageAccepted = false;
  state->pendingValidated = false;
  state->pendingPartitionAddress = 0;
  std::memset(state->pendingImageSha256, 0,
              sizeof(state->pendingImageSha256));
  state->pendingReleaseCounter = 0;
  std::memset(state->pendingReleaseId, 0, sizeof(state->pendingReleaseId));
  std::memset(state->pendingFirmwareVersion, 0,
              sizeof(state->pendingFirmwareVersion));
  std::memset(state->pendingBuildId, 0, sizeof(state->pendingBuildId));
}

bool digestIsZero(const uint8_t digest[kOtaSha256Size]) {
  uint8_t combined = 0;
  for (size_t index = 0; index < kOtaSha256Size; ++index) {
    combined |= digest[index];
  }
  return combined == 0;
}

bool partitionAddressIsValid(uint32_t address) {
  return address != 0 && (address & UINT32_C(0xffff)) == 0;
}

}  // namespace

bool otaInstallStateIsValid(const OtaInstallState& state) {
  if (state.confirmedReleaseCounter > kOtaMaximumReleaseCounter ||
      state.pendingReleaseCounter > kOtaMaximumReleaseCounter) {
    return false;
  }
  const bool confirmedEmpty = state.confirmedReleaseCounter == 0;
  if (!releaseId(state.confirmedReleaseId, confirmedEmpty) ||
      !releaseId(state.previousReleaseId, true) ||
      !canonicalText(state.confirmedFirmwareVersion,
                     sizeof(state.confirmedFirmwareVersion), confirmedEmpty,
                     false) ||
      !canonicalText(state.confirmedBuildId, sizeof(state.confirmedBuildId),
                     confirmedEmpty, true)) {
    return false;
  }
  if (confirmedEmpty &&
      (state.confirmedReleaseId[0] != '\0' ||
       state.previousReleaseId[0] != '\0' ||
       state.confirmedFirmwareVersion[0] != '\0' ||
       state.confirmedBuildId[0] != '\0')) {
    return false;
  }
  if (state.pending && state.developmentPending) {
    return false;
  }
  const bool imageTransaction = state.pending || state.developmentPending;
  if (!imageTransaction) {
    return !state.pendingImageAccepted && !state.pendingValidated &&
           state.pendingPartitionAddress == 0 &&
           digestIsZero(state.pendingImageSha256) &&
           state.pendingReleaseCounter == 0 &&
           state.pendingReleaseId[0] == '\0' &&
           state.pendingFirmwareVersion[0] == '\0' &&
           state.pendingBuildId[0] == '\0';
  }
  if (!partitionAddressIsValid(state.pendingPartitionAddress) ||
      (state.pendingValidated && !state.pendingImageAccepted) ||
      state.pendingImageAccepted != !digestIsZero(state.pendingImageSha256)) {
    return false;
  }
  if (state.developmentPending) {
    return state.pendingReleaseCounter == 0 &&
           state.pendingReleaseId[0] == '\0' &&
           state.pendingFirmwareVersion[0] == '\0' &&
           state.pendingBuildId[0] == '\0';
  }
  return state.pendingReleaseCounter > state.confirmedReleaseCounter &&
         releaseId(state.pendingReleaseId, false) &&
         canonicalText(state.pendingFirmwareVersion,
                       sizeof(state.pendingFirmwareVersion), false, false) &&
         canonicalText(state.pendingBuildId, sizeof(state.pendingBuildId),
                       false, true);
}

bool otaConfirmedProductionMatchesRunningImage(
    const OtaInstallState& state, const char* runningFirmwareVersion,
    const char* runningBuildId) {
  return runningFirmwareVersion != nullptr && runningBuildId != nullptr &&
         otaInstallStateIsValid(state) &&
         !state.runningDevelopmentImage &&
         state.confirmedReleaseCounter != 0 &&
         std::strcmp(state.confirmedFirmwareVersion,
                     runningFirmwareVersion) == 0 &&
         std::strcmp(state.confirmedBuildId, runningBuildId) == 0;
}

bool otaStagePendingRelease(OtaInstallState* state, const char* releaseIdValue,
                            const OtaManifest& manifest,
                            uint32_t targetPartitionAddress) {
  if (state == nullptr || releaseIdValue == nullptr ||
      !otaInstallStateIsValid(*state) ||
      manifest.releaseCounter <= state->confirmedReleaseCounter ||
      manifest.releaseCounter > kOtaMaximumReleaseCounter ||
      !partitionAddressIsValid(targetPartitionAddress) ||
      !releaseId(releaseIdValue, false) ||
      !canonicalText(manifest.firmwareVersion,
                     sizeof(manifest.firmwareVersion), false, false) ||
      !canonicalText(manifest.buildId, sizeof(manifest.buildId), false, true)) {
    return false;
  }
  OtaInstallState candidate = *state;
  clearPending(&candidate);
  candidate.pending = true;
  candidate.pendingPartitionAddress = targetPartitionAddress;
  candidate.pendingValidated = false;
  candidate.pendingReleaseCounter = manifest.releaseCounter;
  std::snprintf(candidate.pendingReleaseId,
                sizeof(candidate.pendingReleaseId), "%s", releaseIdValue);
  std::snprintf(candidate.pendingFirmwareVersion,
                sizeof(candidate.pendingFirmwareVersion), "%s",
                manifest.firmwareVersion);
  std::snprintf(candidate.pendingBuildId, sizeof(candidate.pendingBuildId),
                "%s", manifest.buildId);
  if (!otaInstallStateIsValid(candidate)) {
    return false;
  }
  *state = candidate;
  return true;
}

bool otaStagePendingDevelopmentImage(OtaInstallState* state,
                                     uint32_t targetPartitionAddress) {
  if (state == nullptr || !otaInstallStateIsValid(*state) || state->pending ||
      state->developmentPending ||
      !partitionAddressIsValid(targetPartitionAddress)) {
    return false;
  }
  OtaInstallState candidate = *state;
  clearPending(&candidate);
  candidate.developmentPending = true;
  candidate.pendingPartitionAddress = targetPartitionAddress;
  if (!otaInstallStateIsValid(candidate)) {
    return false;
  }
  *state = candidate;
  return true;
}

bool otaMarkPendingImageAccepted(
    OtaInstallState* state, uint32_t partitionAddress,
    const uint8_t imageSha256[kOtaSha256Size]) {
  if (state == nullptr || imageSha256 == nullptr ||
      !otaInstallStateIsValid(*state) ||
      !(state->pending || state->developmentPending) ||
      state->pendingImageAccepted ||
      state->pendingPartitionAddress != partitionAddress ||
      digestIsZero(imageSha256)) {
    return false;
  }
  OtaInstallState candidate = *state;
  candidate.pendingImageAccepted = true;
  std::memcpy(candidate.pendingImageSha256, imageSha256,
              sizeof(candidate.pendingImageSha256));
  if (!otaInstallStateIsValid(candidate)) {
    return false;
  }
  *state = candidate;
  return true;
}

bool otaPendingImageIdentityMatches(
    const OtaInstallState& state, uint32_t partitionAddress,
    const uint8_t imageSha256[kOtaSha256Size]) {
  return imageSha256 != nullptr && otaInstallStateIsValid(state) &&
         (state.pending || state.developmentPending) &&
         state.pendingImageAccepted &&
         state.pendingPartitionAddress == partitionAddress &&
         std::memcmp(state.pendingImageSha256, imageSha256,
                     sizeof(state.pendingImageSha256)) == 0;
}

void otaCancelPendingRelease(OtaInstallState* state) {
  if (state != nullptr && state->pending) {
    clearPending(state);
  }
}

void otaCancelPendingDevelopmentImage(OtaInstallState* state) {
  if (state != nullptr && state->developmentPending) {
    clearPending(state);
  }
}

bool otaMarkPendingValidated(OtaInstallState* state,
                             const char* runningBuildId) {
  if (state == nullptr || !otaInstallStateIsValid(*state) ||
      !(state->pending || state->developmentPending) ||
      !state->pendingImageAccepted ||
      (state->pending &&
       (runningBuildId == nullptr ||
        std::strcmp(state->pendingBuildId, runningBuildId) != 0))) {
    return false;
  }
  OtaInstallState candidate = *state;
  candidate.pendingValidated = true;
  if (!otaInstallStateIsValid(candidate)) {
    return false;
  }
  *state = candidate;
  return true;
}

bool otaConfirmPendingDevelopmentImage(OtaInstallState* state) {
  if (state == nullptr || !otaInstallStateIsValid(*state) ||
      !state->developmentPending || !state->pendingImageAccepted ||
      !state->pendingValidated) {
    return false;
  }
  OtaInstallState candidate = *state;
  clearPending(&candidate);
  candidate.runningDevelopmentImage = true;
  if (!otaInstallStateIsValid(candidate)) {
    return false;
  }
  *state = candidate;
  return true;
}

bool otaConfirmPendingRelease(OtaInstallState* state,
                              const char* runningBuildId) {
  if (state == nullptr || runningBuildId == nullptr ||
      !otaInstallStateIsValid(*state) || !state->pending ||
      !state->pendingValidated ||
      std::strcmp(state->pendingBuildId, runningBuildId) != 0) {
    return false;
  }
  OtaInstallState candidate = *state;
  candidate.confirmedReleaseCounter = candidate.pendingReleaseCounter;
  std::snprintf(candidate.previousReleaseId,
                sizeof(candidate.previousReleaseId), "%s",
                candidate.confirmedReleaseId);
  std::memcpy(candidate.confirmedReleaseId, candidate.pendingReleaseId,
              sizeof(candidate.confirmedReleaseId));
  std::memcpy(candidate.confirmedFirmwareVersion,
              candidate.pendingFirmwareVersion,
              sizeof(candidate.confirmedFirmwareVersion));
  std::memcpy(candidate.confirmedBuildId, candidate.pendingBuildId,
              sizeof(candidate.confirmedBuildId));
  candidate.runningDevelopmentImage = false;
  clearPending(&candidate);
  if (!otaInstallStateIsValid(candidate)) {
    return false;
  }
  *state = candidate;
  return true;
}

OtaInstallStateBlobError encodeOtaInstallState(
    const OtaInstallState& state, uint8_t* output, size_t outputCapacity) {
  if (output == nullptr) {
    return OtaInstallStateBlobError::kNullArgument;
  }
  if (outputCapacity < kOtaInstallStateBlobSize) {
    return OtaInstallStateBlobError::kWrongLength;
  }
  if (!otaInstallStateIsValid(state)) {
    return OtaInstallStateBlobError::kInvalidState;
  }
  uint8_t blob[kOtaInstallStateBlobSize] = {};
  std::memcpy(blob, kMagic, sizeof(kMagic));
  putU16(blob + 4, kFormatVersion);
  putU16(blob + 6, kPayloadLength);
  blob[kFlagsOffset] = (state.pending ? 1U : 0U) |
                       (state.pendingValidated ? 2U : 0U) |
                       (state.pendingImageAccepted ? 4U : 0U) |
                       (state.developmentPending ? 8U : 0U) |
                       (state.runningDevelopmentImage ? 16U : 0U);
  putU64(blob + kConfirmedCounterOffset, state.confirmedReleaseCounter);
  putU64(blob + kPendingCounterOffset, state.pendingReleaseCounter);
  writeText(blob, kConfirmedReleaseLengthOffset, kConfirmedReleaseOffset,
            state.confirmedReleaseId);
  writeText(blob, kConfirmedVersionLengthOffset, kConfirmedVersionOffset,
            state.confirmedFirmwareVersion);
  writeText(blob, kConfirmedBuildLengthOffset, kConfirmedBuildOffset,
            state.confirmedBuildId);
  writeText(blob, kPendingReleaseLengthOffset, kPendingReleaseOffset,
            state.pendingReleaseId);
  writeText(blob, kPendingVersionLengthOffset, kPendingVersionOffset,
            state.pendingFirmwareVersion);
  writeText(blob, kPendingBuildLengthOffset, kPendingBuildOffset,
            state.pendingBuildId);
  putU32(blob + kPendingPartitionAddressOffset,
         state.pendingPartitionAddress);
  std::memcpy(blob + kPendingImageSha256Offset, state.pendingImageSha256,
              sizeof(state.pendingImageSha256));
  writeText(blob, kPreviousReleaseLengthOffset, kPreviousReleaseOffset,
            state.previousReleaseId);
  putU32(blob + kChecksumOffset, crc32(blob, kChecksumOffset));
  std::memcpy(output, blob, sizeof(blob));
  return OtaInstallStateBlobError::kNone;
}

OtaInstallStateBlobError decodeOtaInstallState(
    const uint8_t* input, size_t inputLength, OtaInstallState* output) {
  if (input == nullptr || output == nullptr) {
    return OtaInstallStateBlobError::kNullArgument;
  }
  if (inputLength != kOtaInstallStateV2BlobSize &&
      inputLength != kOtaInstallStateBlobSize) {
    return OtaInstallStateBlobError::kWrongLength;
  }
  if (std::memcmp(input, kMagic, sizeof(kMagic)) != 0) {
    return OtaInstallStateBlobError::kBadMagic;
  }
  const uint16_t formatVersion = getU16(input + 4);
  const bool v2 = formatVersion == kV2FormatVersion;
  if (!v2 && formatVersion != kFormatVersion) {
    return OtaInstallStateBlobError::kUnsupportedVersion;
  }
  if ((v2 && inputLength != kOtaInstallStateV2BlobSize) ||
      (!v2 && inputLength != kOtaInstallStateBlobSize) ||
      getU16(input + 6) != (v2 ? kV2PayloadLength : kPayloadLength)) {
    return OtaInstallStateBlobError::kBadPayloadLength;
  }
  const size_t checksumOffset = v2 ? kV2ChecksumOffset : kChecksumOffset;
  if (getU32(input + checksumOffset) != crc32(input, checksumOffset)) {
    return OtaInstallStateBlobError::kChecksumMismatch;
  }
  if ((input[kFlagsOffset] & (v2 ? ~15U : ~31U)) != 0) {
    return OtaInstallStateBlobError::kInvalidState;
  }
  OtaInstallState candidate = {};
  candidate.pending = (input[kFlagsOffset] & 1U) != 0;
  candidate.pendingValidated = (input[kFlagsOffset] & 2U) != 0;
  candidate.pendingImageAccepted = (input[kFlagsOffset] & 4U) != 0;
  candidate.developmentPending = (input[kFlagsOffset] & 8U) != 0;
  candidate.runningDevelopmentImage =
      !v2 && (input[kFlagsOffset] & 16U) != 0;
  candidate.confirmedReleaseCounter = getU64(input + kConfirmedCounterOffset);
  candidate.pendingReleaseCounter = getU64(input + kPendingCounterOffset);
  if (!readText(input, kConfirmedReleaseLengthOffset,
                kConfirmedReleaseOffset, candidate.confirmedReleaseId) ||
      !readText(input, kConfirmedVersionLengthOffset,
                kConfirmedVersionOffset, candidate.confirmedFirmwareVersion) ||
      !readText(input, kConfirmedBuildLengthOffset, kConfirmedBuildOffset,
                candidate.confirmedBuildId) ||
      !readText(input, kPendingReleaseLengthOffset, kPendingReleaseOffset,
                candidate.pendingReleaseId) ||
      !readText(input, kPendingVersionLengthOffset, kPendingVersionOffset,
                candidate.pendingFirmwareVersion) ||
      !readText(input, kPendingBuildLengthOffset, kPendingBuildOffset,
                candidate.pendingBuildId)) {
    return OtaInstallStateBlobError::kInvalidState;
  }
  if (!v2 &&
      !readText(input, kPreviousReleaseLengthOffset, kPreviousReleaseOffset,
                candidate.previousReleaseId)) {
    return OtaInstallStateBlobError::kInvalidState;
  }
  candidate.pendingPartitionAddress =
      getU32(input + kPendingPartitionAddressOffset);
  std::memcpy(candidate.pendingImageSha256,
              input + kPendingImageSha256Offset,
              sizeof(candidate.pendingImageSha256));
  if (!otaInstallStateIsValid(candidate)) {
    return OtaInstallStateBlobError::kInvalidState;
  }
  *output = candidate;
  return OtaInstallStateBlobError::kNone;
}

OtaInstallStateStorageResult loadOtaInstallState(OtaInstallState* output) {
  if (output == nullptr) {
    return OtaInstallStateStorageResult::kInvalidState;
  }
  // Never leave a caller-visible state object carrying stale data when the
  // namespace has not been created yet or a later read fails.
  *output = {};
#if defined(ARDUINO_ARCH_ESP32)
  nvs_handle_t handle = 0;
  if (nvs_open("m5ota", NVS_READWRITE, &handle) != ESP_OK) {
    return OtaInstallStateStorageResult::kOpenFailed;
  }
  size_t length = 0;
  const esp_err_t sizeResult = nvs_get_blob(handle, "state", nullptr, &length);
  if (sizeResult == ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    return OtaInstallStateStorageResult::kNotStored;
  }
  if (sizeResult != ESP_OK) {
    nvs_close(handle);
    return OtaInstallStateStorageResult::kReadFailed;
  }
  if (length != kOtaInstallStateV2BlobSize &&
      length != kOtaInstallStateBlobSize) {
    nvs_close(handle);
    return OtaInstallStateStorageResult::kInvalidStoredData;
  }
  uint8_t blob[kOtaInstallStateBlobSize] = {};
  size_t read = length;
  const esp_err_t readResult = nvs_get_blob(handle, "state", blob, &read);
  nvs_close(handle);
  if (readResult != ESP_OK || read != length) {
    return OtaInstallStateStorageResult::kReadFailed;
  }
  OtaInstallState decoded = {};
  if (decodeOtaInstallState(blob, length, &decoded) !=
      OtaInstallStateBlobError::kNone) {
    return OtaInstallStateStorageResult::kInvalidStoredData;
  }
  // A v2 journal has no development-running or durable-previous fields.
  // Rewrite the valid decoded state as v3 before treating it as boot evidence,
  // so every later status report has one stable durable representation.
  if (length == kOtaInstallStateV2BlobSize) {
    const OtaInstallStateStorageResult migration = saveOtaInstallState(decoded);
    if (migration != OtaInstallStateStorageResult::kOk) {
      return migration;
    }
  }
  *output = decoded;
  return OtaInstallStateStorageResult::kOk;
#else
  return OtaInstallStateStorageResult::kUnsupportedPlatform;
#endif
}

OtaInstallStateStorageResult saveOtaInstallState(
    const OtaInstallState& state) {
  uint8_t blob[kOtaInstallStateBlobSize] = {};
  if (encodeOtaInstallState(state, blob, sizeof(blob)) !=
      OtaInstallStateBlobError::kNone) {
    return OtaInstallStateStorageResult::kInvalidState;
  }
#if defined(ARDUINO_ARCH_ESP32)
  nvs_handle_t handle = 0;
  if (nvs_open("m5ota", NVS_READWRITE, &handle) != ESP_OK) {
    return OtaInstallStateStorageResult::kOpenFailed;
  }
  if (nvs_set_blob(handle, "state", blob, sizeof(blob)) != ESP_OK ||
      nvs_commit(handle) != ESP_OK) {
    nvs_close(handle);
    return OtaInstallStateStorageResult::kWriteFailed;
  }
  uint8_t verified[kOtaInstallStateBlobSize] = {};
  size_t read = sizeof(verified);
  const esp_err_t readResult =
      nvs_get_blob(handle, "state", verified, &read);
  nvs_close(handle);
  if (readResult != ESP_OK || read != sizeof(verified) ||
      std::memcmp(blob, verified, sizeof(blob)) != 0) {
    return OtaInstallStateStorageResult::kVerifyFailed;
  }
  return OtaInstallStateStorageResult::kOk;
#else
  return OtaInstallStateStorageResult::kUnsupportedPlatform;
#endif
}
