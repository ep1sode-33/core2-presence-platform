#include "device_config_storage.h"

#include <cstring>

namespace {

constexpr uint8_t kMagic[] = {'M', '5', 'C', 'F'};
constexpr uint16_t kFormatVersion = 1;
constexpr uint16_t kPayloadLength = 38;
constexpr size_t kChecksumOffset = kDeviceConfigBlobSize - sizeof(uint32_t);

void putU16(uint8_t* destination, uint16_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8U);
}

void putU32(uint8_t* destination, uint32_t value) {
  for (size_t index = 0; index < sizeof(value); ++index) {
    destination[index] = static_cast<uint8_t>(value >> (index * 8U));
  }
}

void putU64(uint8_t* destination, uint64_t value) {
  for (size_t index = 0; index < sizeof(value); ++index) {
    destination[index] = static_cast<uint8_t>(value >> (index * 8U));
  }
}

uint16_t getU16(const uint8_t* source) {
  return static_cast<uint16_t>(source[0]) |
         (static_cast<uint16_t>(source[1]) << 8U);
}

uint32_t getU32(const uint8_t* source) {
  uint32_t value = 0;
  for (size_t index = 0; index < sizeof(value); ++index) {
    value |= static_cast<uint32_t>(source[index]) << (index * 8U);
  }
  return value;
}

uint64_t getU64(const uint8_t* source) {
  uint64_t value = 0;
  for (size_t index = 0; index < sizeof(value); ++index) {
    value |= static_cast<uint64_t>(source[index]) << (index * 8U);
  }
  return value;
}

uint32_t crc32(const uint8_t* bytes, size_t length) {
  uint32_t crc = 0xffffffffU;
  for (size_t index = 0; index < length; ++index) {
    crc ^= bytes[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      const uint32_t lowBitMask =
          static_cast<uint32_t>(0U - (crc & 1U));
      crc = (crc >> 1U) ^ (0xedb88320U & lowBitMask);
    }
  }
  return crc ^ 0xffffffffU;
}

DeviceConfigBlobDecodeResult blobFailure(DeviceConfigBlobError error) {
  return {defaultPresenceConfig(), error};
}

DeviceConfigBlobError validateForStorage(const PresenceConfig& config) {
  if (validatePresenceConfig(config) !=
      PresenceConfigValidationError::kNone) {
    return DeviceConfigBlobError::kInvalidConfig;
  }
  if (validatePresenceConfigDeviceCapabilities(config) !=
      PresenceConfigCapabilityError::kNone) {
    return DeviceConfigBlobError::kUnsupportedCapabilities;
  }
  return DeviceConfigBlobError::kNone;
}

}  // namespace

bool deviceConfigsEqual(const PresenceConfig& left,
                        const PresenceConfig& right) {
  return left.revision == right.revision &&
         left.minimumOnMs == right.minimumOnMs &&
         left.pirHoldMs == right.pirHoldMs &&
         left.soundHoldMs == right.soundHoldMs &&
         left.maxSoundBridgeMs == right.maxSoundBridgeMs &&
         left.cooldownMs == right.cooldownMs &&
         left.soundFactor == right.soundFactor &&
         left.telemetryIntervalMs == right.telemetryIntervalMs &&
         left.uploadBatchSize == right.uploadBatchSize;
}

DeviceConfigBlobError encodeDeviceConfigBlob(
    const PresenceConfig& config, uint8_t* output, size_t outputCapacity) {
  if (output == nullptr) {
    return DeviceConfigBlobError::kNullArgument;
  }
  if (outputCapacity < kDeviceConfigBlobSize) {
    return DeviceConfigBlobError::kWrongLength;
  }
  const DeviceConfigBlobError validation = validateForStorage(config);
  if (validation != DeviceConfigBlobError::kNone) {
    return validation;
  }

  // Construct separately so an error can never expose a partial blob.
  uint8_t blob[kDeviceConfigBlobSize] = {};
  std::memcpy(blob, kMagic, sizeof(kMagic));
  putU16(blob + 4, kFormatVersion);
  putU16(blob + 6, kPayloadLength);
  putU64(blob + 8, config.revision);
  putU32(blob + 16, config.minimumOnMs);
  putU32(blob + 20, config.pirHoldMs);
  putU32(blob + 24, config.soundHoldMs);
  putU32(blob + 28, config.maxSoundBridgeMs);
  putU32(blob + 32, config.cooldownMs);
  uint32_t soundFactorBits = 0;
  static_assert(sizeof(soundFactorBits) == sizeof(config.soundFactor),
                "float storage requires binary32");
  std::memcpy(&soundFactorBits, &config.soundFactor,
              sizeof(soundFactorBits));
  putU32(blob + 36, soundFactorBits);
  putU32(blob + 40, config.telemetryIntervalMs);
  putU16(blob + 44, config.uploadBatchSize);
  putU32(blob + kChecksumOffset, crc32(blob, kChecksumOffset));
  std::memcpy(output, blob, sizeof(blob));
  return DeviceConfigBlobError::kNone;
}

