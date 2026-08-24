#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>

#include "device_config_storage.h"

namespace {

PresenceConfig configAtRevision(uint64_t revision) {
  PresenceConfig config = defaultPresenceConfig();
  config.revision = revision;
  config.minimumOnMs = 12345;
  config.pirHoldMs = 54321;
  config.soundHoldMs = 23456;
  config.maxSoundBridgeMs = 345678;
  config.cooldownMs = 4567;
  config.soundFactor = 2.375f;
  config.telemetryIntervalMs = 4321;
  config.uploadBatchSize = 17;
  return config;
}

void testRoundTripAndStableBytes() {
  const PresenceConfig original = configAtRevision(0x0102030405060708ULL);
  std::array<uint8_t, kDeviceConfigBlobSize> blob = {};
  assert(encodeDeviceConfigBlob(original, blob.data(), blob.size()) ==
         DeviceConfigBlobError::kNone);

  // Header and revision assert the format's byte order, not just symmetry.
  assert(blob[0] == 'M' && blob[1] == '5' && blob[2] == 'C' &&
         blob[3] == 'F');
  assert(blob[4] == 1 && blob[5] == 0);
  assert(blob[6] == 38 && blob[7] == 0);
  assert(blob[8] == 0x08 && blob[9] == 0x07 && blob[10] == 0x06 &&
         blob[15] == 0x01);

  const DeviceConfigBlobDecodeResult decoded =
      decodeDeviceConfigBlob(blob.data(), blob.size());
  assert(decoded.ok());
  assert(deviceConfigsEqual(decoded.config, original));

  std::array<uint8_t, kDeviceConfigBlobSize> second = {};
  assert(encodeDeviceConfigBlob(decoded.config, second.data(), second.size()) ==
         DeviceConfigBlobError::kNone);
  assert(second == blob);
}

void testCorruptionAndHeaderRejection() {
  const PresenceConfig original = configAtRevision(7);
  std::array<uint8_t, kDeviceConfigBlobSize> blob = {};
  assert(encodeDeviceConfigBlob(original, blob.data(), blob.size()) ==
         DeviceConfigBlobError::kNone);

  std::array<uint8_t, kDeviceConfigBlobSize> corrupt = blob;
  corrupt[24] ^= 0x40;
  assert(decodeDeviceConfigBlob(corrupt.data(), corrupt.size()).error ==
         DeviceConfigBlobError::kChecksumMismatch);

  corrupt = blob;
  corrupt.back() ^= 0x01;
  assert(decodeDeviceConfigBlob(corrupt.data(), corrupt.size()).error ==
         DeviceConfigBlobError::kChecksumMismatch);

  corrupt = blob;
  corrupt[0] = 'X';
  assert(decodeDeviceConfigBlob(corrupt.data(), corrupt.size()).error ==
         DeviceConfigBlobError::kBadMagic);

  corrupt = blob;
  corrupt[4] = 2;
  assert(decodeDeviceConfigBlob(corrupt.data(), corrupt.size()).error ==
         DeviceConfigBlobError::kUnsupportedVersion);

  corrupt = blob;
  corrupt[6] = 0;
  assert(decodeDeviceConfigBlob(corrupt.data(), corrupt.size()).error ==
         DeviceConfigBlobError::kBadPayloadLength);

  assert(decodeDeviceConfigBlob(blob.data(), blob.size() - 1).error ==
         DeviceConfigBlobError::kWrongLength);
  assert(decodeDeviceConfigBlob(nullptr, blob.size()).error ==
         DeviceConfigBlobError::kNullArgument);
}

void testValidationAndFailureAtomicity() {
  PresenceConfig invalid = configAtRevision(9);
  invalid.uploadBatchSize = 0;
  std::array<uint8_t, kDeviceConfigBlobSize> output;
  output.fill(0xa5);
  const auto before = output;
  assert(encodeDeviceConfigBlob(invalid, output.data(), output.size()) ==
         DeviceConfigBlobError::kInvalidConfig);
  assert(output == before);

  PresenceConfig valid = configAtRevision(10);
  assert(encodeDeviceConfigBlob(valid, output.data(), output.size() - 1) ==
         DeviceConfigBlobError::kWrongLength);
  assert(output == before);
  assert(encodeDeviceConfigBlob(valid, nullptr, output.size()) ==
         DeviceConfigBlobError::kNullArgument);

  assert(loadStoredDeviceConfig(&valid) ==
         DeviceConfigStorageResult::kUnsupportedPlatform);
  assert(saveStoredDeviceConfig(valid) ==
         DeviceConfigStorageResult::kUnsupportedPlatform);
}

}  // namespace

int main() {
  testRoundTripAndStableBytes();
  testCorruptionAndHeaderRejection();
  testValidationAndFailureAtomicity();
  return 0;
}
