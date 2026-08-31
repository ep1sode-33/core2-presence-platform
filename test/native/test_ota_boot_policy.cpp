#include <cassert>
#include <cstring>

#include "ota_boot_policy.h"

namespace {

OtaManifest manifest() {
  OtaManifest value = {};
  value.releaseCounter = 1;
  std::strcpy(value.firmwareVersion, "0.7.0");
  std::strcpy(value.buildId, "git.0123456789ab");
  return value;
}

OtaRunningImageInfo image(OtaRunningImageState state, uint32_t address,
                          uint8_t digestByte) {
  OtaRunningImageInfo value = {};
  value.state = state;
  value.identity.partitionAddress = address;
  value.identity.sha256[0] = digestByte;
  return value;
}

}  // namespace

int main() {
  const auto ordinaryValid =
      image(OtaRunningImageState::kValid, UINT32_C(0x10000), 1);
  const auto ordinaryUndefined =
      image(OtaRunningImageState::kUndefined, UINT32_C(0x10000), 1);
  const auto ordinaryNew =
      image(OtaRunningImageState::kNew, UINT32_C(0x10000), 1);
  const auto ordinaryInvalid =
      image(OtaRunningImageState::kInvalid, UINT32_C(0x10000), 1);
  const auto ordinaryAborted =
      image(OtaRunningImageState::kAborted, UINT32_C(0x10000), 1);
  const auto candidatePending =
      image(OtaRunningImageState::kPendingVerify, UINT32_C(0x190000), 2);
  const auto candidateValid =
      image(OtaRunningImageState::kValid, UINT32_C(0x190000), 2);
  const auto candidateUndefined =
      image(OtaRunningImageState::kUndefined, UINT32_C(0x190000), 2);

  OtaInstallState state = {};
  assert(decideOtaBootRecovery(state, ordinaryValid, "0.7.0", "usb") ==
         OtaBootRecoveryAction::kOrdinary);
  assert(decideOtaBootRecovery(state, ordinaryUndefined, "0.7.0", "usb") ==
         OtaBootRecoveryAction::kOrdinary);
  assert(decideOtaBootRecovery(state, candidatePending, "0.7.0", "usb") ==
         OtaBootRecoveryAction::kRollbackPendingImage);
  assert(decideOtaBootRecovery(state, ordinaryNew, "0.7.0", "usb") ==
         OtaBootRecoveryAction::kHalt);
  assert(decideOtaBootRecovery(state, ordinaryInvalid, "0.7.0", "usb") ==
         OtaBootRecoveryAction::kHalt);
  assert(decideOtaBootRecovery(state, ordinaryAborted, "0.7.0", "usb") ==
         OtaBootRecoveryAction::kHalt);
  assert(decideOtaBootRecovery(state, OtaRunningImageInfo{}, "0.7.0",
                               "usb") == OtaBootRecoveryAction::kHalt);

  assert(otaStagePendingRelease(
      &state, "rel-0123456789abcdef0123456789abcdef", manifest(),
      UINT32_C(0x190000)));
  assert(decideOtaBootRecovery(state, candidatePending, "0.7.0",
                               "git.0123456789ab") ==
         OtaBootRecoveryAction::kRollbackPendingImage);
  uint8_t digest[kOtaSha256Size] = {};
  digest[0] = 2;
  assert(otaMarkPendingImageAccepted(&state, UINT32_C(0x190000), digest));
  assert(decideOtaBootRecovery(state, candidatePending, "0.7.0",
                               "git.0123456789ab") ==
         OtaBootRecoveryAction::kValidatePendingImage);
  assert(decideOtaBootRecovery(state, ordinaryValid, "0.7.0",
                               "git.0123456789ab") ==
         OtaBootRecoveryAction::kReportProductionRollback);
  assert(decideOtaBootRecovery(state, candidateValid, "0.7.0",
                               "git.0123456789ab") ==
         OtaBootRecoveryAction::kHalt);
  assert(otaMarkPendingValidated(&state, "git.0123456789ab"));
  assert(decideOtaBootRecovery(state, candidateValid, "0.7.0",
                               "git.0123456789ab") ==
         OtaBootRecoveryAction::kPromoteProduction);
  assert(decideOtaBootRecovery(state, candidateValid, "0.7.0", "spoof") ==
         OtaBootRecoveryAction::kHalt);

  otaCancelPendingRelease(&state);
  assert(otaStagePendingDevelopmentImage(&state, UINT32_C(0x190000)));
  // A rejected ArduinoOTA candidate remains selected with an unaccepted
  // journal. Its first PENDING boot must roll back; once the untouched old slot
  // is running again, either stable state clears the abandoned transaction.
  assert(decideOtaBootRecovery(state, candidatePending, "dev", "dev") ==
         OtaBootRecoveryAction::kRollbackPendingImage);
  assert(decideOtaBootRecovery(state, ordinaryValid, "dev", "dev") ==
         OtaBootRecoveryAction::kClearDevelopment);
  assert(decideOtaBootRecovery(state, ordinaryUndefined, "dev", "dev") ==
         OtaBootRecoveryAction::kClearDevelopment);
  assert(otaMarkPendingImageAccepted(&state, UINT32_C(0x190000), digest));
  assert(decideOtaBootRecovery(state, candidatePending, "dev", "dev") ==
         OtaBootRecoveryAction::kValidatePendingImage);
  assert(otaMarkPendingValidated(&state, nullptr));
  assert(decideOtaBootRecovery(state, candidateValid, "dev", "dev") ==
         OtaBootRecoveryAction::kClearDevelopment);
  assert(decideOtaBootRecovery(state, candidateUndefined, "dev", "dev") ==
         OtaBootRecoveryAction::kHalt);
  assert(decideOtaBootRecovery(state, ordinaryValid, "dev", "dev") ==
         OtaBootRecoveryAction::kClearDevelopment);
  return 0;
}
