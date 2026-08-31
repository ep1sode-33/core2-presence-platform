#pragma once

#include <cstdint>

// A single compile-time source for identity that is shared by telemetry,
// diagnostics, health reports, and OTA release packaging. Release builds may
// override the version/build id from PlatformIO/CI without editing source.
#ifndef M5GO_HARDWARE_MODEL
#define M5GO_HARDWARE_MODEL "m5go-classic-esp32-16m"
#endif

#ifndef M5GO_FIRMWARE_VERSION
#define M5GO_FIRMWARE_VERSION "0.7.0-dev"
#endif

#ifndef M5GO_BUILD_ID
#define M5GO_BUILD_ID "local"
#endif

// This fixed-layout, versioned record is deliberately embedded in the loadable
// image. The release tools require exactly one copy in both firmware.bin and
// firmware.elf and bind its three identity fields to the signed manifest.
// Capacities include a trailing NUL; unused bytes must remain zero.
struct __attribute__((packed)) M5goArtifactIdentityRecordV1 {
  std::uint8_t magic[16];
  std::uint8_t formatVersion;
  std::uint8_t hardwareLength;
  std::uint8_t firmwareVersionLength;
  std::uint8_t buildIdLength;
  char hardware[49];
  char firmwareVersion[33];
  char buildId[65];
  std::uint8_t trailer[16];
};

static_assert(sizeof(M5goArtifactIdentityRecordV1) == 183,
              "OTA artifact identity layout changed");

extern const M5goArtifactIdentityRecordV1 kM5goArtifactIdentity;

inline const char* const kM5goHardwareModel = kM5goArtifactIdentity.hardware;
inline const char* const kM5goFirmwareVersion =
    kM5goArtifactIdentity.firmwareVersion;
inline const char* const kM5goBuildId = kM5goArtifactIdentity.buildId;
