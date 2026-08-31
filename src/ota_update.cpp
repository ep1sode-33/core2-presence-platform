#include "ota_update.h"

#include <cstring>

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_ota_ops.h>
#include <esp_partition.h>
#endif

namespace {

#if defined(ARDUINO_ARCH_ESP32)
struct Esp32OtaContext {
  esp_ota_handle_t handle = 0;
  const esp_partition_t* partition = nullptr;
  bool active = false;
};

Esp32OtaContext productionContext;

bool esp32UpdateBegin(void* rawContext, uint32_t imageSize) {
  auto* context = static_cast<Esp32OtaContext*>(rawContext);
  if (context == nullptr || context->active) {
    return false;
  }
  context->partition = esp_ota_get_next_update_partition(nullptr);
  context->handle = 0;
  if (context->partition == nullptr ||
      esp_ota_begin(context->partition, imageSize, &context->handle) != ESP_OK) {
    context->partition = nullptr;
    return false;
  }
  context->active = true;
  return true;
}

size_t esp32UpdateWrite(void* rawContext, const uint8_t* bytes, size_t size) {
  auto* context = static_cast<Esp32OtaContext*>(rawContext);
  if (context == nullptr || !context->active || bytes == nullptr ||
      esp_ota_write(context->handle, bytes, size) != ESP_OK) {
    return 0;
  }
  return size;
}

bool esp32UpdateFinish(void* rawContext) {
  auto* context = static_cast<Esp32OtaContext*>(rawContext);
  if (context == nullptr || !context->active) {
    return false;
  }
  const esp_err_t result = esp_ota_end(context->handle);
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
  if (esp_partition_get_sha256(partition, identity.sha256) != ESP_OK ||
      !otaApplicationImageIdentityIsValid(identity)) {
    return false;
  }
  *output = identity;
  return true;
}
#endif

}  // namespace

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
  OtaApplicationImageIdentity actual = {};
  if (!otaApplicationImageIdentityIsValid(expected) || target == nullptr ||
      !readIdentity(target, &actual) ||
      !otaApplicationImageIdentityEquals(expected, actual)) {
    return false;
  }
  const esp_err_t selectResult = esp_ota_set_boot_partition(target);
  OtaApplicationImageIdentity selected = {};
  const bool selectedExactly = otaReadBootApplicationIdentity(&selected) &&
                               otaApplicationImageIdentityEquals(
                                   expected, selected);
  return selectResult == ESP_OK && selectedExactly;
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
