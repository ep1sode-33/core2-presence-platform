#pragma once

#include <cstdint>

#include "ota_boot_validation.h"
#include "ota_install_state.h"

enum class OtaBootRecoveryAction : uint8_t {
  kOrdinary = 0,
  kValidatePendingImage,
  kPromoteProduction,
  kClearDevelopment,
  kReportProductionRollback,
  kRollbackPendingImage,
  kHalt,
};

// Pure fail-closed startup policy. Release counters advance only for an exact
// VALID partition whose accepted and prepared transaction matches both the
// persisted partition digest and the running production build identity.
OtaBootRecoveryAction decideOtaBootRecovery(
    const OtaInstallState& installState,
    const OtaRunningImageInfo& runningImage, const char* runningVersion,
    const char* runningBuildId);
