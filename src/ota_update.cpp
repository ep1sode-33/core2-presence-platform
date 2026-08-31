#include "ota_update.h"

#include <cstring>

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_spi_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

namespace {

#if defined(ARDUINO_ARCH_ESP32)
static_assert(kOtaMaximumWriteChunkSize == SPI_FLASH_SEC_SIZE,
              "OTA download chunks must match one flash erase sector");

StaticSemaphore_t flashSensorGuardStorage = {};
SemaphoreHandle_t flashSensorGuard = nullptr;

struct Esp32OtaContext {
  esp_ota_handle_t handle = 0;
  const esp_partition_t* partition = nullptr;
  bool active = false;
};

Esp32OtaContext productionContext;

bool esp32UpdateBegin(void* rawContext, uint32_t imageSize) {
  auto* context = static_cast<Esp32OtaContext*>(rawContext);
  if (context == nullptr || context->active ||
      !otaInitializeFlashSensorGuard()) {
    return false;
  }
  context->partition = esp_ota_get_next_update_partition(nullptr);
  context->handle = 0;
  if (context->partition == nullptr || imageSize == 0 ||
      imageSize > context->partition->size) {
    context->partition = nullptr;
    return false;
  }
  // A known image size asks ESP-IDF to erase the full target range inside
  // esp_ota_begin(), which stalls the other core long enough to violate the
  // measured input/sampling safety gate. Our authenticated stream is strictly
  // sequential. The downloader additionally splits writes at sector
  // boundaries, so each bounded transaction erases at most one new sector.
  if (!otaLockFlashSensorGuard(500)) {
    context->partition = nullptr;
    return false;
  }
  const esp_err_t beginResult = esp_ota_begin(
      context->partition, OTA_WITH_SEQUENTIAL_WRITES, &context->handle);
  otaUnlockFlashSensorGuard();
  if (beginResult != ESP_OK) {
    context->partition = nullptr;
    return false;
  }
  context->active = true;
  return true;
}

size_t esp32UpdateWrite(void* rawContext, const uint8_t* bytes, size_t size) {
  auto* context = static_cast<Esp32OtaContext*>(rawContext);
  if (context == nullptr || !context->active || bytes == nullptr ||
      !otaLockFlashSensorGuard(500)) {
    return 0;
  }
  const esp_err_t writeResult = esp_ota_write(context->handle, bytes, size);
  otaUnlockFlashSensorGuard();
  return writeResult == ESP_OK ? size : 0;
}

bool esp32UpdateFinish(void* rawContext) {
  auto* context = static_cast<Esp32OtaContext*>(rawContext);
  if (context == nullptr || !context->active) {
    return false;
  }
  if (!otaLockFlashSensorGuard(500)) {
    return false;
  }
  const esp_err_t result = esp_ota_end(context->handle);
  otaUnlockFlashSensorGuard();
  context->active = false;
  context->handle = 0;
  return result == ESP_OK;
}

void esp32UpdateAbort(void* rawContext) {
  auto* context = static_cast<Esp32OtaContext*>(rawContext);
  if (context != nullptr && context->active) {
    esp_ota_abort(context->handle);
    context->active = false;
    context->handle = 0;
  }
}

bool readIdentity(const esp_partition_t* partition,
                  OtaApplicationImageIdentity* output) {
  if (partition == nullptr || output == nullptr) {
    return false;
  }
  OtaApplicationImageIdentity identity = {};
  identity.partitionAddress = partition->address;
  if (!otaLockFlashSensorGuard(500)) {
    return false;
  }
  const esp_err_t digestResult =
      esp_partition_get_sha256(partition, identity.sha256);
  otaUnlockFlashSensorGuard();
  if (digestResult != ESP_OK ||
      !otaApplicationImageIdentityIsValid(identity)) {
    return false;
  }
  *output = identity;
  return true;
}
#endif

}  // namespace

size_t otaSectorBoundedChunkSize(uint32_t bytesWritten, size_t available,
                                 size_t remaining) {
  if (available == 0 || remaining == 0) {
    return 0;
  }
  const size_t offset = bytesWritten % kOtaMaximumWriteChunkSize;
  const size_t sectorRemaining = kOtaMaximumWriteChunkSize - offset;
  size_t requested = available < remaining ? available : remaining;
  requested = requested < sectorRemaining ? requested : sectorRemaining;
  return requested < kOtaMaximumWriteChunkSize
             ? requested
             : kOtaMaximumWriteChunkSize;
}

bool otaInitializeFlashSensorGuard() {
#if defined(ARDUINO_ARCH_ESP32)
  if (flashSensorGuard == nullptr) {
    flashSensorGuard = xSemaphoreCreateMutexStatic(&flashSensorGuardStorage);
  }
  return flashSensorGuard != nullptr;
#else
  return true;
#endif
}

