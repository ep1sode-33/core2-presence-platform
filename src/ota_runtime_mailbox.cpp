#include "ota_runtime_mailbox.h"

#include <cstring>

namespace {

bool terminated(const char* value, size_t capacity) {
  return value != nullptr && capacity != 0 &&
         std::memchr(value, '\0', capacity) != nullptr;
}

bool snapshotValid(const OtaRuntimeSnapshot& snapshot) {
  if (!terminated(snapshot.localIp, sizeof(snapshot.localIp)) ||
      !terminated(snapshot.hostname, sizeof(snapshot.hostname)) ||
      !terminated(snapshot.releaseId, sizeof(snapshot.releaseId)) ||
      (snapshot.totalBytes == 0 && snapshot.completedBytes != 0) ||
      snapshot.completedBytes > snapshot.totalBytes ||
      snapshot.invalidMicrophoneWindows > snapshot.totalMicrophoneWindows ||
      snapshot.consecutiveInvalidMicrophoneWindows >
          snapshot.totalMicrophoneWindows ||
      (snapshot.installStateHealthy && !snapshot.installStateKnown) ||
      ((snapshot.productionPending || snapshot.developmentPending) &&
       !snapshot.installStateKnown) ||
      (snapshot.productionPending && snapshot.developmentPending) ||
      (snapshot.confirmationPrepared &&
       (!(snapshot.productionPending || snapshot.developmentPending) ||
        !snapshot.installStateHealthy))) {
    return false;
  }
  if (snapshot.phase == OtaRuntimePhase::kDevelopmentWindowOpen &&
      snapshot.remainingMs == 0) {
    return false;
  }
  return true;
}

bool safetyAbortValid(const OtaSafetyAbortRequest& request) {
  return request.invalidMicrophoneWindows <= request.totalMicrophoneWindows &&
         request.consecutiveInvalidMicrophoneWindows <=
             request.totalMicrophoneWindows &&
         (request.maximumMainLoopGapMs >= 500 ||
          request.consecutiveInvalidMicrophoneWindows > 5);
}

bool safetyMetricsValid(const OtaSafetyAbortRequest& request) {
  return request.invalidMicrophoneWindows <= request.totalMicrophoneWindows &&
         request.consecutiveInvalidMicrophoneWindows <=
             request.totalMicrophoneWindows;
}

}  // namespace

bool OtaRuntimeMailbox::requestPhysicallyConfirmedDevelopmentOpen() {
  lock();
  const bool accepted = !physicalOpenPending_;
  if (accepted) {
    physicalOpenPending_ = true;
  }
  unlock();
  return accepted;
}

bool OtaRuntimeMailbox::takePhysicallyConfirmedDevelopmentOpen() {
  lock();
  const bool pending = physicalOpenPending_;
  physicalOpenPending_ = false;
  unlock();
  return pending;
}

bool OtaRuntimeMailbox::requestSafetyAbort(
    const OtaSafetyAbortRequest& request) {
  if (!safetyAbortValid(request)) {
    return false;
  }
  lock();
  // A later observation can only strengthen the evidence retained for
  // diagnostics; never replace a larger counter with a smaller one.
  safetyAbort_.maximumMainLoopGapMs =
      request.maximumMainLoopGapMs > safetyAbort_.maximumMainLoopGapMs
          ? request.maximumMainLoopGapMs
          : safetyAbort_.maximumMainLoopGapMs;
  safetyAbort_.invalidMicrophoneWindows =
      request.invalidMicrophoneWindows > safetyAbort_.invalidMicrophoneWindows
          ? request.invalidMicrophoneWindows
          : safetyAbort_.invalidMicrophoneWindows;
  safetyAbort_.totalMicrophoneWindows =
      request.totalMicrophoneWindows > safetyAbort_.totalMicrophoneWindows
          ? request.totalMicrophoneWindows
          : safetyAbort_.totalMicrophoneWindows;
  safetyAbort_.consecutiveInvalidMicrophoneWindows =
      request.consecutiveInvalidMicrophoneWindows >
              safetyAbort_.consecutiveInvalidMicrophoneWindows
          ? request.consecutiveInvalidMicrophoneWindows
          : safetyAbort_.consecutiveInvalidMicrophoneWindows;
  safetyAbortPending_ = true;
  safetyMetrics_ = request;
  safetyMetricsPending_ = true;
  unlock();
  return true;
}

bool OtaRuntimeMailbox::publishSafetyMetrics(
    const OtaSafetyAbortRequest& metrics) {
  if (!safetyMetricsValid(metrics)) {
    return false;
  }
  lock();
  safetyMetrics_ = metrics;
  safetyMetricsPending_ = true;
  unlock();
  return true;
}

