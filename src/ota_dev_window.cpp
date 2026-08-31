#include "ota_dev_window.h"

#include <cstring>
#include <limits>

#if defined(ARDUINO_ARCH_ESP32)
#include <ArduinoOTA.h>
#include <Update.h>
#include <esp_ota_ops.h>
#endif

namespace {

bool base64UrlByte(char value) {
  return (value >= 'a' && value <= 'z') ||
         (value >= 'A' && value <= 'Z') ||
         (value >= '0' && value <= '9') || value == '-' || value == '_';
}

bool validHostname(const char* hostname) {
  if (hostname == nullptr) {
    return false;
  }
  const void* terminator =
      std::memchr(hostname, '\0', kOtaDevelopmentHostnameMaximumLength + 1);
  if (terminator == nullptr || terminator == hostname) {
    return false;
  }
  const size_t length = static_cast<const char*>(terminator) - hostname;
  for (size_t index = 0; index < length; ++index) {
    const char value = hostname[index];
    if (!((value >= 'a' && value <= 'z') ||
          (value >= 'A' && value <= 'Z') ||
          (value >= '0' && value <= '9') || value == '-')) {
      return false;
    }
  }
  return true;
}

#if defined(ARDUINO_ARCH_ESP32)
OtaDevelopmentWindow* activeWindow = nullptr;
OtaDevelopmentSafetyAbortCheck activeSafetyAbortCheck = nullptr;
OtaDevelopmentCompletionCheck activeCompletionCheck = nullptr;
OtaDevelopmentActivityCallback activeActivityCallback = nullptr;
void* activeSafetyAbortContext = nullptr;

void handleStart() {
  if (activeWindow == nullptr) {
    Update.abort();
    return;
  }
  if (ArduinoOTA.getCommand() != U_FLASH) {
    Update.abort();
    activeWindow->noteUploadFailed(
        OtaDevelopmentWindowError::kNonApplicationUpload);
    return;
  }
  if (activeSafetyAbortCheck != nullptr &&
      activeSafetyAbortCheck(activeSafetyAbortContext)) {
    Update.abort();
    activeWindow->noteUploadFailed(OtaDevelopmentWindowError::kSafetyAbort);
    return;
  }
  if (!activeWindow->noteUploadStarted()) {
    Update.abort();
    return;
  }
  if (activeActivityCallback != nullptr) {
    activeActivityCallback(activeSafetyAbortContext);
  }
}

void handleProgress(unsigned int completed, unsigned int total) {
  if (activeWindow != nullptr) {
    if (activeSafetyAbortCheck != nullptr &&
        activeSafetyAbortCheck(activeSafetyAbortContext)) {
      Update.abort();
      activeWindow->noteUploadFailed(OtaDevelopmentWindowError::kSafetyAbort);
      return;
    }
    if (!activeWindow->noteUploadProgress(completed, total)) {
      Update.abort();
      activeWindow->noteUploadFailed(
          OtaDevelopmentWindowError::kInvalidProgress);
      return;
    }
    if (activeActivityCallback != nullptr) {
      activeActivityCallback(activeSafetyAbortContext);
    }
  }
}

void handleEnd() {
  if (activeWindow != nullptr) {
    if (activeCompletionCheck != nullptr &&
        !activeCompletionCheck(activeSafetyAbortContext)) {
      activeWindow->noteUploadFailed(OtaDevelopmentWindowError::kSafetyAbort);
      return;
    }
    activeWindow->noteUploadSucceeded();
  }
}

void handleError(ota_error_t error) {
  if (activeWindow == nullptr) {
    return;
  }
  OtaDevelopmentWindowError mapped = OtaDevelopmentWindowError::kFinalize;
  switch (error) {
    case OTA_AUTH_ERROR:
      mapped = OtaDevelopmentWindowError::kAuthentication;
      break;
    case OTA_BEGIN_ERROR:
      mapped = OtaDevelopmentWindowError::kBegin;
      break;
    case OTA_CONNECT_ERROR:
      mapped = OtaDevelopmentWindowError::kConnect;
      break;
    case OTA_RECEIVE_ERROR:
      mapped = OtaDevelopmentWindowError::kReceive;
      break;
    case OTA_END_ERROR:
      mapped = OtaDevelopmentWindowError::kFinalize;
      break;
  }
  activeWindow->noteUploadFailed(mapped);
}
#endif

}  // namespace

OtaDevelopmentWindow::~OtaDevelopmentWindow() {
  otaStopArduinoDevelopmentService(this);
  clearSecret();
}

