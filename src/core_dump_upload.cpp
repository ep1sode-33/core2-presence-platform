#include "core_dump_upload.h"

#include <cstdio>
#include <cstring>
#include <limits>

#include "ota_crypto.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_core_dump.h>
#include <esp_err.h>
#include <esp_partition.h>
#endif

namespace {

constexpr char kCrashIdDomain[] = "m5go-coredump-crash-id-v1";
constexpr char kHexDigits[] = "0123456789abcdef";

void secureZero(void* bytes, size_t size) {
  volatile uint8_t* cursor = static_cast<volatile uint8_t*>(bytes);
  while (size-- != 0) {
    *cursor++ = 0;
  }
}

bool validIdentifier(const char* value, size_t minimum, size_t maximum,
                     bool allowDot, bool allowColon, bool allowPlus) {
  if (value == nullptr) {
    return false;
  }
  size_t length = 0;
  while (length <= maximum && value[length] != '\0') {
    ++length;
  }
  if (length < minimum || length > maximum) {
    return false;
  }
  for (size_t index = 0; index < length; ++index) {
    const char byte = value[index];
    if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
        (byte >= '0' && byte <= '9') || byte == '_' || byte == '-' ||
        (allowDot && byte == '.') || (allowColon && byte == ':') ||
        (allowPlus && byte == '+')) {
      continue;
    }
    return false;
  }
  return true;
}

bool validContext(const CoreDumpReportContext& context) {
  // Device IDs are not part of the JSON body, but the deployed endpoint uses
  // core2-<12 lowercase hex>. A slightly broader safe identifier validator
  // keeps this layer reusable without permitting path/JSON metacharacters.
  return validIdentifier(context.deviceId, 1, 64, true, true, false) &&
         validIdentifier(context.bootId, 16, 64, false, false, false) &&
         validIdentifier(context.buildId, 1, 128, true, true, true) &&
         validIdentifier(context.resetReason, 1, 64, true, false, false);
}

void encodeHex(const uint8_t* bytes, size_t size, char* destination) {
  for (size_t index = 0; index < size; ++index) {
    destination[index * 2] = kHexDigits[bytes[index] >> 4U];
    destination[index * 2 + 1] = kHexDigits[bytes[index] & 0x0fU];
  }
  destination[size * 2] = '\0';
}

bool hashLengthPrefixed(OtaSha256& hasher, const char* value) {
  const size_t length = std::strlen(value);
  if (length > UINT16_MAX) {
    return false;
  }
  const uint8_t encodedLength[2] = {
      static_cast<uint8_t>(length >> 8U), static_cast<uint8_t>(length)};
  return hasher.update(encodedLength, sizeof(encodedLength)) &&
         hasher.update(reinterpret_cast<const uint8_t*>(value), length);
}

bool deriveCrashId(const CoreDumpReportContext& context,
                   const uint8_t dumpDigest[kCoreDumpSha256Size],
                   char crashId[CoreDumpReportMetadata::kCrashIdCapacity]) {
  OtaSha256 hasher;
  if (!hasher.update(reinterpret_cast<const uint8_t*>(kCrashIdDomain),
                     sizeof(kCrashIdDomain) - 1) ||
      !hashLengthPrefixed(hasher, context.deviceId) ||
      !hashLengthPrefixed(hasher, context.buildId) ||
      !hasher.update(dumpDigest, kCoreDumpSha256Size)) {
    return false;
  }
  uint8_t digest[kCoreDumpSha256Size] = {};
  if (!hasher.finish(digest)) {
    return false;
  }
  encodeHex(digest, sizeof(digest), crashId);
  secureZero(digest, sizeof(digest));
  return true;
}

#if defined(ARDUINO_ARCH_ESP32)
const esp_partition_t* coreDumpPartition() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                  ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
}

bool descriptorInsidePartition(const CoreDumpImageDescriptor& descriptor,
                               const esp_partition_t& partition) {
  if (descriptor.size == 0 || descriptor.size > kCoreDumpMaximumSize ||
      descriptor.storageAddress < partition.address) {
    return false;
  }
  const size_t relative = descriptor.storageAddress - partition.address;
  return relative <= partition.size && descriptor.size <= partition.size - relative;
}

