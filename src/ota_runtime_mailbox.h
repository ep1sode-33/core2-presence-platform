#pragma once

#include <cstddef>
#include <cstdint>

#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#else
#include <mutex>
#endif

enum class OtaRuntimePhase : uint8_t {
  kUnavailable = 0,
  kInactive,
  kAwaitingLocalConfirmation,
  kDevelopmentWindowOpen,
  kDevelopmentUploading,
  kProductionDownloading,
  kProductionVerifying,
  kRebootPending,
  kValidating,
  kRunning,
  kFailed,
};

enum class OtaRuntimeError : uint8_t {
  kNone = 0,
  kDevelopmentSecretMissing,
  kDevelopmentServiceFailed,
  kProductionTrustUnavailable,
  kControlProtocol,
  kNetwork,
  kManifestRejected,
  kImageRejected,
  kStorage,
  kLocalConfirmationRejected,
  kBootValidationFailed,
};

struct OtaRuntimeSnapshot {
  static constexpr size_t kIpCapacity = 16;
  static constexpr size_t kHostnameCapacity = 33;
  static constexpr size_t kReleaseIdCapacity = 49;

  uint64_t updatedAtMs = 0;
  uint64_t confirmedReleaseCounter = 0;
  uint32_t remainingMs = 0;
  uint32_t completedBytes = 0;
  uint32_t totalBytes = 0;
  uint32_t maximumMainLoopGapMs = 0;
  uint32_t invalidMicrophoneWindows = 0;
  uint32_t totalMicrophoneWindows = 0;
  uint32_t consecutiveInvalidMicrophoneWindows = 0;
  uint32_t version = 0;
  OtaRuntimePhase phase = OtaRuntimePhase::kUnavailable;
  OtaRuntimeError error = OtaRuntimeError::kNone;
  char localIp[kIpCapacity] = {};
  char hostname[kHostnameCapacity] = {};
  char releaseId[kReleaseIdCapacity] = {};
  bool developmentConfigured = false;
  bool productionTrusted = false;
  // Boot validation must not infer durable OTA state from the runtime phase.
  // These flags form the worker-to-main half of the two-phase confirmation
  // handshake.
  bool installStateKnown = false;
  bool installStateHealthy = false;
  bool productionPending = false;
  bool developmentPending = false;
  bool confirmationPrepared = false;
};

struct OtaSafetyAbortRequest {
  uint32_t maximumMainLoopGapMs = 0;
  uint32_t invalidMicrophoneWindows = 0;
  uint32_t totalMicrophoneWindows = 0;
  uint32_t consecutiveInvalidMicrophoneWindows = 0;
};

enum class OtaBootValidationNotice : uint8_t {
  kNone = 0,
  kPrepareConfirmation,
  kConfirmed,
  kFailed,
};

// Cross-core bridge. Main is the only producer of physical-open and boot
// validation notices; the Core 0 network worker is the only consumer and the
// only publisher of the immutable display/diagnostic snapshot.
class OtaRuntimeMailbox {
 public:
  bool requestPhysicallyConfirmedDevelopmentOpen();
  bool takePhysicallyConfirmedDevelopmentOpen();

  bool requestSafetyAbort(const OtaSafetyAbortRequest& request);
  bool takeSafetyAbort(OtaSafetyAbortRequest* output);
  bool publishSafetyMetrics(const OtaSafetyAbortRequest& metrics);
  bool takeSafetyMetrics(OtaSafetyAbortRequest* output);

  bool publishBootValidationNotice(OtaBootValidationNotice notice);
  OtaBootValidationNotice takeBootValidationNotice();

  bool publishSnapshot(const OtaRuntimeSnapshot& snapshot);
  bool copySnapshot(OtaRuntimeSnapshot* output) const;

 private:
  void lock() const;
  void unlock() const;

#if defined(ARDUINO_ARCH_ESP32)
  mutable portMUX_TYPE mutex_ = portMUX_INITIALIZER_UNLOCKED;
#else
  mutable std::mutex mutex_;
#endif
  OtaRuntimeSnapshot snapshot_ = {};
  OtaBootValidationNotice bootNotice_ = OtaBootValidationNotice::kNone;
  OtaSafetyAbortRequest safetyAbort_ = {};
  OtaSafetyAbortRequest safetyMetrics_ = {};
  bool physicalOpenPending_ = false;
  bool safetyAbortPending_ = false;
  bool safetyMetricsPending_ = false;
  bool snapshotInitialized_ = false;
};

const char* otaRuntimePhaseName(OtaRuntimePhase phase);
const char* otaRuntimeErrorName(OtaRuntimeError error);
