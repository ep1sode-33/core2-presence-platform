#pragma once

#include <cstdint>

#include "ota_update.h"

constexpr uint64_t kOtaBootValidationDelayMs = 30U * 1000U;

// Only local application initialization is a hard confirmation gate. Network,
// backend, weather, and optional-sensor availability intentionally do not appear
// here, so a temporary outage cannot roll back a healthy image.
struct OtaBootHealthGates {
  bool mainLoopHealthy = false;
  bool uploaderTaskHealthy = false;
  bool settingsStorageHealthy = false;
  bool filesystemHealthy = false;
  bool displayHealthy = false;
  bool otaInstallStateReady = false;

  bool allReady() const {
    return mainLoopHealthy && uploaderTaskHealthy && settingsStorageHealthy &&
           filesystemHealthy && displayHealthy && otaInstallStateReady;
  }
};

enum class OtaRunningImageState : uint8_t {
  kUnknown = 0,
  kUndefined,
  kNew,
  kPendingVerify,
  kValid,
  kInvalid,
  kAborted,
};

struct OtaRunningImageInfo {
  OtaRunningImageState state = OtaRunningImageState::kUnknown;
  OtaApplicationImageIdentity identity = {};

  bool valid() const {
    return state != OtaRunningImageState::kUnknown &&
           otaApplicationImageIdentityIsValid(identity);
  }
};

struct OtaBootValidationBackend {
  void* context = nullptr;
  bool (*inspectRunningImage)(void* context,
                              OtaRunningImageInfo* output) = nullptr;
  bool (*confirmRunningImage)(void* context) = nullptr;
  bool (*rollbackAndReboot)(void* context) = nullptr;
};

enum class OtaBootValidationPhase : uint8_t {
  kNotStarted = 0,
  kNotPending,
  kWaiting,
  // The local health interval passed. The caller must first durably prepare
  // any production-release state, then call confirmAfterPersistence().
  kAwaitingPersistence,
  kConfirmed,
  kRollbackRequested,
  kPlatformError,
};

enum class OtaBootValidationError : uint8_t {
  kNone = 0,
  kInvalidBackend,
  kPendingStateReadFailed,
  kHardValidationFailed,
  kConfirmationFailed,
  kRollbackFailed,
};

class OtaDelayedBootValidator {
 public:
  bool begin(uint64_t nowMs, const OtaBootValidationBackend& backend);

  // Poll from the hardware loop. hardFailure is latched and immediately asks
  // the bootloader to return to the previous image. A healthy pending image is
  // confirmed only after the complete 30-second validation interval.
  bool poll(uint64_t nowMs, const OtaBootHealthGates& gates, bool hardFailure);

  // Completes the bootloader half of the confirmation transaction only after
  // the worker has durably prepared the release state. Development OTA has no
  // release record, but still calls this after the worker reports its state
  // store has been initialized successfully.
  bool confirmAfterPersistence();

  OtaBootValidationPhase phase() const { return phase_; }
  OtaBootValidationError error() const { return error_; }
  const OtaRunningImageInfo& runningImageInfo() const {
    return runningImageInfo_;
  }
  uint64_t validationStartedAtMs() const { return validationStartedAtMs_; }
  uint32_t remainingMs(uint64_t nowMs) const;

 private:
  OtaBootValidationBackend backend_ = {};
  OtaRunningImageInfo runningImageInfo_ = {};
  uint64_t validationStartedAtMs_ = 0;
  uint64_t healthySinceMs_ = 0;
  bool healthWindowStarted_ = false;
  OtaBootValidationPhase phase_ = OtaBootValidationPhase::kNotStarted;
  OtaBootValidationError error_ = OtaBootValidationError::kNone;
};

OtaBootValidationBackend otaEsp32BootValidationBackend();

// Strong override for the Arduino core's weak hook. Returning true prevents
// initArduino() from accepting a PENDING_VERIFY image before setup() runs.
extern "C" bool verifyRollbackLater();

const char* otaBootValidationErrorName(OtaBootValidationError error);
