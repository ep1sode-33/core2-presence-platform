#include "firmware_info.h"

static_assert(sizeof(M5GO_HARDWARE_MODEL) - 1 <= 48,
              "M5GO_HARDWARE_MODEL exceeds marker capacity");
static_assert(sizeof(M5GO_FIRMWARE_VERSION) - 1 <= 32,
              "M5GO_FIRMWARE_VERSION exceeds marker capacity");
static_assert(sizeof(M5GO_BUILD_ID) - 1 <= 64,
              "M5GO_BUILD_ID exceeds marker capacity");

// The unusual bookends let offline tooling find the record without relying on
// ELF symbols while the fixed-width zero-padded fields make parsing unique.
// kM5goFirmwareVersion/kM5goBuildId reference this object, so the linker must
// retain the same record that is inspected before an OTA bundle is signed.
const M5goArtifactIdentityRecordV1 kM5goArtifactIdentity
    __attribute__((used, section(".rodata.m5go_artifact_identity"))) = {
        {0x89, 'M',  '5',  'G',  'O', '-', 'F', 'W',
         '-',  'I',  'D',  '\r', '\n', 0x1a, '\n', 0x7f},
        1,
        sizeof(M5GO_HARDWARE_MODEL) - 1,
        sizeof(M5GO_FIRMWARE_VERSION) - 1,
        sizeof(M5GO_BUILD_ID) - 1,
        M5GO_HARDWARE_MODEL,
        M5GO_FIRMWARE_VERSION,
        M5GO_BUILD_ID,
        {0xff, 'M', '5', 'G', 'O', '-', 'F', 'W',
         '-',  'I', 'D', '-', 'E', 'N', 'D', 0x00},
};