bool OtaRuntimeMailbox::takeSafetyMetrics(OtaSafetyAbortRequest* output) {
  if (output == nullptr) {
    return false;
  }
  lock();
  const bool pending = safetyMetricsPending_;
  if (pending) {
    *output = safetyMetrics_;
    safetyMetricsPending_ = false;
  }
  unlock();
  return pending;
}

bool OtaRuntimeMailbox::takeSafetyAbort(OtaSafetyAbortRequest* output) {
  if (output == nullptr) {
    return false;
  }
  lock();
  const bool pending = safetyAbortPending_;
  if (pending) {
    *output = safetyAbort_;
    safetyAbort_ = {};
    safetyAbortPending_ = false;
  }
  unlock();
  return pending;
}

bool OtaRuntimeMailbox::publishBootValidationNotice(
    OtaBootValidationNotice notice) {
  if (notice == OtaBootValidationNotice::kNone) {
    return false;
  }
  lock();
  const bool accepted = bootNotice_ == OtaBootValidationNotice::kNone ||
                        bootNotice_ == notice ||
                        (bootNotice_ ==
                             OtaBootValidationNotice::kPrepareConfirmation &&
                         (notice == OtaBootValidationNotice::kConfirmed ||
                          notice == OtaBootValidationNotice::kFailed));
  if (accepted) {
    bootNotice_ = notice;
  }
  unlock();
  return accepted;
}

OtaBootValidationNotice OtaRuntimeMailbox::takeBootValidationNotice() {
  lock();
  const OtaBootValidationNotice notice = bootNotice_;
  bootNotice_ = OtaBootValidationNotice::kNone;
  unlock();
  return notice;
}

bool OtaRuntimeMailbox::publishSnapshot(
    const OtaRuntimeSnapshot& snapshot) {
  if (!snapshotValid(snapshot)) {
    return false;
  }
  lock();
  OtaRuntimeSnapshot candidate = snapshot;
  candidate.version = snapshot_.version + 1U;
  if (candidate.version == 0) {
    candidate.version = 1;
  }
  snapshot_ = candidate;
  snapshotInitialized_ = true;
  unlock();
  return true;
}

bool OtaRuntimeMailbox::copySnapshot(OtaRuntimeSnapshot* output) const {
  if (output == nullptr) {
    return false;
  }
  lock();
  const bool initialized = snapshotInitialized_;
  if (initialized) {
    *output = snapshot_;
  }
  unlock();
  return initialized;
}

void OtaRuntimeMailbox::lock() const {
#if defined(ARDUINO_ARCH_ESP32)
  portENTER_CRITICAL(&mutex_);
#else
  mutex_.lock();
#endif
}

void OtaRuntimeMailbox::unlock() const {
#if defined(ARDUINO_ARCH_ESP32)
  portEXIT_CRITICAL(&mutex_);
#else
  mutex_.unlock();
#endif
}

const char* otaRuntimePhaseName(OtaRuntimePhase phase) {
  switch (phase) {
    case OtaRuntimePhase::kUnavailable:
      return "unavailable";
    case OtaRuntimePhase::kInactive:
      return "inactive";
    case OtaRuntimePhase::kAwaitingLocalConfirmation:
      return "awaiting_local_confirmation";
    case OtaRuntimePhase::kDevelopmentWindowOpen:
      return "dev_window_open";
    case OtaRuntimePhase::kDevelopmentUploading:
      return "dev_uploading";
    case OtaRuntimePhase::kProductionDownloading:
      return "downloading";
    case OtaRuntimePhase::kProductionVerifying:
      return "verifying";
    case OtaRuntimePhase::kRebootPending:
      return "reboot_pending";
    case OtaRuntimePhase::kValidating:
      return "validating";
    case OtaRuntimePhase::kRunning:
      return "running";
    case OtaRuntimePhase::kFailed:
      return "failed";
  }
  return "unavailable";
}

const char* otaRuntimeErrorName(OtaRuntimeError error) {
  switch (error) {
    case OtaRuntimeError::kNone:
      return "none";
    case OtaRuntimeError::kDevelopmentSecretMissing:
      return "development_secret_missing";
    case OtaRuntimeError::kDevelopmentServiceFailed:
      return "development_service_failed";
    case OtaRuntimeError::kProductionTrustUnavailable:
      return "production_trust_unavailable";
    case OtaRuntimeError::kControlProtocol:
      return "control_protocol";
    case OtaRuntimeError::kNetwork:
      return "network";
    case OtaRuntimeError::kManifestRejected:
      return "manifest_rejected";
    case OtaRuntimeError::kImageRejected:
      return "image_rejected";
    case OtaRuntimeError::kStorage:
      return "storage";
    case OtaRuntimeError::kLocalConfirmationRejected:
      return "local_confirmation_rejected";
    case OtaRuntimeError::kBootValidationFailed:
      return "boot_validation_failed";
  }
  return "unknown";
}
