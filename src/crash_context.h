#pragma once

#include <cstddef>
#include <cstdint>

// Durable identity of the firmware instance that was running during a crash.
// bootId is the 128-bit lowercase-hex identifier generated once per boot;
// buildId uses the same canonical alphabet and bound as the signed OTA
// manifest. Capacities include the trailing NUL.
constexpr size_t kCrashContextBootIdLength = 32;
constexpr size_t kCrashContextBootIdCapacity =
    kCrashContextBootIdLength + 1;
constexpr size_t kCrashContextBuildIdMaximumLength = 64;
constexpr size_t kCrashContextBuildIdCapacity =
    kCrashContextBuildIdMaximumLength + 1;
constexpr uint64_t kCrashContextMaximumGeneration =
    UINT64_C(0x7fffffffffffffff);

struct CrashBootContext {
  char bootId[kCrashContextBootIdCapacity] = {};
  char buildId[kCrashContextBuildIdCapacity] = {};
  uint64_t generation = 0;
};

bool crashBootContextIsValid(const CrashBootContext& context);
bool crashBootContextsEqual(const CrashBootContext& left,
                            const CrashBootContext& right);

// Stable packed wire image used for each Preferences slot. It deliberately
// does not depend on compiler padding or native endianness.
//
// Layout v1 (integers are little-endian):
//   magic[4], version(u16), payload_length(u16), generation(u64),
//   boot_id_length(u8), build_id_length(u8), reserved(u16),
//   boot_id[32], build_id[64], crc32(u32).
constexpr size_t kCrashContextBlobSize = 120;

enum class CrashContextBlobError : uint8_t {
  kNone = 0,
  kNullArgument,
  kWrongLength,
  kBadMagic,
  kUnsupportedVersion,
  kBadPayloadLength,
  kChecksumMismatch,
  kInvalidRecord,
};

CrashContextBlobError encodeCrashBootContext(
    const CrashBootContext& context, uint8_t* output,
    size_t outputCapacity);
CrashContextBlobError decodeCrashBootContext(
    const uint8_t* input, size_t inputLength, CrashBootContext* output);

// The callback form makes the complete rotation policy natively testable.
// A successful read may report any length; the decoder rejects non-v1 blobs.
// writeSlot must not return true until the whole value is durable enough to be
// read back through readSlot.
enum class CrashContextSlotReadStatus : uint8_t {
  kOk = 0,
  kMissing,
  kReadFailed,
};

struct CrashContextStorageOps {
  void* context = nullptr;
  CrashContextSlotReadStatus (*readSlot)(void* context, uint8_t slot,
                                         uint8_t* output,
                                         size_t outputCapacity,
                                         size_t* outputLength) = nullptr;
  bool (*writeSlot)(void* context, uint8_t slot, const uint8_t* input,
                    size_t inputLength) = nullptr;
};

enum class PreviousCrashContextStatus : uint8_t {
  kAvailable = 0,
  kNotStored,
  kInvalidStoredData,
  kReadFailed,
};

enum class CrashContextStorageResult : uint8_t {
  kOk = 0,
  kInvalidCurrentContext,
  kInvalidStorageOps,
  kOpenFailed,
  kReadFailed,
  kWriteFailed,
  kVerifyFailed,
  kGenerationExhausted,
  kUnsupportedPlatform,
};

struct CrashContextRotationResult {
  CrashContextStorageResult storage =
      CrashContextStorageResult::kUnsupportedPlatform;
  PreviousCrashContextStatus previousStatus =
      PreviousCrashContextStatus::kNotStored;
  CrashBootContext previous = {};
  uint64_t persistedGeneration = 0;

  bool currentPersisted() const {
    return storage == CrashContextStorageResult::kOk;
  }
  bool hasPrevious() const {
    return currentPersisted() &&
           previousStatus == PreviousCrashContextStatus::kAvailable;
  }
};

CrashContextRotationResult rotateCrashContextWithStorage(
    const char* currentBootId, const char* currentBuildId,
    const CrashContextStorageOps& storage);

// ESP32 convenience wrapper backed by the dedicated "m5crashctx" Preferences
// namespace. Call this immediately after generating the current boot id and
// before initializing components that may crash. Never substitute the current
// identity when hasPrevious() is false; doing so would misattribute a dump.
CrashContextRotationResult captureAndRotateCrashContext(
    const char* currentBootId, const char* currentBuildId);

// Once a flash core dump is observed, its originating identity must survive
// more than one recovery boot. Otherwise an offline reboot would rotate the
// ordinary boot journal and misattribute the still-pending dump.
constexpr size_t kCrashResetReasonMaximumLength = 64;
constexpr size_t kCrashResetReasonCapacity =
    kCrashResetReasonMaximumLength + 1;

struct CrashDumpAttribution {
  char bootId[kCrashContextBootIdCapacity] = {};
  char buildId[kCrashContextBuildIdCapacity] = {};
  char resetReason[kCrashResetReasonCapacity] = {};
};

bool crashDumpAttributionIsValid(const CrashDumpAttribution& attribution);
bool crashDumpAttributionsEqual(const CrashDumpAttribution& left,
                                const CrashDumpAttribution& right);

constexpr size_t kCrashDumpAttributionBlobSize = 176;

CrashContextBlobError encodeCrashDumpAttribution(
    const CrashDumpAttribution& attribution, uint8_t* output,
    size_t outputCapacity);
CrashContextBlobError decodeCrashDumpAttribution(
    const uint8_t* input, size_t inputLength,
    CrashDumpAttribution* output);

enum class CrashDumpAttributionStorageResult : uint8_t {
  kOk = 0,
  kNotStored,
  kInvalidAttribution,
  kInvalidStoredData,
  kOpenFailed,
  kReadFailed,
  kWriteFailed,
  kVerifyFailed,
  kRemoveFailed,
  kUnsupportedPlatform,
};

CrashDumpAttributionStorageResult loadPinnedCrashDumpAttribution(
    CrashDumpAttribution* output);
CrashDumpAttributionStorageResult savePinnedCrashDumpAttribution(
    const CrashDumpAttribution& attribution);
CrashDumpAttributionStorageResult clearPinnedCrashDumpAttribution();
