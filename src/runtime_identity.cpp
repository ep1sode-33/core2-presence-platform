#include "runtime_identity.h"

#include <esp_mac.h>
#include <esp_system.h>
#include <esp_timer.h>

#include <cstdio>

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

void encodeHex(const uint8_t* source, size_t sourceSize, char* destination) {
  for (size_t index = 0; index < sourceSize; ++index) {
    destination[index * 2] = kHexDigits[source[index] >> 4];
    destination[index * 2 + 1] = kHexDigits[source[index] & 0x0f];
  }
  destination[sourceSize * 2] = '\0';
}

}  // namespace

uint64_t monotonicMillis() {
  return static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
}

RuntimeIdentity createRuntimeIdentity() {
  RuntimeIdentity identity;
  uint8_t baseMac[6] = {};
  identity.deviceIdValid = esp_efuse_mac_get_default(baseMac) == ESP_OK;
  std::snprintf(identity.deviceId, sizeof(identity.deviceId),
                "core2-%02x%02x%02x%02x%02x%02x", baseMac[0], baseMac[1],
                baseMac[2], baseMac[3], baseMac[4], baseMac[5]);

  uint8_t bootRandom[16] = {};
  esp_fill_random(bootRandom, sizeof(bootRandom));
  encodeHex(bootRandom, sizeof(bootRandom), identity.bootId);
  return identity;
}