CoreDumpProbeStatus esp32Probe(void*, CoreDumpImageDescriptor* descriptor) {
  if (descriptor == nullptr) {
    return CoreDumpProbeStatus::kIoError;
  }
  *descriptor = {};
  const esp_err_t validation = esp_core_dump_image_check();
  if (validation == ESP_ERR_NOT_FOUND) {
    return CoreDumpProbeStatus::kNotFound;
  }
  if (validation == ESP_ERR_INVALID_SIZE || validation == ESP_ERR_INVALID_CRC) {
    return CoreDumpProbeStatus::kCorrupt;
  }
  if (validation != ESP_OK) {
    return CoreDumpProbeStatus::kIoError;
  }

  size_t address = 0;
  size_t size = 0;
  const esp_err_t result = esp_core_dump_image_get(&address, &size);
  if (result == ESP_ERR_NOT_FOUND) {
    return CoreDumpProbeStatus::kNotFound;
  }
  if (result != ESP_OK) {
    return CoreDumpProbeStatus::kIoError;
  }
  descriptor->storageAddress = address;
  descriptor->size = size;
  const esp_partition_t* partition = coreDumpPartition();
  if (partition == nullptr || !descriptorInsidePartition(*descriptor, *partition)) {
    *descriptor = {};
    return CoreDumpProbeStatus::kCorrupt;
  }
  return CoreDumpProbeStatus::kPresent;
}

bool esp32Read(void*, const CoreDumpImageDescriptor* descriptor, size_t offset,
               uint8_t* destination, size_t size) {
  if (descriptor == nullptr || (destination == nullptr && size != 0) ||
      size > kCoreDumpMaximumReadChunk || offset > descriptor->size ||
      size > descriptor->size - offset) {
    return false;
  }
  if (size == 0) {
    return true;
  }
  const esp_partition_t* partition = coreDumpPartition();
  if (partition == nullptr || !descriptorInsidePartition(*descriptor, *partition)) {
    return false;
  }
  const size_t relative = descriptor->storageAddress - partition->address;
  return esp_partition_read(partition, relative + offset, destination, size) ==
         ESP_OK;
}

bool esp32Erase(void*, const CoreDumpImageDescriptor* descriptor) {
  const esp_partition_t* partition = coreDumpPartition();
  if (descriptor == nullptr || partition == nullptr ||
      !descriptorInsidePartition(*descriptor, *partition)) {
    return false;
  }
  // ESP-IDF owns the coredump layout and erase semantics; do not erase the
  // partition directly.
  return esp_core_dump_image_erase() == ESP_OK;
}
#endif

}  // namespace

CoreDumpPrepareResult PendingCoreDump::prepare(
    const CoreDumpStorageBackend& backend,
    const CoreDumpReportContext& context) {
  clear();
  if (!validContext(context)) {
    return CoreDumpPrepareResult::kInvalidContext;
  }
  if (backend.probe == nullptr || backend.read == nullptr ||
      backend.erase == nullptr) {
    return CoreDumpPrepareResult::kInvalidBackend;
  }

  CoreDumpImageDescriptor descriptor;
  const CoreDumpProbeStatus status = backend.probe(backend.context, &descriptor);
  switch (status) {
    case CoreDumpProbeStatus::kNotFound:
      return CoreDumpPrepareResult::kNoDump;
    case CoreDumpProbeStatus::kCorrupt:
      return CoreDumpPrepareResult::kCorruptDump;
    case CoreDumpProbeStatus::kIoError:
      return CoreDumpPrepareResult::kIoError;
    case CoreDumpProbeStatus::kPresent:
      break;
  }
  if (descriptor.size == 0) {
    return CoreDumpPrepareResult::kCorruptDump;
  }
  if (descriptor.size > kCoreDumpMaximumSize ||
      descriptor.size > std::numeric_limits<uint32_t>::max()) {
    return CoreDumpPrepareResult::kDumpTooLarge;
  }

  OtaSha256 hasher;
  uint8_t buffer[kCoreDumpMaximumReadChunk] = {};
  size_t offset = 0;
  while (offset < descriptor.size) {
    const size_t remaining = descriptor.size - offset;
    const size_t chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    if (!backend.read(backend.context, &descriptor, offset, buffer, chunk)) {
      secureZero(buffer, sizeof(buffer));
      return CoreDumpPrepareResult::kReadFailed;
    }
    if (!hasher.update(buffer, chunk)) {
      secureZero(buffer, sizeof(buffer));
      return CoreDumpPrepareResult::kHashFailed;
    }
    offset += chunk;
  }
  secureZero(buffer, sizeof(buffer));

  CoreDumpReportMetadata metadata;
  if (!hasher.finish(metadata.dumpSha256) ||
      !deriveCrashId(context, metadata.dumpSha256, metadata.crashId)) {
    return CoreDumpPrepareResult::kHashFailed;
  }
  encodeHex(metadata.dumpSha256, sizeof(metadata.dumpSha256),
            metadata.dumpSha256Hex);
  std::snprintf(metadata.bootId, sizeof(metadata.bootId), "%s", context.bootId);
  std::snprintf(metadata.buildId, sizeof(metadata.buildId), "%s", context.buildId);
  std::snprintf(metadata.resetReason, sizeof(metadata.resetReason), "%s",
                context.resetReason);
  metadata.dumpSize = static_cast<uint32_t>(descriptor.size);

  backend_ = backend;
  descriptor_ = descriptor;
  metadata_ = metadata;
  ready_ = true;
  return CoreDumpPrepareResult::kReady;
}

