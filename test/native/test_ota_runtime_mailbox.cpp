#include <atomic>
#include <cassert>
#include <cstring>
#include <thread>

#include "ota_runtime_mailbox.h"

int main() {
  OtaRuntimeMailbox mailbox;
  OtaRuntimeSnapshot empty = {};
  assert(!mailbox.copySnapshot(&empty));
  assert(!mailbox.copySnapshot(nullptr));

  OtaRuntimeSnapshot snapshot = {};
  snapshot.updatedAtMs = 100;
  snapshot.remainingMs = 120000;
  snapshot.phase = OtaRuntimePhase::kDevelopmentWindowOpen;
  snapshot.developmentConfigured = true;
  snapshot.installStateKnown = true;
  snapshot.installStateHealthy = true;
  std::strcpy(snapshot.localIp, "192.168.0.51");
  std::strcpy(snapshot.hostname, "m5go-81eb60");
  assert(mailbox.publishSnapshot(snapshot));
  OtaRuntimeSnapshot copied = {};
  assert(mailbox.copySnapshot(&copied));
  assert(copied.version == 1);
  assert(copied.remainingMs == 120000);
  assert(std::strcmp(copied.localIp, "192.168.0.51") == 0);
  assert(copied.installStateKnown);
  assert(copied.installStateHealthy);

  OtaRuntimeSnapshot invalidHandshake = snapshot;
  invalidHandshake.productionPending = true;
  invalidHandshake.installStateKnown = false;
  invalidHandshake.installStateHealthy = false;
  assert(!mailbox.publishSnapshot(invalidHandshake));
  invalidHandshake = snapshot;
  invalidHandshake.confirmationPrepared = true;
  assert(!mailbox.publishSnapshot(invalidHandshake));
  OtaRuntimeSnapshot developmentPrepared = snapshot;
  developmentPrepared.developmentPending = true;
  developmentPrepared.confirmationPrepared = true;
  assert(mailbox.publishSnapshot(developmentPrepared));
  invalidHandshake = developmentPrepared;
  invalidHandshake.productionPending = true;
  assert(!mailbox.publishSnapshot(invalidHandshake));

  assert(mailbox.requestPhysicallyConfirmedDevelopmentOpen());
  assert(!mailbox.requestPhysicallyConfirmedDevelopmentOpen());
  assert(mailbox.takePhysicallyConfirmedDevelopmentOpen());
  assert(!mailbox.takePhysicallyConfirmedDevelopmentOpen());

  OtaSafetyAbortRequest abort = {};
  abort.maximumMainLoopGapMs = 499;
  assert(!mailbox.requestSafetyAbort(abort));
  abort.maximumMainLoopGapMs = 500;
  assert(mailbox.requestSafetyAbort(abort));
  OtaSafetyAbortRequest deliveredAbort = {};
  assert(mailbox.takeSafetyAbort(&deliveredAbort));
  assert(deliveredAbort.maximumMainLoopGapMs == 500);
  assert(!mailbox.takeSafetyAbort(&deliveredAbort));

  abort = {};
  abort.invalidMicrophoneWindows = 6;
  abort.totalMicrophoneWindows = 6;
  assert(!mailbox.requestSafetyAbort(abort));
  abort.consecutiveInvalidMicrophoneWindows = 6;
  assert(mailbox.requestSafetyAbort(abort));
  assert(mailbox.takeSafetyAbort(&deliveredAbort));
  assert(deliveredAbort.invalidMicrophoneWindows == 6);
  assert(deliveredAbort.consecutiveInvalidMicrophoneWindows == 6);

  OtaSafetyAbortRequest metrics = {};
  metrics.maximumMainLoopGapMs = 42;
  metrics.invalidMicrophoneWindows = 1;
  metrics.totalMicrophoneWindows = 100;
  assert(mailbox.publishSafetyMetrics(metrics));
  OtaSafetyAbortRequest deliveredMetrics = {};
  assert(mailbox.takeSafetyMetrics(&deliveredMetrics));
  assert(deliveredMetrics.maximumMainLoopGapMs == 42);
  assert(!mailbox.takeSafetyMetrics(&deliveredMetrics));

  assert(mailbox.publishBootValidationNotice(
      OtaBootValidationNotice::kPrepareConfirmation));
  assert(mailbox.publishBootValidationNotice(
      OtaBootValidationNotice::kFailed));
  assert(mailbox.takeBootValidationNotice() ==
         OtaBootValidationNotice::kFailed);
  assert(mailbox.publishBootValidationNotice(
      OtaBootValidationNotice::kPrepareConfirmation));
  assert(mailbox.publishBootValidationNotice(
      OtaBootValidationNotice::kConfirmed));
  assert(mailbox.takeBootValidationNotice() ==
         OtaBootValidationNotice::kConfirmed);
  assert(mailbox.publishBootValidationNotice(
      OtaBootValidationNotice::kConfirmed));
  assert(mailbox.takeBootValidationNotice() ==
         OtaBootValidationNotice::kConfirmed);
  assert(mailbox.takeBootValidationNotice() == OtaBootValidationNotice::kNone);

  std::atomic<bool> done{false};
  std::thread producer([&] {
    for (uint64_t value = 1; value <= 5000; ++value) {
      OtaRuntimeSnapshot update = {};
      update.updatedAtMs = value;
      update.phase = OtaRuntimePhase::kInactive;
      assert(mailbox.publishSnapshot(update));
    }
    done = true;
  });
  while (!done.load()) {
    OtaRuntimeSnapshot observed = {};
    assert(mailbox.copySnapshot(&observed));
    assert(observed.phase == OtaRuntimePhase::kDevelopmentWindowOpen ||
           observed.phase == OtaRuntimePhase::kInactive);
  }
  producer.join();

  assert(std::strcmp(otaRuntimePhaseName(OtaRuntimePhase::kValidating),
                     "validating") == 0);
  assert(std::strcmp(otaRuntimeErrorName(OtaRuntimeError::kNetwork),
                     "network") == 0);
}
