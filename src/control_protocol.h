#pragma once

#include <cstddef>
#include <cstdint>

#include "command_journal.h"

struct DesiredFirmwareRelease {
  static constexpr size_t kReleaseIdCapacity = 49;
  static constexpr size_t kHardwareCapacity = 49;
  static constexpr size_t kVersionCapacity = 33;
  static constexpr size_t kBuildIdCapacity = 65;
  static constexpr size_t kDigestHexCapacity = 65;
  static constexpr size_t kKeyIdCapacity = 33;
  static constexpr size_t kRelativeUrlCapacity = 193;

  char releaseId[kReleaseIdCapacity] = {};
  char hardwareModel[kHardwareCapacity] = {};
  char firmwareVersion[kVersionCapacity] = {};
  char buildId[kBuildIdCapacity] = {};
  char imageSha256[kDigestHexCapacity] = {};
  char elfSha256[kDigestHexCapacity] = {};
  char signingKeyId[kKeyIdCapacity] = {};
  char manifestUrl[kRelativeUrlCapacity] = {};
  char imageUrl[kRelativeUrlCapacity] = {};
  uint64_t releaseCounter = 0;
  uint32_t imageSize = 0;
};

struct ControlPoll {
  uint64_t serverUtcMs = 0;
  uint32_t pollAfterMs = 0;
  bool hasDesiredRelease = false;
  DesiredFirmwareRelease desiredRelease = {};
  bool hasCommand = false;
  RemoteCommandEnvelope command = {};
};

enum class ControlPollParseError : uint8_t {
  kNone,
  kNullArgument,
  kTopLevelNotObject,
  kMalformedJson,
  kMissingField,
  kDuplicateField,
  kUnknownField,
  kWrongType,
  kStringTooLong,
  kStringEscapeNotAllowed,
  kIntegerOverflow,
  kIntegerOutOfRange,
  kInvalidValue,
  kTrailingData,
};

struct ControlPollParseResult {
  ControlPoll value = {};
  ControlPollParseError error = ControlPollParseError::kNone;

  bool ok() const { return error == ControlPollParseError::kNone; }
  explicit operator bool() const { return ok(); }
};

// Strictly parses the complete backend ControlPollResponse. Release URLs must
// be same-origin relative paths; an authenticated response can therefore never
// redirect the device bearer token to another host.
ControlPollParseResult parseControlPollResponse(const char* json,
                                                size_t jsonLength);

const char* controlPollParseErrorName(ControlPollParseError error);
