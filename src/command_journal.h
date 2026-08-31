#pragma once

#include <cstddef>
#include <cstdint>

enum class RemoteCommandAction : uint8_t {
  kDiagnosticSnapshot,
  kSetLogLevel,
  kRecalibrateMicrophone,
  kRetryUpload,
  kReboot,
  kOpenDevOta,
};

enum class CommandExecutionStatus : uint8_t {
  kAccepted,
  kRunning,
  kSucceeded,
  kFailed,
  kExpired,
  kRejected,
};

struct RemoteCommandEnvelope {
  static constexpr size_t kIdCapacity = 65;

  char commandId[kIdCapacity] = {};
  char leaseId[kIdCapacity] = {};
  uint64_t createdAtMs = 0;
  uint64_t expiresAtMs = 0;
  uint64_t leaseExpiresAtMs = 0;
  RemoteCommandAction action = RemoteCommandAction::kDiagnosticSnapshot;
  uint16_t durationSeconds = 0;
  bool detailedLog = false;
  bool requiresLocalConfirmation = false;
};

bool remoteCommandEnvelopeIsValid(const RemoteCommandEnvelope& command);
bool commandExecutionStatusIsTerminal(CommandExecutionStatus status);

struct CommandJournalRecord {
  static constexpr size_t kIdCapacity = RemoteCommandEnvelope::kIdCapacity;

  char commandId[kIdCapacity] = {};
  char leaseId[kIdCapacity] = {};
  uint64_t acceptedAtMs = 0;
  uint64_t expiresAtMs = 0;
  RemoteCommandAction action = RemoteCommandAction::kDiagnosticSnapshot;
  CommandExecutionStatus status = CommandExecutionStatus::kAccepted;
};

enum class CommandAcceptance : uint8_t {
  kAcceptedNew,
  kReplayExisting,
  kExpired,
  kLeaseExpired,
  kBusy,
  kInvalid,
};

// Pure state machine. The caller must persist record() after kAcceptedNew,
// kReplayExisting (the backend may have issued a fresh lease), and every
// successful status transition before performing the associated side effect or
// sending its acknowledgement.
class CommandJournal {
 public:
  bool restore(const CommandJournalRecord& record);
  CommandAcceptance consider(const RemoteCommandEnvelope& command,
                              uint64_t serverUtcMs);
  bool transition(CommandExecutionStatus next);
  bool hasRecord() const { return hasRecord_; }
  const CommandJournalRecord& record() const { return record_; }

 private:
  bool hasRecord_ = false;
  CommandJournalRecord record_ = {};
};

constexpr size_t kCommandJournalBlobSize = 162;

enum class CommandJournalBlobError : uint8_t {
  kNone,
  kNullArgument,
  kWrongLength,
  kBadMagic,
  kUnsupportedVersion,
  kBadPayloadLength,
  kChecksumMismatch,
  kInvalidRecord,
};

bool commandJournalRecordIsValid(const CommandJournalRecord& record);

enum class CommandJournalRecoveryAction : uint8_t {
  kInvalid = 0,
  kFailInterruptedThenAck,
  kAckPersistedTerminal,
};

// Recovery is driven by the durable record, not by whether the backend still
// leases the command. In particular, a terminal record persisted immediately
// before power loss must have its deterministic final ACK replayed.
CommandJournalRecoveryAction commandJournalRecoveryAction(
    const CommandJournalRecord& record);
bool buildCommandAckId(const CommandJournalRecord& record, char* output,
                       size_t outputCapacity);
CommandJournalBlobError encodeCommandJournalRecord(
    const CommandJournalRecord& record, uint8_t* output,
    size_t outputCapacity);
CommandJournalBlobError decodeCommandJournalRecord(
    const uint8_t* input, size_t inputLength, CommandJournalRecord* output);

enum class CommandJournalStorageResult : uint8_t {
  kOk,
  kNotStored,
  kInvalidRecord,
  kInvalidStoredData,
  kOpenFailed,
  kReadFailed,
  kWriteFailed,
  kVerifyFailed,
  kUnsupportedPlatform,
};

CommandJournalStorageResult loadCommandJournalRecord(
    CommandJournalRecord* output);
CommandJournalStorageResult saveCommandJournalRecord(
    const CommandJournalRecord& record);
