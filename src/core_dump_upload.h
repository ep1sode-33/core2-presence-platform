#pragma once

#include <cstddef>
#include <cstdint>

// The shipped 16 MiB partition layout reserves exactly 64 KiB for an ESP-IDF
// flash core dump. Never accept a larger descriptor even if a damaged
// partition table or backend reports one.
constexpr size_t kCoreDumpMaximumSize = 65'536;
constexpr size_t kCoreDumpMaximumReadChunk = 1'024;
constexpr size_t kCoreDumpSha256Size = 32;

enum class CoreDumpProbeStatus : uint8_t {
  kPresent = 0,
  kNotFound,
  kCorrupt,
  kIoError,
};

struct CoreDumpImageDescriptor {
  // An opaque storage address supplied back to the same backend. On ESP32 it
  // is the absolute flash address returned by esp_core_dump_image_get().
  size_t storageAddress = 0;
  size_t size = 0;
};

// A narrow adapter keeps the safety and streaming policy host-testable while
// the ESP32 implementation delegates integrity validation to ESP-IDF.
struct CoreDumpStorageBackend {
  void* context = nullptr;
  CoreDumpProbeStatus (*probe)(void* context,
                               CoreDumpImageDescriptor* descriptor) = nullptr;
  bool (*read)(void* context, const CoreDumpImageDescriptor* descriptor,
               size_t offset, uint8_t* destination, size_t size) = nullptr;
  bool (*erase)(void* context,
                const CoreDumpImageDescriptor* descriptor) = nullptr;
};

struct CoreDumpReportContext {
  const char* deviceId = nullptr;
  const char* bootId = nullptr;
  const char* buildId = nullptr;
  const char* resetReason = nullptr;
};

struct CoreDumpReportMetadata {
  static constexpr size_t kCrashIdCapacity = 65;
  static constexpr size_t kBootIdCapacity = 65;
  static constexpr size_t kBuildIdCapacity = 129;
  static constexpr size_t kResetReasonCapacity = 65;
  static constexpr size_t kDigestHexCapacity = 65;

  char crashId[kCrashIdCapacity] = {};
  char bootId[kBootIdCapacity] = {};
  char buildId[kBuildIdCapacity] = {};
  char resetReason[kResetReasonCapacity] = {};
  uint32_t dumpSize = 0;
  uint8_t dumpSha256[kCoreDumpSha256Size] = {};
  char dumpSha256Hex[kDigestHexCapacity] = {};
};

enum class CoreDumpPrepareResult : uint8_t {
  kReady = 0,
  kNoDump,
  kInvalidContext,
  kInvalidBackend,
  kCorruptDump,
  kDumpTooLarge,
  kReadFailed,
  kHashFailed,
  kIoError,
};

enum class CoreDumpAcknowledgeResult : uint8_t {
  kErased = 0,
  kNotPrepared,
  kNotDurable,
  kCrashIdMismatch,
  kEraseFailed,
};

// Owns only bounded metadata and the backend descriptor. The dump itself is
// always read directly from flash in small chunks.
class PendingCoreDump {
 public:
  PendingCoreDump() = default;
  PendingCoreDump(const PendingCoreDump&) = delete;
  PendingCoreDump& operator=(const PendingCoreDump&) = delete;

  CoreDumpPrepareResult prepare(const CoreDumpStorageBackend& backend,
                                const CoreDumpReportContext& context);

  bool ready() const { return ready_; }
  const CoreDumpReportMetadata& metadata() const { return metadata_; }

  // Reads at most kCoreDumpMaximumReadChunk bytes. A zero-byte read is valid;
  // every non-zero range must be fully inside the prepared dump.
  bool readChunk(size_t offset, uint8_t* destination, size_t size) const;

  // This is the only destructive API. Call it only after parsing a durable
  // backend response for this exact crash_id. All preparation, hashing, and
  // JSON streaming paths are deliberately non-destructive.
  CoreDumpAcknowledgeResult acknowledgeDurable(const char* responseCrashId,
                                               bool durable);

  // Drops local metadata without acknowledging or erasing flash.
  void clear();

 private:
  CoreDumpStorageBackend backend_ = {};
  CoreDumpImageDescriptor descriptor_ = {};
  CoreDumpReportMetadata metadata_ = {};
  bool ready_ = false;
};

// ESP32 Arduino adapter. Its probe implementation calls
// esp_core_dump_image_check() and esp_core_dump_image_get(); read/erase stay
// bounded to the discovered coredump partition. Host builds return an empty
// backend so unit tests can inject memory-backed storage.
CoreDumpStorageBackend coreDumpEsp32StorageBackend();

const char* coreDumpPrepareResultName(CoreDumpPrepareResult result);
const char* coreDumpAcknowledgeResultName(CoreDumpAcknowledgeResult result);
