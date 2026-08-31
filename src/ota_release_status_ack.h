#pragma once

#include <cstddef>
#include <cstdint>

enum class OtaReleaseStatusAckError : uint8_t {
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

struct OtaReleaseStatusAck {
  uint64_t serverUtcMs = 0;
  bool duplicate = false;
  bool desiredReleaseCompleted = false;
};

struct OtaReleaseStatusAckResult {
  OtaReleaseStatusAck ack = {};
  OtaReleaseStatusAckError error = OtaReleaseStatusAckError::kNone;

  bool ok() const { return error == OtaReleaseStatusAckError::kNone; }
};

// Strictly consumes the complete release-status response. A matching ID is
// required before the worker treats a status report as durably acknowledged.
OtaReleaseStatusAckResult parseOtaReleaseStatusAck(
    const char* json, size_t jsonLength, const char* expectedStatusId);

const char* otaReleaseStatusAckErrorName(OtaReleaseStatusAckError error);
