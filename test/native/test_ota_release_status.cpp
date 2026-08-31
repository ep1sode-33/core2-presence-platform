#include <cassert>
#include <cstdint>

#include "ota_release_status.h"

namespace {

OtaReleaseStatusIdentity baseline() {
  OtaReleaseStatusIdentity value = {};
  value.deviceId = "m5go-2cbb81eb60";
  value.desiredReleaseId = "rel-0123456789abcdef0123456789abcdef";
  value.runningReleaseId = "rel-0123456789abcdef0123456789abcdef";
  value.previousReleaseId = "rel-fedcba9876543210fedcba9876543210";
  value.lastKnownGoodReleaseId = value.runningReleaseId;
  value.phase = "running";
  value.progressPercent = 100;
  value.lastError = nullptr;
  value.rollbackOutcome = "not_needed";
  value.firmwareVersion = "0.7.0";
  value.buildId = "git:0123456789ab";
  return value;
}

}  // namespace

int main() {
  const OtaReleaseStatusIdentity firstBoot = baseline();
  const OtaReleaseStatusIdentity secondBoot = baseline();
  const uint64_t stable = otaReleaseStatusIdentityHash(firstBoot);
  // boot_id is intentionally not an input: this is the same payload after a
  // reboot and therefore must deduplicate to the same status_id.
  assert(stable == otaReleaseStatusIdentityHash(secondBoot));

  OtaReleaseStatusIdentity changed = baseline();
  changed.deviceId = "m5go-2cbb81eb61";
  assert(stable != otaReleaseStatusIdentityHash(changed));
  changed = baseline();
  changed.firmwareVersion = "0.7.1";
  assert(stable != otaReleaseStatusIdentityHash(changed));
  changed = baseline();
  changed.buildId = "git:0123456789ac";
  assert(stable != otaReleaseStatusIdentityHash(changed));
  changed = baseline();
  changed.runningReleaseId = nullptr;
  assert(stable != otaReleaseStatusIdentityHash(changed));
  changed = baseline();
  changed.desiredReleaseId = "rel-1123456789abcdef0123456789abcdef";
  assert(stable != otaReleaseStatusIdentityHash(changed));
  changed = baseline();
  changed.previousReleaseId = "rel-0123456789abcdef0123456789abcdef";
  assert(stable != otaReleaseStatusIdentityHash(changed));
  changed = baseline();
  changed.lastKnownGoodReleaseId =
      "rel-fedcba9876543210fedcba9876543210";
  assert(stable != otaReleaseStatusIdentityHash(changed));
  changed = baseline();
  changed.phase = "reboot_pending";
  assert(stable != otaReleaseStatusIdentityHash(changed));
  changed = baseline();
  changed.progressPercent = 99;
  assert(stable != otaReleaseStatusIdentityHash(changed));
  changed = baseline();
  changed.lastError = "image_rejected";
  assert(stable != otaReleaseStatusIdentityHash(changed));
  changed = baseline();
  changed.rollbackOutcome = "succeeded";
  assert(stable != otaReleaseStatusIdentityHash(changed));

  // Null and empty are wire-equivalent because nullableJsonString emits null
  // for both; preserving that equivalence avoids a false idempotency split.
  changed = baseline();
  changed.lastError = "";
  assert(otaReleaseStatusIdentityHash(changed) ==
         otaReleaseStatusIdentityHash(baseline()));
  return 0;
}
