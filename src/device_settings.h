#pragma once

#include <cstddef>
#include <cstdint>

struct DeviceSettings {
  static constexpr size_t kMaxSsidBytes = 32;
  static constexpr size_t kMaxPasswordBytes = 63;
  static constexpr size_t kMaxBaseUrlBytes = 128;
  static constexpr size_t kMaxTokenBytes = 256;
  static constexpr size_t kOtaSecretBytes = 43;

  char ssid[kMaxSsidBytes + 1] = {};
  char password[kMaxPasswordBytes + 1] = {};
  char baseUrl[kMaxBaseUrlBytes + 1] = {};
  char token[kMaxTokenBytes + 1] = {};
  char otaSecret[kOtaSecretBytes + 1] = {};
};

enum class DeviceSettingsStorageResult : uint8_t {
  kOk,
  kNotConfigured,
  kInvalidSettings,
  kOpenFailed,
  kReadFailed,
  kWriteFailed,
  kUnsupportedPlatform,
};

// Both functions leave secrets inside DeviceSettings only; they never log them.
// loadDeviceSettings leaves output unchanged unless it returns kOk.
DeviceSettingsStorageResult loadDeviceSettings(DeviceSettings* output);
DeviceSettingsStorageResult saveDeviceSettings(const DeviceSettings& settings);