bool OtaDevelopmentWindow::configureSecret(const char* secret, size_t length) {
  if (secret == nullptr || length != kOtaDevelopmentSecretLength ||
      std::memchr(secret, '\0', length) != nullptr) {
    error_ = OtaDevelopmentWindowError::kInvalidSecret;
    return false;
  }
  for (size_t index = 0; index < length; ++index) {
    if (!base64UrlByte(secret[index])) {
      error_ = OtaDevelopmentWindowError::kInvalidSecret;
      return false;
    }
  }
  clearSecret();
  std::memcpy(secret_, secret, length);
  secret_[length] = '\0';
  phase_ = OtaDevelopmentWindowPhase::kClosed;
  error_ = OtaDevelopmentWindowError::kNone;
  return true;
}

bool OtaDevelopmentWindow::openAfterPhysicalConfirmation(uint64_t nowMs) {
  if (phase_ == OtaDevelopmentWindowPhase::kUnconfigured) {
    error_ = OtaDevelopmentWindowError::kNotConfigured;
    return false;
  }
  if (phase_ == OtaDevelopmentWindowPhase::kUploading) {
    return false;
  }
  openedAtMs_ = nowMs;
  deadlineMs_ =
      nowMs > std::numeric_limits<uint64_t>::max() -
                  kOtaDevelopmentWindowDurationMs
          ? std::numeric_limits<uint64_t>::max()
          : nowMs + kOtaDevelopmentWindowDurationMs;
  completedBytes_ = 0;
  totalBytes_ = 0;
  phase_ = OtaDevelopmentWindowPhase::kOpen;
  error_ = OtaDevelopmentWindowError::kNone;
  return true;
}

void OtaDevelopmentWindow::close() {
  if (phase_ != OtaDevelopmentWindowPhase::kUnconfigured) {
    phase_ = OtaDevelopmentWindowPhase::kClosed;
    error_ = OtaDevelopmentWindowError::kNone;
  }
  openedAtMs_ = 0;
  deadlineMs_ = 0;
  completedBytes_ = 0;
  totalBytes_ = 0;
}

void OtaDevelopmentWindow::tick(uint64_t nowMs) {
  if (phase_ == OtaDevelopmentWindowPhase::kOpen && nowMs >= deadlineMs_) {
    close();
  }
}

bool OtaDevelopmentWindow::noteUploadStarted() {
  if (phase_ != OtaDevelopmentWindowPhase::kOpen) {
    error_ = OtaDevelopmentWindowError::kNotOpen;
    return false;
  }
  phase_ = OtaDevelopmentWindowPhase::kUploading;
  completedBytes_ = 0;
  totalBytes_ = 0;
  return true;
}

bool OtaDevelopmentWindow::noteUploadProgress(uint32_t completed,
                                              uint32_t total) {
  if (phase_ != OtaDevelopmentWindowPhase::kUploading || total == 0 ||
      completed > total || (totalBytes_ != 0 && total != totalBytes_) ||
      completed < completedBytes_) {
    error_ = OtaDevelopmentWindowError::kInvalidProgress;
    return false;
  }
  completedBytes_ = completed;
  totalBytes_ = total;
  return true;
}

void OtaDevelopmentWindow::noteUploadSucceeded() {
  // ArduinoOTA invokes onEnd only after Update.end() succeeds. A final progress
  // callback is not guaranteed, so do not reject a valid completed upload.
  if (phase_ == OtaDevelopmentWindowPhase::kUploading) {
    if (totalBytes_ != 0) {
      completedBytes_ = totalBytes_;
    }
    phase_ = OtaDevelopmentWindowPhase::kSucceeded;
    error_ = OtaDevelopmentWindowError::kNone;
  } else {
    noteUploadFailed(OtaDevelopmentWindowError::kFinalize);
  }
}

void OtaDevelopmentWindow::noteUploadFailed(OtaDevelopmentWindowError error) {
  phase_ = OtaDevelopmentWindowPhase::kFailed;
  error_ = error == OtaDevelopmentWindowError::kNone
               ? OtaDevelopmentWindowError::kFinalize
               : error;
}

uint32_t OtaDevelopmentWindow::remainingMs(uint64_t nowMs) const {
  if (phase_ != OtaDevelopmentWindowPhase::kOpen || nowMs >= deadlineMs_) {
    return 0;
  }
  const uint64_t remaining = deadlineMs_ - nowMs;
  return remaining > std::numeric_limits<uint32_t>::max()
             ? std::numeric_limits<uint32_t>::max()
             : static_cast<uint32_t>(remaining);
}

