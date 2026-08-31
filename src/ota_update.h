#pragma once

#include <cstddef>
#include <cstdint>

#include "ota_crypto.h"
#include "ota_release.h"

// Keep production downloader buffers small on the classic ESP32 (no PSRAM),
// and give the Core 0 worker a natural point to yield between flash writes.
constexpr size_t kOtaMaximumWriteChunkSize = 4096;

// Caps a sequential production download read at the next flash-sector
// boundary. This guarantees that one esp_ota_write() transaction cannot cause
// more than one new sector erase even when earlier network reads were short.
size_t otaSectorBoundedChunkSize(uint32_t bytesWritten, size_t available,
                                 size_t remaining);

// The classic ESP32 pauses the other core while flash sectors are erased.
// Serialize production flash operations with the 16 ms ADC sampling window so
// the pause is measured as a main-loop gap instead of corrupting a microphone
// window. Initialize once during setup, before the uploader task starts.
bool otaInitializeFlashSensorGuard();
bool otaLockFlashSensorGuard(uint32_t timeoutMs);
void otaUnlockFlashSensorGuard();

struct OtaUpdateBackend {
  void* context = nullptr;
  bool (*begin)(void* context, uint32_t imageSize) = nullptr;
  size_t (*write)(void* context, const uint8_t* bytes, size_t size) = nullptr;
  bool (*finish)(void* context) = nullptr;
  void (*abort)(void* context) = nullptr;
};

struct OtaApplicationImageIdentity {
  uint32_t partitionAddress = 0;
  uint8_t sha256[kOtaSha256Size] = {};
};

bool otaApplicationImageIdentityIsValid(
    const OtaApplicationImageIdentity& identity);
bool otaApplicationImageIdentityEquals(
    const OtaApplicationImageIdentity& left,
    const OtaApplicationImageIdentity& right);

enum class OtaUpdateState : uint8_t {
  kIdle = 0,
  kWriting,
  kReadyToReboot,
  kAborted,
  kFailed,
};

enum class OtaUpdateError : uint8_t {
  kNone = 0,
  kInvalidVerifiedRelease,
  kInvalidBackend,
  kBackendBeginFailed,
  kNotWriting,
  kNullChunk,
  kChunkTooLarge,
  kImageLengthOverflow,
  kBackendShortWrite,
  kHashFailure,
  kImageLengthMismatch,
  kImageDigestMismatch,
  kBackendFinishFailed,
  kOperatorAborted,
};

// Streams an already authenticated release into an inactive application slot.
// The backend is finalized (and therefore allowed to select a boot partition)
// only after the exact byte count and SHA-256 digest match the signed manifest.
class OtaStreamUpdater {
 public:
  bool begin(const OtaVerifiedRelease& release,
             const OtaUpdateBackend& backend);
  bool write(const uint8_t* bytes, size_t size);
  bool finish();
  void abort();

  OtaUpdateState state() const { return state_; }
  OtaUpdateError error() const { return error_; }
  uint32_t bytesWritten() const { return bytesWritten_; }
  uint32_t imageSize() const { return imageSize_; }
  uint16_t progressPermille() const;

 private:
  void fail(OtaUpdateError error);

  OtaUpdateBackend backend_ = {};
  OtaSha256 hasher_;
  uint8_t expectedDigest_[kOtaSha256Size] = {};
  uint32_t imageSize_ = 0;
  uint32_t bytesWritten_ = 0;
  OtaUpdateState state_ = OtaUpdateState::kIdle;
  OtaUpdateError error_ = OtaUpdateError::kNone;
};

// ESP-IDF production adapter. esp_ota_end() validates the image without
// selecting it for boot. Selection happens only after post-final safety and
// the accepted-image transaction have been durably committed.
OtaUpdateBackend otaEsp32ApplicationUpdateBackend();
uint32_t otaInactiveApplicationPartitionSize();
uint32_t otaInactiveApplicationPartitionAddress();
bool otaReadInactiveApplicationIdentity(OtaApplicationImageIdentity* output);
bool otaReadRunningApplicationIdentity(OtaApplicationImageIdentity* output);
bool otaReadBootApplicationIdentity(OtaApplicationImageIdentity* output);
bool otaSelectAcceptedApplicationBootPartition(
    const OtaApplicationImageIdentity& expected);

const char* otaUpdateErrorName(OtaUpdateError error);