bool otaLockFlashSensorGuard(uint32_t timeoutMs) {
#if defined(ARDUINO_ARCH_ESP32)
  if (flashSensorGuard == nullptr) {
    return false;
  }
  TickType_t waitTicks = pdMS_TO_TICKS(timeoutMs);
  if (timeoutMs != 0 && waitTicks == 0) {
    waitTicks = 1;
  }
  return xSemaphoreTake(flashSensorGuard, waitTicks) == pdTRUE;
#else
  (void)timeoutMs;
  return true;
#endif
}

void otaUnlockFlashSensorGuard() {
#if defined(ARDUINO_ARCH_ESP32)
  if (flashSensorGuard != nullptr) {
    xSemaphoreGive(flashSensorGuard);
  }
#endif
}

bool OtaStreamUpdater::begin(const OtaVerifiedRelease& release,
                             const OtaUpdateBackend& backend) {
  if (state_ == OtaUpdateState::kWriting) {
    return false;
  }
  backend_ = {};
  imageSize_ = 0;
  bytesWritten_ = 0;
  std::memset(expectedDigest_, 0, sizeof(expectedDigest_));
  hasher_.reset();
  state_ = OtaUpdateState::kIdle;
  error_ = OtaUpdateError::kNone;
  if (!release.valid()) {
    fail(OtaUpdateError::kInvalidVerifiedRelease);
    return false;
  }
  if (backend.begin == nullptr || backend.write == nullptr ||
      backend.finish == nullptr || backend.abort == nullptr) {
    fail(OtaUpdateError::kInvalidBackend);
    return false;
  }
  backend_ = backend;
  imageSize_ = release.manifest().firmwareSize;
  std::memcpy(expectedDigest_, release.manifest().firmwareSha256,
              sizeof(expectedDigest_));
  if (!backend_.begin(backend_.context, imageSize_)) {
    fail(OtaUpdateError::kBackendBeginFailed);
    return false;
  }
  state_ = OtaUpdateState::kWriting;
  return true;
}

bool OtaStreamUpdater::write(const uint8_t* bytes, size_t size) {
  if (state_ != OtaUpdateState::kWriting) {
    error_ = OtaUpdateError::kNotWriting;
    return false;
  }
  if (bytes == nullptr && size != 0) {
    fail(OtaUpdateError::kNullChunk);
    return false;
  }
  if (size > kOtaMaximumWriteChunkSize) {
    fail(OtaUpdateError::kChunkTooLarge);
    return false;
  }
  if (size > imageSize_ - bytesWritten_) {
    fail(OtaUpdateError::kImageLengthOverflow);
    return false;
  }
  if (size == 0) {
    return true;
  }
  if (!hasher_.update(bytes, size)) {
    fail(OtaUpdateError::kHashFailure);
    return false;
  }
  const size_t written = backend_.write(backend_.context, bytes, size);
  if (written != size) {
    fail(OtaUpdateError::kBackendShortWrite);
    return false;
  }
  bytesWritten_ += static_cast<uint32_t>(written);
  return true;
}

bool OtaStreamUpdater::finish() {
  if (state_ != OtaUpdateState::kWriting) {
    error_ = OtaUpdateError::kNotWriting;
    return false;
  }
  if (bytesWritten_ != imageSize_) {
    fail(OtaUpdateError::kImageLengthMismatch);
    return false;
  }
  uint8_t actualDigest[kOtaSha256Size] = {};
  if (!hasher_.finish(actualDigest)) {
    fail(OtaUpdateError::kHashFailure);
    return false;
  }
  if (!otaConstantTimeEqual(actualDigest, expectedDigest_,
                            sizeof(actualDigest))) {
    fail(OtaUpdateError::kImageDigestMismatch);
    return false;
  }
  if (!backend_.finish(backend_.context)) {
    fail(OtaUpdateError::kBackendFinishFailed);
    return false;
  }
  state_ = OtaUpdateState::kReadyToReboot;
  error_ = OtaUpdateError::kNone;
  return true;
}

void OtaStreamUpdater::abort() {
  if (state_ == OtaUpdateState::kWriting && backend_.abort != nullptr) {
    backend_.abort(backend_.context);
  }
  if (state_ != OtaUpdateState::kReadyToReboot) {
    state_ = OtaUpdateState::kAborted;
    error_ = OtaUpdateError::kOperatorAborted;
  }
}

uint16_t OtaStreamUpdater::progressPermille() const {
  if (imageSize_ == 0) {
    return 0;
  }
  const uint64_t scaled = static_cast<uint64_t>(bytesWritten_) * 1000U;
  return static_cast<uint16_t>(scaled / imageSize_);
}

void OtaStreamUpdater::fail(OtaUpdateError error) {
  if (state_ == OtaUpdateState::kWriting && backend_.abort != nullptr) {
    backend_.abort(backend_.context);
  }
  state_ = OtaUpdateState::kFailed;
  error_ = error;
}

