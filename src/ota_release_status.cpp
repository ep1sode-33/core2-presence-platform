#include "ota_release_status.h"

#include <cstddef>
#include <cstdint>

namespace {

constexpr uint64_t kFnvOffsetBasis = UINT64_C(1469598103934665603);
constexpr uint64_t kFnvPrime = UINT64_C(1099511628211);

void addByte(uint64_t* hash, uint8_t value) {
  *hash = (*hash ^ value) * kFnvPrime;
}

void addText(uint64_t* hash, const char* value) {
  for (const unsigned char* cursor =
           reinterpret_cast<const unsigned char*>(value);
       *cursor != '\0'; ++cursor) {
    addByte(hash, *cursor);
  }
}

void addFieldName(uint64_t* hash, const char* name) {
  addByte(hash, 0xf1U);
  addText(hash, name);
  addByte(hash, 0xf2U);
}

void addNullableString(uint64_t* hash, const char* name, const char* value) {
  addFieldName(hash, name);
  // The wire encoder serializes both nullptr and an empty string as JSON null.
  if (value == nullptr || value[0] == '\0') {
    addByte(hash, 0x00U);
    return;
  }
  addByte(hash, 0x01U);
  addText(hash, value);
  addByte(hash, 0x00U);
}

void addRequiredString(uint64_t* hash, const char* name, const char* value) {
  addFieldName(hash, name);
  addByte(hash, 0x01U);
  if (value != nullptr) {
    addText(hash, value);
  }
  addByte(hash, 0x00U);
}

void addProgress(uint64_t* hash, int progress) {
  addFieldName(hash, "progress_percent");
  if (progress < 0) {
    addByte(hash, 0x00U);
    return;
  }
  addByte(hash, 0x01U);
  const uint32_t value = static_cast<uint32_t>(progress);
  for (size_t index = 0; index < sizeof(value); ++index) {
    addByte(hash, static_cast<uint8_t>(value >> (index * 8U)));
  }
}

}  // namespace

uint64_t otaReleaseStatusIdentityHash(
    const OtaReleaseStatusIdentity& identity) {
  uint64_t hash = kFnvOffsetBasis;
  addFieldName(&hash, "schema_version");
  addByte(&hash, 1U);
  addRequiredString(&hash, "device_id", identity.deviceId);
  addNullableString(&hash, "desired_release_id", identity.desiredReleaseId);
  addNullableString(&hash, "running_release_id", identity.runningReleaseId);
  addNullableString(&hash, "previous_release_id", identity.previousReleaseId);
  addNullableString(&hash, "last_known_good_release_id",
                    identity.lastKnownGoodReleaseId);
  addRequiredString(&hash, "phase", identity.phase);
  addProgress(&hash, identity.progressPercent);
  addNullableString(&hash, "last_error", identity.lastError);
  addRequiredString(&hash, "rollback_outcome", identity.rollbackOutcome);
  addRequiredString(&hash, "firmware_version", identity.firmwareVersion);
  addRequiredString(&hash, "build_id", identity.buildId);
  return hash;
}