DeviceConfigBlobDecodeResult decodeDeviceConfigBlob(const uint8_t* input,
                                                     size_t inputLength) {
  if (input == nullptr) {
    return blobFailure(DeviceConfigBlobError::kNullArgument);
  }
  if (inputLength != kDeviceConfigBlobSize) {
    return blobFailure(DeviceConfigBlobError::kWrongLength);
  }
  if (std::memcmp(input, kMagic, sizeof(kMagic)) != 0) {
    return blobFailure(DeviceConfigBlobError::kBadMagic);
  }
  if (getU16(input + 4) != kFormatVersion) {
    return blobFailure(DeviceConfigBlobError::kUnsupportedVersion);
  }
  if (getU16(input + 6) != kPayloadLength) {
    return blobFailure(DeviceConfigBlobError::kBadPayloadLength);
  }
  if (getU32(input + kChecksumOffset) != crc32(input, kChecksumOffset)) {
    return blobFailure(DeviceConfigBlobError::kChecksumMismatch);
  }

  PresenceConfig config = {};
  config.revision = getU64(input + 8);
  config.minimumOnMs = getU32(input + 16);
  config.pirHoldMs = getU32(input + 20);
  config.soundHoldMs = getU32(input + 24);
  config.maxSoundBridgeMs = getU32(input + 28);
  config.cooldownMs = getU32(input + 32);
  const uint32_t soundFactorBits = getU32(input + 36);
  std::memcpy(&config.soundFactor, &soundFactorBits,
              sizeof(config.soundFactor));
  config.telemetryIntervalMs = getU32(input + 40);
  config.uploadBatchSize = getU16(input + 44);

  const DeviceConfigBlobError validation = validateForStorage(config);
  if (validation != DeviceConfigBlobError::kNone) {
    return blobFailure(validation);
  }
  return {config, DeviceConfigBlobError::kNone};
}

#if defined(ARDUINO_ARCH_ESP32)

#include <Preferences.h>

namespace {

constexpr char kPreferencesNamespace[] = "m5cfg";
constexpr char kActiveSlotKey[] = "active";
constexpr char kSlotKeys[][7] = {"a_blob", "b_blob"};
constexpr uint8_t kNoSlot = 0xff;

enum class SlotReadResult : uint8_t {
  kOk,
  kMissing,
  kInvalid,
  kReadFailed,
};

struct SlotSnapshot {
  SlotReadResult result = SlotReadResult::kMissing;
  PresenceConfig config = {};
};

SlotSnapshot readSlot(Preferences& preferences, uint8_t slot) {
  SlotSnapshot snapshot;
  const size_t length = preferences.getBytesLength(kSlotKeys[slot]);
  if (length == 0) {
    return snapshot;
  }
  if (length != kDeviceConfigBlobSize) {
    snapshot.result = SlotReadResult::kInvalid;
    return snapshot;
  }

  uint8_t blob[kDeviceConfigBlobSize] = {};
  if (preferences.getBytes(kSlotKeys[slot], blob, sizeof(blob)) !=
      sizeof(blob)) {
    snapshot.result = SlotReadResult::kReadFailed;
    return snapshot;
  }
  const DeviceConfigBlobDecodeResult decoded =
      decodeDeviceConfigBlob(blob, sizeof(blob));
  if (!decoded.ok()) {
    snapshot.result = SlotReadResult::kInvalid;
    return snapshot;
  }
  snapshot.result = SlotReadResult::kOk;
  snapshot.config = decoded.config;
  return snapshot;
}

uint8_t selectReadableSlot(uint8_t activeSlot,
                           const SlotSnapshot snapshots[2]) {
  if (activeSlot <= 1 &&
      snapshots[activeSlot].result == SlotReadResult::kOk) {
    return activeSlot;
  }
  if (activeSlot <= 1) {
    const uint8_t fallback = activeSlot == 0 ? 1 : 0;
    return snapshots[fallback].result == SlotReadResult::kOk ? fallback
                                                             : kNoSlot;
  }
  if (snapshots[0].result != SlotReadResult::kOk) {
    return snapshots[1].result == SlotReadResult::kOk ? 1 : kNoSlot;
  }
  if (snapshots[1].result != SlotReadResult::kOk) {
    return 0;
  }
  // The marker is itself invalid. API revisions are monotonic, so the higher
  // valid snapshot is the safest recovery choice; equal revisions prefer A.
  return snapshots[1].config.revision > snapshots[0].config.revision ? 1 : 0;
}

DeviceConfigStorageResult failedReadResult(
    const SlotSnapshot snapshots[2]) {
  if (snapshots[0].result == SlotReadResult::kReadFailed ||
      snapshots[1].result == SlotReadResult::kReadFailed) {
    return DeviceConfigStorageResult::kReadFailed;
  }
  if (snapshots[0].result == SlotReadResult::kInvalid ||
      snapshots[1].result == SlotReadResult::kInvalid) {
    return DeviceConfigStorageResult::kInvalidStoredData;
  }
  return DeviceConfigStorageResult::kNotStored;
}

void readBothSlots(Preferences& preferences, SlotSnapshot snapshots[2]) {
  snapshots[0] = readSlot(preferences, 0);
  snapshots[1] = readSlot(preferences, 1);
}

}  // namespace

