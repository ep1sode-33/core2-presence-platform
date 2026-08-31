#include "ota_boot_policy.h"

#include <cstring>

OtaBootRecoveryAction decideOtaBootRecovery(
    const OtaInstallState& installState,
    const OtaRunningImageInfo& runningImage, const char* runningVersion,
    const char* runningBuildId) {
  if (!otaInstallStateIsValid(installState) || !runningImage.valid()) {
    return OtaBootRecoveryAction::kHalt;
  }
  const bool production = installState.pending;
  const bool development = installState.developmentPending;
  if (!production && !development) {
    if (runningImage.state == OtaRunningImageState::kPendingVerify) {
      return OtaBootRecoveryAction::kRollbackPendingImage;
    }
    return runningImage.state == OtaRunningImageState::kValid ||
                   runningImage.state == OtaRunningImageState::kUndefined
               ? OtaBootRecoveryAction::kOrdinary
               : OtaBootRecoveryAction::kHalt;
  }

  const bool identityMatches = otaPendingImageIdentityMatches(
      installState, runningImage.identity.partitionAddress,
      runningImage.identity.sha256);
  const bool releaseMatches =
      !production ||
      (runningVersion != nullptr && runningBuildId != nullptr &&
       std::strcmp(installState.pendingFirmwareVersion, runningVersion) == 0 &&
       std::strcmp(installState.pendingBuildId, runningBuildId) == 0);

  if (runningImage.state == OtaRunningImageState::kPendingVerify) {
    return identityMatches && releaseMatches &&
                   installState.pendingImageAccepted
               ? OtaBootRecoveryAction::kValidatePendingImage
               : OtaBootRecoveryAction::kRollbackPendingImage;
  }
  if (runningImage.state == OtaRunningImageState::kValid && identityMatches) {
    if (!releaseMatches || !installState.pendingImageAccepted ||
        !installState.pendingValidated) {
      return OtaBootRecoveryAction::kHalt;
    }
    return production ? OtaBootRecoveryAction::kPromoteProduction
                      : OtaBootRecoveryAction::kClearDevelopment;
  }
  if ((runningImage.state == OtaRunningImageState::kValid ||
       runningImage.state == OtaRunningImageState::kUndefined) &&
      !identityMatches) {
    return production ? OtaBootRecoveryAction::kReportProductionRollback
                      : OtaBootRecoveryAction::kClearDevelopment;
  }
  return OtaBootRecoveryAction::kHalt;
}
