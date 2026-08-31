#include "device_settings.h"

#include "provisioning_protocol.h"

#include <cstring>

#if defined(ARDUINO)
#include <Preferences.h>

namespace {

constexpr char kNamespace[] = "m5pres";
constexpr char kActiveSlotKey[] = "cfg_active";

struct SlotKeys {
  const char* valid;
  const char* ssid;
  const char* password;
  const char* baseUrl;
  const char* token;
  const char* otaSecret;
};

constexpr SlotKeys kSlotKeys[] = {
    {"a_valid", "a_ssid", "a_password", "a_base_url", "a_token",
     "a_ota"},
    {"b_valid", "b_ssid", "b_password", "b_base_url", "b_token",
     "b_ota"},
};

void clearSettings(DeviceSettings* settings) {
  volatile uint8_t* bytes =
      reinterpret_cast<volatile uint8_t*>(settings);
  for (size_t index = 0; index < sizeof(*settings); ++index) {
    bytes[index] = 0;
  }
}

size_t boundedLength(const char* value, size_t capacity) {
  const void* terminator = std::memchr(value, '\0', capacity);
  if (terminator == nullptr) {
    return capacity;
  }
  return static_cast<const char*>(terminator) - value;
}

bool writeField(Preferences& preferences, const char* key, const char* value,
                size_t capacity) {
  const size_t length = boundedLength(value, capacity);
  if (length >= capacity) {
    return false;
  }

  const size_t storedLength = length + 1;
  return preferences.putBytes(key, value, storedLength) == storedLength &&
         preferences.getBytesLength(key) == storedLength;
}

enum class ReadFieldResult : uint8_t {
  kOk,
  kInvalid,
  kReadFailed,
};

ReadFieldResult readField(Preferences& preferences, const char* key,
                          char* destination, size_t capacity) {
  const size_t storedLength = preferences.getBytesLength(key);
  if (storedLength == 0 || storedLength > capacity) {
    return ReadFieldResult::kInvalid;
  }
  if (preferences.getBytes(key, destination, storedLength) != storedLength) {
    return ReadFieldResult::kReadFailed;
  }
  if (destination[storedLength - 1] != '\0' ||
      std::memchr(destination, '\0', storedLength - 1) != nullptr) {
    return ReadFieldResult::kInvalid;
  }
  return ReadFieldResult::kOk;
}

ReadFieldResult readOptionalField(Preferences& preferences, const char* key,
                                  char* destination, size_t capacity) {
  if (preferences.getBytesLength(key) == 0) {
    destination[0] = '\0';
    return ReadFieldResult::kOk;
  }
  return readField(preferences, key, destination, capacity);
}

DeviceSettingsStorageResult readSlot(Preferences& preferences, uint8_t slot,
                                     DeviceSettings* output) {
  const SlotKeys& keys = kSlotKeys[slot];
  if (!preferences.getBool(keys.valid, false)) {
    return DeviceSettingsStorageResult::kNotConfigured;
  }

  DeviceSettings candidate;
  const ReadFieldResult ssid =
      readField(preferences, keys.ssid, candidate.ssid, sizeof(candidate.ssid));
  const ReadFieldResult password =
      readField(preferences, keys.password, candidate.password,
                sizeof(candidate.password));
  const ReadFieldResult baseUrl = readField(
      preferences, keys.baseUrl, candidate.baseUrl, sizeof(candidate.baseUrl));
  const ReadFieldResult token = readField(
      preferences, keys.token, candidate.token, sizeof(candidate.token));
  const ReadFieldResult otaSecret =
      readOptionalField(preferences, keys.otaSecret, candidate.otaSecret,
                        sizeof(candidate.otaSecret));
  if (ssid == ReadFieldResult::kReadFailed ||
      password == ReadFieldResult::kReadFailed ||
      baseUrl == ReadFieldResult::kReadFailed ||
      token == ReadFieldResult::kReadFailed ||
      otaSecret == ReadFieldResult::kReadFailed) {
    return DeviceSettingsStorageResult::kReadFailed;
  }
  if (ssid != ReadFieldResult::kOk || password != ReadFieldResult::kOk ||
      baseUrl != ReadFieldResult::kOk || token != ReadFieldResult::kOk ||
      otaSecret != ReadFieldResult::kOk ||
      normalizeAndValidateDeviceSettings(&candidate) !=
          ProvisioningError::kOk) {
    return DeviceSettingsStorageResult::kInvalidSettings;
  }

  *output = candidate;
  return DeviceSettingsStorageResult::kOk;
}

}  // namespace