bool otaApplicationImageIdentityIsValid(
    const OtaApplicationImageIdentity& identity) {
  if (identity.partitionAddress == 0 ||
      (identity.partitionAddress & UINT32_C(0xffff)) != 0) {
    return false;
  }
  uint8_t combined = 0;
  for (uint8_t byte : identity.sha256) {
    combined |= byte;
  }
  return combined != 0;
}

bool otaApplicationImageIdentityEquals(
    const OtaApplicationImageIdentity& left,
    const OtaApplicationImageIdentity& right) {
  return otaApplicationImageIdentityIsValid(left) &&
         otaApplicationImageIdentityIsValid(right) &&
         left.partitionAddress == right.partitionAddress &&
         std::memcmp(left.sha256, right.sha256, sizeof(left.sha256)) == 0;
}

OtaUpdateBackend otaEsp32ApplicationUpdateBackend() {
#if defined(ARDUINO_ARCH_ESP32)
  return {&productionContext, esp32UpdateBegin, esp32UpdateWrite,
          esp32UpdateFinish, esp32UpdateAbort};
#else
  return {};
#endif
}

uint32_t otaInactiveApplicationPartitionSize() {
#if defined(ARDUINO_ARCH_ESP32)
  const esp_partition_t* partition = esp_ota_get_next_update_partition(nullptr);
  return partition == nullptr ? 0 : static_cast<uint32_t>(partition->size);
#else
  return 0;
#endif
}

uint32_t otaInactiveApplicationPartitionAddress() {
#if defined(ARDUINO_ARCH_ESP32)
  const esp_partition_t* partition = esp_ota_get_next_update_partition(nullptr);
  return partition == nullptr ? 0 : partition->address;
#else
  return 0;
#endif
}

bool otaReadInactiveApplicationIdentity(OtaApplicationImageIdentity* output) {
#if defined(ARDUINO_ARCH_ESP32)
  return readIdentity(esp_ota_get_next_update_partition(nullptr), output);
#else
  (void)output;
  return false;
#endif
}

bool otaReadRunningApplicationIdentity(OtaApplicationImageIdentity* output) {
#if defined(ARDUINO_ARCH_ESP32)
  return readIdentity(esp_ota_get_running_partition(), output);
#else
  (void)output;
  return false;
#endif
}

bool otaReadBootApplicationIdentity(OtaApplicationImageIdentity* output) {
#if defined(ARDUINO_ARCH_ESP32)
  return readIdentity(esp_ota_get_boot_partition(), output);
#else
  (void)output;
  return false;
#endif
}

bool otaSelectAcceptedApplicationBootPartition(
    const OtaApplicationImageIdentity& expected) {
#if defined(ARDUINO_ARCH_ESP32)
  const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
  if (!otaApplicationImageIdentityIsValid(expected) || target == nullptr ||
      target->address != expected.partitionAddress) {
    return false;
  }
  // The caller has just hashed this exact inactive partition and durably
  // committed that identity. No writer can mutate it between that check and
  // this final handoff, so avoid another full-partition flash read after the
  // terminal safety gate.
  if (!otaLockFlashSensorGuard(500)) {
    return false;
  }
  const esp_err_t selectResult = esp_ota_set_boot_partition(target);
  otaUnlockFlashSensorGuard();
  const esp_partition_t* selected = esp_ota_get_boot_partition();
  return selectResult == ESP_OK && selected != nullptr &&
         selected->address == expected.partitionAddress;
#else
  (void)expected;
  return false;
#endif
}

const char* otaUpdateErrorName(OtaUpdateError error) {
  switch (error) {
    case OtaUpdateError::kNone:
      return "none";
    case OtaUpdateError::kInvalidVerifiedRelease:
      return "invalid_verified_release";
    case OtaUpdateError::kInvalidBackend:
      return "invalid_backend";
    case OtaUpdateError::kBackendBeginFailed:
      return "backend_begin_failed";
    case OtaUpdateError::kNotWriting:
      return "not_writing";
    case OtaUpdateError::kNullChunk:
      return "null_chunk";
    case OtaUpdateError::kChunkTooLarge:
      return "chunk_too_large";
    case OtaUpdateError::kImageLengthOverflow:
      return "image_length_overflow";
    case OtaUpdateError::kBackendShortWrite:
      return "backend_short_write";
    case OtaUpdateError::kHashFailure:
      return "hash_failure";
    case OtaUpdateError::kImageLengthMismatch:
      return "image_length_mismatch";
    case OtaUpdateError::kImageDigestMismatch:
      return "image_digest_mismatch";
    case OtaUpdateError::kBackendFinishFailed:
      return "backend_finish_failed";
    case OtaUpdateError::kOperatorAborted:
      return "operator_aborted";
  }
  return "unknown";
}