bool otaIpv4IsTrustedLan(const uint8_t address[4]) {
  return address != nullptr && address[0] == 192 && address[1] == 168 &&
         address[2] == 0;
}

bool otaStartArduinoDevelopmentService(OtaDevelopmentWindow* window,
                                       const char* hostname,
                                       OtaDevelopmentSafetyAbortCheck
                                           safetyAbortCheck,
                                       OtaDevelopmentCompletionCheck
                                           completionCheck,
                                       OtaDevelopmentActivityCallback
                                           activityCallback,
                                       void* safetyAbortContext) {
  if (window == nullptr || !window->listenerShouldRun() ||
      window->phase() != OtaDevelopmentWindowPhase::kOpen ||
      !validHostname(hostname)) {
    return false;
  }
#if defined(ARDUINO_ARCH_ESP32)
  if (activeWindow != nullptr && activeWindow != window) {
    return false;
  }
  activeWindow = window;
  activeSafetyAbortCheck = safetyAbortCheck;
  activeCompletionCheck = completionCheck;
  activeActivityCallback = activityCallback;
  activeSafetyAbortContext = safetyAbortContext;
  ArduinoOTA.setHostname(hostname)
      .setPassword(window->secretForPlatform())
      .setMdnsEnabled(false)
      // The worker explicitly reboots only after its accepted-image journal
      // is committed. A rejected/ambiguous upload must remain on the current
      // running image.
      .setRebootOnSuccess(false)
      .onStart(handleStart)
      .onProgress(handleProgress)
      .onEnd(handleEnd)
      .onError(handleError);
  ArduinoOTA.setTimeout(1000);
  ArduinoOTA.begin();
  return true;
#else
  (void)safetyAbortCheck;
  (void)completionCheck;
  (void)activityCallback;
  (void)safetyAbortContext;
  window->noteUploadFailed(OtaDevelopmentWindowError::kPlatformUnavailable);
  return false;
#endif
}

void otaServiceArduinoDevelopmentWindow(OtaDevelopmentWindow* window,
                                        uint64_t nowMs) {
  if (window == nullptr) {
    return;
  }
  window->tick(nowMs);
#if defined(ARDUINO_ARCH_ESP32)
  if (activeWindow != window) {
    return;
  }
  if (!window->listenerShouldRun()) {
    otaStopArduinoDevelopmentService(window);
    return;
  }
  ArduinoOTA.handle();
#else
  (void)nowMs;
#endif
}

void otaStopArduinoDevelopmentService(OtaDevelopmentWindow* window) {
#if defined(ARDUINO_ARCH_ESP32)
  if (window != nullptr && activeWindow == window) {
    ArduinoOTA.end();
    activeWindow = nullptr;
    activeSafetyAbortCheck = nullptr;
    activeCompletionCheck = nullptr;
    activeActivityCallback = nullptr;
    activeSafetyAbortContext = nullptr;
  }
#else
  (void)window;
#endif
}

void OtaDevelopmentWindow::clearSecret() {
  volatile char* cursor = secret_;
  for (size_t index = 0; index < sizeof(secret_); ++index) {
    cursor[index] = 0;
  }
}

const char* otaDevelopmentWindowErrorName(OtaDevelopmentWindowError error) {
  switch (error) {
    case OtaDevelopmentWindowError::kNone:
      return "none";
    case OtaDevelopmentWindowError::kInvalidSecret:
      return "invalid_secret";
    case OtaDevelopmentWindowError::kNotConfigured:
      return "not_configured";
    case OtaDevelopmentWindowError::kNotOpen:
      return "not_open";
    case OtaDevelopmentWindowError::kInvalidProgress:
      return "invalid_progress";
    case OtaDevelopmentWindowError::kNonApplicationUpload:
      return "non_application_upload";
    case OtaDevelopmentWindowError::kAuthentication:
      return "authentication";
    case OtaDevelopmentWindowError::kBegin:
      return "begin";
    case OtaDevelopmentWindowError::kConnect:
      return "connect";
    case OtaDevelopmentWindowError::kReceive:
      return "receive";
    case OtaDevelopmentWindowError::kFinalize:
      return "finalize";
    case OtaDevelopmentWindowError::kSafetyAbort:
      return "safety_abort";
    case OtaDevelopmentWindowError::kPlatformUnavailable:
      return "platform_unavailable";
  }
  return "unknown";
}
