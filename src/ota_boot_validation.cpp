#include "ota_boot_validation.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_ota_ops.h>
#endif

namespace {

#if defined(ARDUINO_ARCH_ESP32)
bool esp32InspectRunningImage(void*, OtaRunningImageInfo* output) {
  if (output == nullptr) {
    return false;
  }
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running == nullptr) {
    return false;
  }
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  const esp_err_t stateResult = esp_ota_get_state_partition(running, &state);
  if (stateResult != ESP_OK &&
      running->subtype != ESP_PARTITION_SUBTYPE_APP_FACTORY) {
    return false;
  }
  OtaRunningImageInfo info = {};
  if (!otaReadRunningApplicationIdentity(&info.identity)) {
    return false;
  }
  if (running->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) {
    info.state = OtaRunningImageState::kUndefined;
  } else {
    switch (state) {
      case ESP_OTA_IMG_UNDEFINED:
        info.state = OtaRunningImageState::kUndefined;
        break;
      case ESP_OTA_IMG_NEW:
        info.state = OtaRunningImageState::kNew;
        break;
      case ESP_OTA_IMG_PENDING_VERIFY:
        info.state = OtaRunningImageState::kPendingVerify;
        break;
      case ESP_OTA_IMG_VALID:
        info.state = OtaRunningImageState::kValid;
        break;
      case ESP_OTA_IMG_INVALID:
        info.state = OtaRunningImageState::kInvalid;
        break;
      case ESP_OTA_IMG_ABORTED:
        info.state = OtaRunningImageState::kAborted;
        break;
      default:
        return false;
    }
  }
  *output = info;
  return info.valid();
}

bool esp32ConfirmRunningImage(void*) {
  return esp_ota_mark_app_valid_cancel_rollback() == ESP_OK;
}

bool esp32RollbackAndReboot(void*) {
  return esp_ota_mark_app_invalid_rollback_and_reboot() == ESP_OK;
}
#endif

}  // namespace

extern "C" bool verifyRollbackLater() { return true; }

bool OtaDelayedBootValidator::begin(
    uint64_t nowMs, const OtaBootValidationBackend& backend) {
  backend_ = {};
  runningImageInfo_ = {};
  validationStartedAtMs_ = nowMs;
  healthySinceMs_ = 0;
  healthWindowStarted_ = false;
  phase_ = OtaBootValidationPhase::kNotStarted;
  error_ = OtaBootValidationError::kNone;
  if (backend.inspectRunningImage == nullptr ||
      backend.confirmRunningImage == nullptr ||
      backend.rollbackAndReboot == nullptr) {
    phase_ = OtaBootValidationPhase::kPlatformError;
    error_ = OtaBootValidationError::kInvalidBackend;
    return false;
  }
  backend_ = backend;
  if (!backend_.inspectRunningImage(backend_.context, &runningImageInfo_) ||
      !runningImageInfo_.valid()) {
    phase_ = OtaBootValidationPhase::kPlatformError;
    error_ = OtaBootValidationError::kPendingStateReadFailed;
    return false;
  }
  if (runningImageInfo_.state == OtaRunningImageState::kPendingVerify) {
    phase_ = OtaBootValidationPhase::kWaiting;
  } else if (runningImageInfo_.state == OtaRunningImageState::kValid ||
             runningImageInfo_.state == OtaRunningImageState::kUndefined) {
    phase_ = OtaBootValidationPhase::kNotPending;
  } else {
    phase_ = OtaBootValidationPhase::kPlatformError;
    error_ = OtaBootValidationError::kPendingStateReadFailed;
    return false;
  }
  return true;
}

bool OtaDelayedBootValidator::poll(uint64_t nowMs,
                                   const OtaBootHealthGates& gates,
                                   bool hardFailure) {
  if (phase_ != OtaBootValidationPhase::kWaiting &&
      phase_ != OtaBootValidationPhase::kAwaitingPersistence) {
    return phase_ == OtaBootValidationPhase::kNotPending ||
           phase_ == OtaBootValidationPhase::kConfirmed;
  }
  if (hardFailure) {
    error_ = OtaBootValidationError::kHardValidationFailed;
    if (!backend_.rollbackAndReboot(backend_.context)) {
      phase_ = OtaBootValidationPhase::kPlatformError;
      error_ = OtaBootValidationError::kRollbackFailed;
      return false;
    }
    phase_ = OtaBootValidationPhase::kRollbackRequested;
    return false;
  }
  if (phase_ == OtaBootValidationPhase::kAwaitingPersistence) {
    return true;
  }
  if (!gates.allReady()) {
    healthySinceMs_ = 0;
    healthWindowStarted_ = false;
    return true;
  }
  if (!healthWindowStarted_) {
    healthySinceMs_ = nowMs;
    healthWindowStarted_ = true;
  }
  if (nowMs - healthySinceMs_ < kOtaBootValidationDelayMs) {
    return true;
  }
  phase_ = OtaBootValidationPhase::kAwaitingPersistence;
  return true;
}

bool OtaDelayedBootValidator::confirmAfterPersistence() {
  if (phase_ != OtaBootValidationPhase::kAwaitingPersistence) {
    return phase_ == OtaBootValidationPhase::kConfirmed;
  }
  if (!backend_.confirmRunningImage(backend_.context)) {
    phase_ = OtaBootValidationPhase::kPlatformError;
    error_ = OtaBootValidationError::kConfirmationFailed;
    return false;
  }
  phase_ = OtaBootValidationPhase::kConfirmed;
  error_ = OtaBootValidationError::kNone;
  return true;
}

uint32_t OtaDelayedBootValidator::remainingMs(uint64_t nowMs) const {
  if (phase_ != OtaBootValidationPhase::kWaiting) {
    return 0;
  }
  if (!healthWindowStarted_) {
    return static_cast<uint32_t>(kOtaBootValidationDelayMs);
  }
  if (nowMs - healthySinceMs_ >= kOtaBootValidationDelayMs) {
    return 0;
  }
  return static_cast<uint32_t>(kOtaBootValidationDelayMs -
                               (nowMs - healthySinceMs_));
}

OtaBootValidationBackend otaEsp32BootValidationBackend() {
#if defined(ARDUINO_ARCH_ESP32)
  return {nullptr, esp32InspectRunningImage, esp32ConfirmRunningImage,
          esp32RollbackAndReboot};
#else
  return {};
#endif
}

const char* otaBootValidationErrorName(OtaBootValidationError error) {
  switch (error) {
    case OtaBootValidationError::kNone:
      return "none";
    case OtaBootValidationError::kInvalidBackend:
      return "invalid_backend";
    case OtaBootValidationError::kPendingStateReadFailed:
      return "pending_state_read_failed";
    case OtaBootValidationError::kHardValidationFailed:
      return "hard_validation_failed";
    case OtaBootValidationError::kConfirmationFailed:
      return "confirmation_failed";
    case OtaBootValidationError::kRollbackFailed:
      return "rollback_failed";
  }
  return "unknown";
}
