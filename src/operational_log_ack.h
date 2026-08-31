#pragma once

#include <cstddef>
#include <cstdint>

enum class OperationalLogAckError : uint8_t {
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

struct OperationalLogAck {
  uint64_t stored = 0;
  uint64_t duplicates = 0;
  uint64_t serverUtcMs = 0;
  uint64_t retainedRecords = 0;
};

struct OperationalLogAckResult {
  OperationalLogAck ack = {};
  OperationalLogAckError error = OperationalLogAckError::kNone;

  bool ok() const { return error == OperationalLogAckError::kNone; }
};

OperationalLogAckResult parseOperationalLogAck(
    const char* json, size_t jsonLength, const char* expectedBatchId,
    size_t expectedRecordCount);

const char* operationalLogAckErrorName(OperationalLogAckError error);
