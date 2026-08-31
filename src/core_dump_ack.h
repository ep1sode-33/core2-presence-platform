#pragma once

#include <cstddef>
#include <cstdint>

enum class CoreDumpAckError : uint8_t {
  kNone = 0,
  kNullArgument,
  kMalformedJson,
  kMissingField,
  kDuplicateField,
  kUnknownField,
  kWrongType,
  kInvalidValue,
  kMismatch,
  kTrailingData,
};

struct CoreDumpAckResult {
  uint64_t serverUtcMs = 0;
  bool duplicate = false;
  bool durable = false;
  CoreDumpAckError error = CoreDumpAckError::kNone;

  bool ok() const { return error == CoreDumpAckError::kNone; }
};

CoreDumpAckResult parseCoreDumpAck(const char* json, size_t jsonLength,
                                   const char* expectedCrashId);

const char* coreDumpAckErrorName(CoreDumpAckError error);