bool PendingCoreDump::readChunk(size_t offset, uint8_t* destination,
                                size_t size) const {
  if (!ready_ || backend_.read == nullptr ||
      (destination == nullptr && size != 0) ||
      size > kCoreDumpMaximumReadChunk || offset > descriptor_.size ||
      size > descriptor_.size - offset) {
    return false;
  }
  if (size == 0) {
    return true;
  }
  return backend_.read(backend_.context, &descriptor_, offset, destination,
                       size);
}

CoreDumpAcknowledgeResult PendingCoreDump::acknowledgeDurable(
    const char* responseCrashId, bool durable) {
  if (!ready_) {
    return CoreDumpAcknowledgeResult::kNotPrepared;
  }
  if (!durable) {
    return CoreDumpAcknowledgeResult::kNotDurable;
  }
  if (responseCrashId == nullptr ||
      std::strcmp(responseCrashId, metadata_.crashId) != 0) {
    return CoreDumpAcknowledgeResult::kCrashIdMismatch;
  }
  if (backend_.erase == nullptr ||
      !backend_.erase(backend_.context, &descriptor_)) {
    return CoreDumpAcknowledgeResult::kEraseFailed;
  }
  clear();
  return CoreDumpAcknowledgeResult::kErased;
}

void PendingCoreDump::clear() {
  backend_ = {};
  descriptor_ = {};
  metadata_ = {};
  ready_ = false;
}

CoreDumpStorageBackend coreDumpEsp32StorageBackend() {
#if defined(ARDUINO_ARCH_ESP32)
  return {nullptr, esp32Probe, esp32Read, esp32Erase};
#else
  return {};
#endif
}

const char* coreDumpPrepareResultName(CoreDumpPrepareResult result) {
  switch (result) {
    case CoreDumpPrepareResult::kReady:
      return "ready";
    case CoreDumpPrepareResult::kNoDump:
      return "no_dump";
    case CoreDumpPrepareResult::kInvalidContext:
      return "invalid_context";
    case CoreDumpPrepareResult::kInvalidBackend:
      return "invalid_backend";
    case CoreDumpPrepareResult::kCorruptDump:
      return "corrupt_dump";
    case CoreDumpPrepareResult::kDumpTooLarge:
      return "dump_too_large";
    case CoreDumpPrepareResult::kReadFailed:
      return "read_failed";
    case CoreDumpPrepareResult::kHashFailed:
      return "hash_failed";
    case CoreDumpPrepareResult::kIoError:
      return "io_error";
  }
  return "unknown";
}

const char* coreDumpAcknowledgeResultName(CoreDumpAcknowledgeResult result) {
  switch (result) {
    case CoreDumpAcknowledgeResult::kErased:
      return "erased";
    case CoreDumpAcknowledgeResult::kNotPrepared:
      return "not_prepared";
    case CoreDumpAcknowledgeResult::kNotDurable:
      return "not_durable";
    case CoreDumpAcknowledgeResult::kCrashIdMismatch:
      return "crash_id_mismatch";
    case CoreDumpAcknowledgeResult::kEraseFailed:
      return "erase_failed";
  }
  return "unknown";
}
