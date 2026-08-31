#pragma once

#include <cstddef>
#include <cstdint>

constexpr uint64_t kOtaDevelopmentWindowDurationMs = 120U * 1000U;
constexpr size_t kOtaDevelopmentSecretLength = 43;
constexpr size_t kOtaDevelopmentHostnameMaximumLength = 32;

enum class OtaDevelopmentWindowPhase : uint8_t {
  kUnconfigured = 0,
  kClosed,
  kOpen,
  kUploading,
  kSucceeded,
  kFailed,
};

enum class OtaDevelopmentWindowError : uint8_t {
  kNone = 0,
  kInvalidSecret,
  kNotConfigured,
  kNotOpen,
  kInvalidProgress,
  kNonApplicationUpload,
  kAuthentication,
  kBegin,
  kConnect,
  kReceive,
  kFinalize,
  kSafetyAbort,
  kPlatformUnavailable,
};

using OtaDevelopmentSafetyAbortCheck = bool (*)(void* context);
using OtaDevelopmentCompletionCheck = bool (*)(void* context);
using OtaDevelopmentActivityCallback = void (*)(void* context);

// Physical gesture detection stays in diagnostics/main. This class owns only
// the fixed-capacity secret and the explicit 120-second service lifetime.
class OtaDevelopmentWindow {
 public:
  OtaDevelopmentWindow() = default;
  ~OtaDevelopmentWindow();

  OtaDevelopmentWindow(const OtaDevelopmentWindow&) = delete;
  OtaDevelopmentWindow& operator=(const OtaDevelopmentWindow&) = delete;

  // Secret contract for the future settings/provisioning integration: exactly
  // 32 random bytes encoded as unpadded base64url (43 ASCII characters).
  bool configureSecret(const char* secret, size_t length);
  bool openAfterPhysicalConfirmation(uint64_t nowMs);
  void close();
  void tick(uint64_t nowMs);

  bool noteUploadStarted();
  bool noteUploadProgress(uint32_t completed, uint32_t total);
  void noteUploadSucceeded();
  void noteUploadFailed(OtaDevelopmentWindowError error);

  OtaDevelopmentWindowPhase phase() const { return phase_; }
  OtaDevelopmentWindowError error() const { return error_; }
  bool listenerShouldRun() const {
    return phase_ == OtaDevelopmentWindowPhase::kOpen ||
           phase_ == OtaDevelopmentWindowPhase::kUploading;
  }
  uint32_t remainingMs(uint64_t nowMs) const;
  uint32_t completedBytes() const { return completedBytes_; }
  uint32_t totalBytes() const { return totalBytes_; }

 private:
  friend bool otaStartArduinoDevelopmentService(
      OtaDevelopmentWindow*, const char*, OtaDevelopmentSafetyAbortCheck,
      OtaDevelopmentCompletionCheck, OtaDevelopmentActivityCallback, void*);
  const char* secretForPlatform() const { return secret_; }
  void clearSecret();

  char secret_[kOtaDevelopmentSecretLength + 1] = {};
  uint64_t openedAtMs_ = 0;
  uint64_t deadlineMs_ = 0;
  uint32_t completedBytes_ = 0;
  uint32_t totalBytes_ = 0;
  OtaDevelopmentWindowPhase phase_ =
      OtaDevelopmentWindowPhase::kUnconfigured;
  OtaDevelopmentWindowError error_ = OtaDevelopmentWindowError::kNone;
};

bool otaIpv4IsTrustedLan(const uint8_t address[4]);

// Optional ArduinoOTA adapter for the physically opened development window.
// It disables mDNS and therefore requires PlatformIO's explicit upload host.
// Call service only from the Core 0/network worker because ArduinoOTA handles a
// complete upload synchronously. Production signed OTA does not use this path.
bool otaStartArduinoDevelopmentService(OtaDevelopmentWindow* window,
                                       const char* hostname,
                                       OtaDevelopmentSafetyAbortCheck
                                           safetyAbortCheck = nullptr,
                                       OtaDevelopmentCompletionCheck
                                           completionCheck = nullptr,
                                       OtaDevelopmentActivityCallback
                                           activityCallback = nullptr,
                                       void* safetyAbortContext = nullptr);
void otaServiceArduinoDevelopmentWindow(OtaDevelopmentWindow* window,
                                        uint64_t nowMs);
void otaStopArduinoDevelopmentService(OtaDevelopmentWindow* window);

const char* otaDevelopmentWindowErrorName(OtaDevelopmentWindowError error);
