#pragma once

#include <cstddef>
#include <cstdint>

#include "device_config.h"

// Stable, packed wire image used in Preferences. It deliberately does not
// depend on PresenceConfig's compiler layout, padding, or native endianness.
// Layout v1 (all integers little-endian):
//   magic[4], version(u16), payload_length(u16), revision(u64),
//   minimum_on_ms(u32), pir_hold_ms(u32), sound_hold_ms(u32),
//   max_sound_bridge_ms(u32), cooldown_ms(u32), sound_factor_bits(u32),
//   telemetry_interval_ms(u32), upload_batch_size(u16), crc32(u32).
constexpr size_t kDeviceConfigBlobSize = 50;

enum class DeviceConfigBlobError : uint8_t {
  kNone,
  kNullArgument,
  kWrongLength,
  kBadMagic,
  kUnsupportedVersion,
  kBadPayloadLength,
  kChecksumMismatch,
  kInvalidConfig,
  kUnsupportedCapabilities,
};

struct DeviceConfigBlobDecodeResult {
  PresenceConfig config;
  DeviceConfigBlobError error;

  bool ok() const { return error == DeviceConfigBlobError::kNone; }
  explicit operator bool() const { return ok(); }
};

// These pure functions are available in native builds for byte-exact tests.
// encodeDeviceConfigBlob writes exactly kDeviceConfigBlobSize bytes on success
// and leaves output untouched on failure.
DeviceConfigBlobError encodeDeviceConfigBlob(
    const PresenceConfig& config, uint8_t* output, size_t outputCapacity);
DeviceConfigBlobDecodeResult decodeDeviceConfigBlob(const uint8_t* input,
                                                     size_t inputLength);
bool deviceConfigsEqual(const PresenceConfig& left,
                        const PresenceConfig& right);

enum class DeviceConfigStorageResult : uint8_t {
  kOk,
  kUnchanged,
  kNotStored,
  kInvalidConfig,
  kInvalidStoredData,
  kOpenFailed,
  kReadFailed,
  kWriteFailed,
  kVerifyFailed,
  kUnsupportedPlatform,
};

// Runtime config uses its own Preferences namespace and contains no Wi-Fi or
// API credentials. loadStoredDeviceConfig leaves output unchanged unless it
// returns kOk. saveStoredDeviceConfig returns kUnchanged without writing when
// the logically identical snapshot is already durable.
DeviceConfigStorageResult loadStoredDeviceConfig(PresenceConfig* output);
DeviceConfigStorageResult saveStoredDeviceConfig(
    const PresenceConfig& config);
