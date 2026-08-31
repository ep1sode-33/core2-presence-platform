#include <cassert>
#include <cstdint>
#include <cstring>

#include "ota_boot_validation.h"
#include "ota_dev_window.h"

namespace {

struct FakeBootPlatform {
  OtaRunningImageState state = OtaRunningImageState::kPendingVerify;
  bool inspectionSucceeds = true;
  bool confirmationSucceeds = true;
  bool rollbackSucceeds = true;
  unsigned confirmations = 0;
  unsigned rollbacks = 0;
};

bool inspect(void* context, OtaRunningImageInfo* output) {
  auto* platform = static_cast<FakeBootPlatform*>(context);
  if (!platform->inspectionSucceeds || output == nullptr) {
    return false;
  }
  output->state = platform->state;
  output->identity.partitionAddress = UINT32_C(0x190000);
  output->identity.sha256[0] = 1;
  return true;
}

bool confirm(void* context) {
  auto* platform = static_cast<FakeBootPlatform*>(context);
  ++platform->confirmations;
  return platform->confirmationSucceeds;
}

bool rollback(void* context) {
  auto* platform = static_cast<FakeBootPlatform*>(context);
  ++platform->rollbacks;
  return platform->rollbackSucceeds;
}

OtaBootValidationBackend backend(FakeBootPlatform& platform) {
  return {&platform, inspect, confirm, rollback};
}

OtaBootHealthGates healthyGates() {
  return {true, true, true, true, true, true};
}

void testWeakHookIsOverriddenAndConfirmationIsDelayed() {
  assert(verifyRollbackLater());
  FakeBootPlatform platform;
  OtaDelayedBootValidator validator;
  assert(validator.begin(1000, backend(platform)));
  assert(validator.phase() == OtaBootValidationPhase::kWaiting);
  assert(validator.remainingMs(1000) == 30000);
  assert(validator.poll(1000, healthyGates(), false));
  assert(validator.poll(30999, healthyGates(), false));
  assert(platform.confirmations == 0);
  assert(validator.poll(31000, healthyGates(), false));
  assert(platform.confirmations == 0);
  assert(validator.phase() ==
         OtaBootValidationPhase::kAwaitingPersistence);
  assert(validator.confirmAfterPersistence());
  assert(platform.confirmations == 1);
  assert(validator.phase() == OtaBootValidationPhase::kConfirmed);
}

void testHardFailureRequestsRollbackAndNetworkIsNotAGate() {
  FakeBootPlatform platform;
  OtaDelayedBootValidator validator;
  assert(validator.begin(0, backend(platform)));
  OtaBootHealthGates gates = healthyGates();
  // There is deliberately no Wi-Fi/backend/weather bit in the gate structure.
  assert(validator.poll(0, gates, false));
  assert(validator.poll(30000, gates, false));
  assert(validator.confirmAfterPersistence());
  assert(validator.phase() == OtaBootValidationPhase::kConfirmed);

  FakeBootPlatform failedPlatform;
  OtaDelayedBootValidator failed;
  assert(failed.begin(0, backend(failedPlatform)));
  assert(!failed.poll(1, gates, true));
  assert(failedPlatform.rollbacks == 1);
  assert(failed.phase() == OtaBootValidationPhase::kRollbackRequested);
}

void testHealthWindowRestartsWhenALocalGateDrops() {
  FakeBootPlatform platform;
  OtaDelayedBootValidator validator;
  OtaBootHealthGates gates = healthyGates();
  assert(validator.begin(0, backend(platform)));
  assert(validator.poll(0, gates, false));
  assert(validator.poll(20000, gates, false));
  gates.filesystemHealthy = false;
  assert(validator.poll(20001, gates, false));
  gates.filesystemHealthy = true;
  assert(validator.poll(25000, gates, false));
  assert(validator.poll(54999, gates, false));
  assert(platform.confirmations == 0);
  assert(validator.poll(55000, gates, false));
  assert(platform.confirmations == 0);
  assert(validator.confirmAfterPersistence());
  assert(platform.confirmations == 1);
}

void testPersistenceMustPrecedeBootloaderConfirmation() {
  FakeBootPlatform platform;
  OtaDelayedBootValidator validator;
  assert(validator.begin(0, backend(platform)));
  assert(!validator.confirmAfterPersistence());
  assert(platform.confirmations == 0);
  assert(validator.poll(0, healthyGates(), false));
  assert(validator.poll(30000, healthyGates(), false));
  assert(validator.phase() ==
         OtaBootValidationPhase::kAwaitingPersistence);

  OtaBootHealthGates failed = healthyGates();
  failed.filesystemHealthy = false;
  assert(!validator.poll(30001, failed, true));
  assert(platform.confirmations == 0);
  assert(platform.rollbacks == 1);
  assert(validator.phase() == OtaBootValidationPhase::kRollbackRequested);
}

void testOrdinaryBootNeedsNoConfirmation() {
  FakeBootPlatform platform;
  platform.state = OtaRunningImageState::kValid;
  OtaDelayedBootValidator validator;
  assert(validator.begin(0, backend(platform)));
  assert(validator.phase() == OtaBootValidationPhase::kNotPending);
  assert(validator.poll(60000, OtaBootHealthGates{}, false));
  assert(platform.confirmations == 0);
}

void testInspectionFailureHaltsWithoutInvalidatingUnknownImage() {
  FakeBootPlatform platform;
  platform.inspectionSucceeds = false;
  OtaDelayedBootValidator validator;
  assert(!validator.begin(0, backend(platform)));
  assert(validator.phase() == OtaBootValidationPhase::kPlatformError);
  assert(!validator.poll(1, OtaBootHealthGates{}, false));
  assert(platform.rollbacks == 0);
  assert(validator.phase() == OtaBootValidationPhase::kPlatformError);
}

void testUnexpectedRunningStatesHaltWithoutBlindRollback() {
  const OtaRunningImageState unexpected[] = {
      OtaRunningImageState::kNew, OtaRunningImageState::kInvalid,
      OtaRunningImageState::kAborted};
  for (const OtaRunningImageState state : unexpected) {
    FakeBootPlatform platform;
    platform.state = state;
    OtaDelayedBootValidator validator;
    assert(!validator.begin(0, backend(platform)));
    assert(validator.phase() == OtaBootValidationPhase::kPlatformError);
    assert(!validator.poll(1, OtaBootHealthGates{}, false));
    assert(platform.confirmations == 0);
    assert(platform.rollbacks == 0);
  }
}

void testDevelopmentWindowIsPhysicalTimedAndAuthenticated() {
  OtaDevelopmentWindow window;
  const char secret[] = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
  static_assert(sizeof(secret) - 1 == kOtaDevelopmentSecretLength);
  assert(!window.configureSecret("short", 5));
  assert(window.configureSecret(secret, std::strlen(secret)));
  assert(window.openAfterPhysicalConfirmation(100));
  assert(window.remainingMs(100) == 120000);
  window.tick(120099);
  assert(window.phase() == OtaDevelopmentWindowPhase::kOpen);
  window.tick(120100);
  assert(window.phase() == OtaDevelopmentWindowPhase::kClosed);

  assert(window.openAfterPhysicalConfirmation(200000));
  assert(window.noteUploadStarted());
  window.tick(400000);  // An active upload suspends the opening timeout.
  assert(window.phase() == OtaDevelopmentWindowPhase::kUploading);
  assert(window.noteUploadProgress(50, 100));
  assert(window.noteUploadProgress(100, 100));
  window.noteUploadSucceeded();
  assert(window.phase() == OtaDevelopmentWindowPhase::kSucceeded);

  // ArduinoOTA can call onEnd without delivering an exact final progress event.
  assert(window.openAfterPhysicalConfirmation(500000));
  assert(window.noteUploadStarted());
  window.noteUploadSucceeded();
  assert(window.phase() == OtaDevelopmentWindowPhase::kSucceeded);

  const uint8_t lan[] = {192, 168, 0, 46};
  const uint8_t other[] = {192, 168, 1, 46};
  assert(otaIpv4IsTrustedLan(lan));
  assert(!otaIpv4IsTrustedLan(other));
}

}  // namespace

int main() {
  testWeakHookIsOverriddenAndConfirmationIsDelayed();
  testHardFailureRequestsRollbackAndNetworkIsNotAGate();
  testHealthWindowRestartsWhenALocalGateDrops();
  testPersistenceMustPrecedeBootloaderConfirmation();
  testOrdinaryBootNeedsNoConfirmation();
  testInspectionFailureHaltsWithoutInvalidatingUnknownImage();
  testUnexpectedRunningStatesHaltWithoutBlindRollback();
  testDevelopmentWindowIsPhysicalTimedAndAuthenticated();
  return 0;
}
