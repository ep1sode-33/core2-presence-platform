#pragma once

#include <cstddef>
#include <cstdint>

#include "ota_manifest.h"

struct OtaInstallState {
  char confirmedReleaseId[49] = {};
  char confirmedFirmwareVersion[kOtaFirmwareVersionMaximumLength + 1] = {};
  char confirmedBuildId[kOtaBuildIdMaximumLength + 1] = {};
  // The immediately preceding confirmed production release. This is durable
  // because release-status retries can span a reboot after confirmation.
  char previousReleaseId[49] = {};
  char pendingReleaseId[49] = {};
  char pendingFirmwareVersion[kOtaFirmwareVersionMaximumLength + 1] = {};
  char pendingBuildId[kOtaBuildIdMaximumLength + 1] = {};
  uint64_t confirmedReleaseCounter = 0;
  uint64_t pendingReleaseCounter = 0;
  bool pending = false;
  // A physical, authenticated development upload has no release counter, but
  // it still needs the same durable image-identity and boot-confirmation
  // transaction as production OTA. Production and development transactions
  // are mutually exclusive.
  bool developmentPending = false;
  // Set only after Update.end(), the post-finalization safety gates, and an
  // exact readback of the selected boot partition all succeed. A rollback-
  // pending boot without this bit must never be confirmed.
  bool pendingImageAccepted = false;
  // Durable half of the boot-confirmation transaction. The worker writes and
  // verifies this bit before main is allowed to mark the running partition
  // valid in the bootloader. If power is lost after bootloader confirmation,
  // the next boot can safely finish promoting the pending release.
  bool pendingValidated = false;
  // A locally authenticated development image is running. The confirmed
  // production identity remains the rollback baseline, never the current
  // running release while this bit is set.
  bool runningDevelopmentImage = false;
  uint32_t pendingPartitionAddress = 0;
  uint8_t pendingImageSha256[kOtaSha256Size] = {};
};

bool otaInstallStateIsValid(const OtaInstallState& state);
// A confirmed release is only the current production release when its durable
// identity matches this image and no development image has taken over.
bool otaConfirmedProductionMatchesRunningImage(
    const OtaInstallState& state, const char* runningFirmwareVersion,
    const char* runningBuildId);
bool otaStagePendingRelease(OtaInstallState* state, const char* releaseId,
                            const OtaManifest& manifest,
                            uint32_t targetPartitionAddress);
bool otaStagePendingDevelopmentImage(OtaInstallState* state,
                                     uint32_t targetPartitionAddress);
bool otaMarkPendingImageAccepted(
    OtaInstallState* state, uint32_t partitionAddress,
    const uint8_t imageSha256[kOtaSha256Size]);
bool otaPendingImageIdentityMatches(
    const OtaInstallState& state, uint32_t partitionAddress,
    const uint8_t imageSha256[kOtaSha256Size]);
void otaCancelPendingRelease(OtaInstallState* state);
void otaCancelPendingDevelopmentImage(OtaInstallState* state);
bool otaMarkPendingValidated(OtaInstallState* state,
                             const char* runningBuildId);
bool otaConfirmPendingRelease(OtaInstallState* state,
                              const char* runningBuildId);
bool otaConfirmPendingDevelopmentImage(OtaInstallState* state);

// Version 2 was deployed with the v0.7 pre-release firmware. Keep accepting
// its exact wire shape so the first v3 boot can rewrite it without discarding
// release-counter history.
constexpr size_t kOtaInstallStateV2BlobSize = 359;
constexpr size_t kOtaInstallStateBlobSize = 408;

enum class OtaInstallStateBlobError : uint8_t {
  kNone = 0,
  kNullArgument,
  kWrongLength,
  kBadMagic,
  kUnsupportedVersion,
  kBadPayloadLength,
  kChecksumMismatch,
  kInvalidState,
};

OtaInstallStateBlobError encodeOtaInstallState(
    const OtaInstallState& state, uint8_t* output, size_t outputCapacity);
OtaInstallStateBlobError decodeOtaInstallState(
    const uint8_t* input, size_t inputLength, OtaInstallState* output);

enum class OtaInstallStateStorageResult : uint8_t {
  kOk = 0,
  kNotStored,
  kInvalidState,
  kInvalidStoredData,
  kOpenFailed,
  kReadFailed,
  kWriteFailed,
  kVerifyFailed,
  kUnsupportedPlatform,
};

OtaInstallStateStorageResult loadOtaInstallState(OtaInstallState* output);
OtaInstallStateStorageResult saveOtaInstallState(
    const OtaInstallState& state);
