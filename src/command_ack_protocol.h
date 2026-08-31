#pragma once

#include <cstddef>
#include <cstdint>

#include "command_journal.h"

using CommandAckJsonWrite = bool (*)(void* context, const char* data,
                                     size_t size);

struct CommandAckJsonSink {
  void* context = nullptr;
  CommandAckJsonWrite write = nullptr;
};

const char* commandExecutionStatusWireName(CommandExecutionStatus status);

bool writeCommandAckJson(const CommandJournalRecord& record,
                         const CommandAckJsonSink& sink);

enum class CommandAckResponseError : uint8_t {
  kNone,
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

struct CommandAckResponseResult {
  uint64_t serverUtcMs = 0;
  bool duplicate = false;
  CommandAckResponseError error = CommandAckResponseError::kNone;

  bool ok() const { return error == CommandAckResponseError::kNone; }
  explicit operator bool() const { return ok(); }
};

CommandAckResponseResult parseCommandAckResponse(
    const char* json, size_t jsonLength, const CommandJournalRecord& expected);

const char* commandAckResponseErrorName(CommandAckResponseError error);