DeviceConfigStorageResult loadStoredDeviceConfig(PresenceConfig* output) {
  if (output == nullptr) {
    return DeviceConfigStorageResult::kInvalidConfig;
  }

  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, true)) {
    // Preferences cannot open a never-created namespace read-only. Create the
    // empty namespace once so first boot is distinct from a persistent error.
    if (!preferences.begin(kPreferencesNamespace, false)) {
      return DeviceConfigStorageResult::kOpenFailed;
    }
    preferences.end();
    return DeviceConfigStorageResult::kNotStored;
  }

  SlotSnapshot snapshots[2];
  readBothSlots(preferences, snapshots);
  const uint8_t selected = selectReadableSlot(
      preferences.getUChar(kActiveSlotKey, kNoSlot), snapshots);
  preferences.end();
  if (selected == kNoSlot) {
    return failedReadResult(snapshots);
  }
  *output = snapshots[selected].config;
  return DeviceConfigStorageResult::kOk;
}

DeviceConfigStorageResult saveStoredDeviceConfig(
    const PresenceConfig& config) {
  uint8_t blob[kDeviceConfigBlobSize] = {};
  if (encodeDeviceConfigBlob(config, blob, sizeof(blob)) !=
      DeviceConfigBlobError::kNone) {
    return DeviceConfigStorageResult::kInvalidConfig;
  }

  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, false)) {
    return DeviceConfigStorageResult::kOpenFailed;
  }

  SlotSnapshot snapshots[2];
  readBothSlots(preferences, snapshots);
  const uint8_t activeSlot =
      preferences.getUChar(kActiveSlotKey, kNoSlot);
  const uint8_t selected = selectReadableSlot(activeSlot, snapshots);
  if (selected != kNoSlot &&
      deviceConfigsEqual(snapshots[selected].config, config)) {
    preferences.end();
    return DeviceConfigStorageResult::kUnchanged;
  }

  // The selected slot is never modified in place. The complete checksummed
  // snapshot is written and read back in the other slot before a one-byte
  // active marker commits it. A reset before that marker preserves the old
  // snapshot; a reset after it selects the fully verified new snapshot.
  const uint8_t target = selected == 0 ? 1 : 0;
  if (preferences.putBytes(kSlotKeys[target], blob, sizeof(blob)) !=
      sizeof(blob)) {
    preferences.end();
    return DeviceConfigStorageResult::kWriteFailed;
  }
  const SlotSnapshot verified = readSlot(preferences, target);
  if (verified.result != SlotReadResult::kOk ||
      !deviceConfigsEqual(verified.config, config)) {
    preferences.end();
    return DeviceConfigStorageResult::kVerifyFailed;
  }
  if (preferences.putUChar(kActiveSlotKey, target) != sizeof(uint8_t) ||
      preferences.getUChar(kActiveSlotKey, kNoSlot) != target) {
    preferences.end();
    return DeviceConfigStorageResult::kWriteFailed;
  }
  preferences.end();
  return DeviceConfigStorageResult::kOk;
}

#else

DeviceConfigStorageResult loadStoredDeviceConfig(PresenceConfig*) {
  return DeviceConfigStorageResult::kUnsupportedPlatform;
}

DeviceConfigStorageResult saveStoredDeviceConfig(const PresenceConfig&) {
  return DeviceConfigStorageResult::kUnsupportedPlatform;
}

#endif