DeviceSettingsStorageResult saveDeviceSettings(
    const DeviceSettings& settings) {
  DeviceSettings normalized = settings;
  if (normalizeAndValidateDeviceSettings(&normalized) !=
      ProvisioningError::kOk) {
    clearSettings(&normalized);
    return DeviceSettingsStorageResult::kInvalidSettings;
  }

  Preferences preferences;
  if (!preferences.begin(kNamespace, false)) {
    clearSettings(&normalized);
    return DeviceSettingsStorageResult::kOpenFailed;
  }

  const uint8_t activeSlot = preferences.getUChar(kActiveSlotKey, 0xff);
  const uint8_t targetSlot = activeSlot == 0 ? 1 : 0;
  const SlotKeys& keys = kSlotKeys[targetSlot];

  // Build the inactive slot completely before switching one-byte ownership.
  // A reset at any earlier point leaves the previous active slot untouched.
  if (preferences.putBool(keys.valid, false) != 1) {
    preferences.end();
    clearSettings(&normalized);
    return DeviceSettingsStorageResult::kWriteFailed;
  }

  const bool fieldsWritten =
      writeField(preferences, keys.ssid, normalized.ssid,
                 sizeof(normalized.ssid)) &&
      writeField(preferences, keys.password, normalized.password,
                 sizeof(normalized.password)) &&
      writeField(preferences, keys.baseUrl, normalized.baseUrl,
                 sizeof(normalized.baseUrl)) &&
      writeField(preferences, keys.token, normalized.token,
                 sizeof(normalized.token)) &&
      writeField(preferences, keys.otaSecret, normalized.otaSecret,
                 sizeof(normalized.otaSecret));
  if (!fieldsWritten || preferences.putBool(keys.valid, true) != 1 ||
      preferences.putUChar(kActiveSlotKey, targetSlot) != 1) {
    preferences.end();
    clearSettings(&normalized);
    return DeviceSettingsStorageResult::kWriteFailed;
  }

  preferences.end();
  clearSettings(&normalized);
  return DeviceSettingsStorageResult::kOk;
}

DeviceSettingsStorageResult loadDeviceSettings(DeviceSettings* output) {
  if (output == nullptr) {
    return DeviceSettingsStorageResult::kInvalidSettings;
  }

  Preferences preferences;
  if (!preferences.begin(kNamespace, true)) {
    // A namespace that has never been written cannot be opened read-only.
    // Confirm that read-write open succeeds (and creates the empty namespace)
    // so first boot is distinguishable from an actual NVS failure.
    if (!preferences.begin(kNamespace, false)) {
      return DeviceSettingsStorageResult::kOpenFailed;
    }
    preferences.end();
    return DeviceSettingsStorageResult::kNotConfigured;
  }
  DeviceSettings candidate;
  const uint8_t activeSlot = preferences.getUChar(kActiveSlotKey, 0xff);
  const uint8_t primarySlot = activeSlot <= 1 ? activeSlot : 0;
  const DeviceSettingsStorageResult primary =
      readSlot(preferences, primarySlot, &candidate);
  if (primary == DeviceSettingsStorageResult::kOk) {
    preferences.end();
    *output = candidate;
    clearSettings(&candidate);
    return DeviceSettingsStorageResult::kOk;
  }

  // A fully valid inactive slot is a safe fallback if the active marker or
  // active slot was torn or corrupted.
  const uint8_t fallbackSlot = primarySlot == 0 ? 1 : 0;
  const DeviceSettingsStorageResult fallback =
      readSlot(preferences, fallbackSlot, &candidate);
  preferences.end();
  if (fallback == DeviceSettingsStorageResult::kOk) {
    *output = candidate;
    clearSettings(&candidate);
    return DeviceSettingsStorageResult::kOk;
  }
  clearSettings(&candidate);
  if (primary == DeviceSettingsStorageResult::kReadFailed ||
      fallback == DeviceSettingsStorageResult::kReadFailed) {
    return DeviceSettingsStorageResult::kReadFailed;
  }
  if (primary == DeviceSettingsStorageResult::kInvalidSettings ||
      fallback == DeviceSettingsStorageResult::kInvalidSettings) {
    return DeviceSettingsStorageResult::kInvalidSettings;
  }
  return DeviceSettingsStorageResult::kNotConfigured;
}

#else

DeviceSettingsStorageResult saveDeviceSettings(const DeviceSettings&) {
  return DeviceSettingsStorageResult::kUnsupportedPlatform;
}

DeviceSettingsStorageResult loadDeviceSettings(DeviceSettings*) {
  return DeviceSettingsStorageResult::kUnsupportedPlatform;
}

#endif
