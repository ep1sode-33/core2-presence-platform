#include "command_journal.h"

#include <cstdio>
#include <cstring>
#include <limits>

namespace {

constexpr uint64_t kMaxSigned64 = 0x7fffffffffffffffULL;
constexpr uint8_t kMagic[] = {'M', '5', 'C', 'J'};
constexpr uint16_t kFormatVersion = 1;
constexpr uint16_t kPayloadLength = 150;
constexpr size_t kIdLengthOffset = 8;
constexpr size_t kIdOffset = 9;
constexpr size_t kLeaseIdLengthOffset = 73;
constexpr size_t kLeaseIdOffset = 74;
constexpr size_t kActionOffset = 138;
constexpr size_t kStatusOffset = 139;
constexpr size_t kAcceptedAtOffset = 142;
constexpr size_t kExpiresAtOffset = 150;
constexpr size_t kChecksumOffset = 158;

size_t boundedLength(const char* value, size_t capacity) {
  const void* end = std::memchr(value, '\0', capacity);
  return end == nullptr ? capacity
                        : static_cast<const char*>(end) - value;
}

bool validId(const char* value, size_t capacity) {
  const size_t length = boundedLength(value, capacity);
  if (length < 8 || length >= capacity) {
    return false;
  }
  for (size_t index = 0; index < length; ++index) {
    const char byte = value[index];
    if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
        (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' ||
        byte == ':' || byte == '-') {
      continue;
    }
    return false;
  }
  return true;
}

bool validAction(RemoteCommandAction action) {
  switch (action) {
    case RemoteCommandAction::kDiagnosticSnapshot:
    case RemoteCommandAction::kSetLogLevel:
    case RemoteCommandAction::kRecalibrateMicrophone:
    case RemoteCommandAction::kRetryUpload:
    case RemoteCommandAction::kReboot:
    case RemoteCommandAction::kOpenDevOta:
      return true;
  }
  return false;
}

bool validStatus(CommandExecutionStatus status) {
  switch (status) {
    case CommandExecutionStatus::kAccepted:
    case CommandExecutionStatus::kRunning:
    case CommandExecutionStatus::kSucceeded:
    case CommandExecutionStatus::kFailed:
    case CommandExecutionStatus::kExpired:
    case CommandExecutionStatus::kRejected:
      return true;
  }
  return false;
}

[[maybe_unused]] bool recordsEqual(const CommandJournalRecord& left,
                                   const CommandJournalRecord& right) {
  return std::strcmp(left.commandId, right.commandId) == 0 &&
         std::strcmp(left.leaseId, right.leaseId) == 0 &&
         left.acceptedAtMs == right.acceptedAtMs &&
         left.expiresAtMs == right.expiresAtMs && left.action == right.action &&
         left.status == right.status;
}

void putU16(uint8_t* destination, uint16_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8U);
}

void putU32(uint8_t* destination, uint32_t value) {
  for (size_t index = 0; index < sizeof(value); ++index) {
    destination[index] = static_cast<uint8_t>(value >> (index * 8U));
  }
}

void putU64(uint8_t* destination, uint64_t value) {
  for (size_t index = 0; index < sizeof(value); ++index) {
    destination[index] = static_cast<uint8_t>(value >> (index * 8U));
  }
}

uint16_t getU16(const uint8_t* source) {
  return static_cast<uint16_t>(source[0]) |
         (static_cast<uint16_t>(source[1]) << 8U);
}

uint32_t getU32(const uint8_t* source) {
  uint32_t value = 0;
  for (size_t index = 0; index < sizeof(value); ++index) {
    value |= static_cast<uint32_t>(source[index]) << (index * 8U);
  }
  return value;
}

uint64_t getU64(const uint8_t* source) {
  uint64_t value = 0;
  for (size_t index = 0; index < sizeof(value); ++index) {
    value |= static_cast<uint64_t>(source[index]) << (index * 8U);
  }
  return value;
}

uint32_t crc32(const uint8_t* bytes, size_t length) {
  uint32_t crc = 0xffffffffU;
  for (size_t index = 0; index < length; ++index) {
    crc ^= bytes[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      const uint32_t lowBitMask = static_cast<uint32_t>(0U - (crc & 1U));
      crc = (crc >> 1U) ^ (0xedb88320U & lowBitMask);
    }
  }
  return crc ^ 0xffffffffU;
}

}  // namespace

bool remoteCommandEnvelopeIsValid(const RemoteCommandEnvelope& command) {
  if (!validId(command.commandId, sizeof(command.commandId)) ||
      !validId(command.leaseId, sizeof(command.leaseId)) ||
      !validAction(command.action) || command.createdAtMs > kMaxSigned64 ||
      command.expiresAtMs > kMaxSigned64 ||
      command.leaseExpiresAtMs > kMaxSigned64 ||
      command.createdAtMs > command.expiresAtMs ||
      command.leaseExpiresAtMs > command.expiresAtMs) {
    return false;
  }
  if (command.action == RemoteCommandAction::kSetLogLevel) {
    return command.durationSeconds >= 1 && command.durationSeconds <= 600 &&
           !command.requiresLocalConfirmation;
  }
  if (command.durationSeconds != 0 || command.detailedLog) {
    return false;
  }
  return command.action == RemoteCommandAction::kOpenDevOta
             ? command.requiresLocalConfirmation
             : !command.requiresLocalConfirmation;
}

bool commandExecutionStatusIsTerminal(CommandExecutionStatus status) {
  return status == CommandExecutionStatus::kSucceeded ||
         status == CommandExecutionStatus::kFailed ||
         status == CommandExecutionStatus::kExpired ||
         status == CommandExecutionStatus::kRejected;
}

bool commandJournalRecordIsValid(const CommandJournalRecord& record) {
  return validId(record.commandId, sizeof(record.commandId)) &&
         validId(record.leaseId, sizeof(record.leaseId)) &&
         validAction(record.action) && validStatus(record.status) &&
         record.acceptedAtMs <= kMaxSigned64 &&
         record.expiresAtMs <= kMaxSigned64 &&
         record.acceptedAtMs <= record.expiresAtMs;
}

CommandJournalRecoveryAction commandJournalRecoveryAction(
    const CommandJournalRecord& record) {
  if (!commandJournalRecordIsValid(record)) {
    return CommandJournalRecoveryAction::kInvalid;
  }
  return commandExecutionStatusIsTerminal(record.status)
             ? CommandJournalRecoveryAction::kAckPersistedTerminal
             : CommandJournalRecoveryAction::kFailInterruptedThenAck;
}

bool buildCommandAckId(const CommandJournalRecord& record, char* output,
                       size_t outputCapacity) {
  if (!commandJournalRecordIsValid(record) || output == nullptr ||
      outputCapacity < 37) {
    return false;
  }
  uint64_t first = 1469598103934665603ULL;
  uint64_t second = 7809847782465536322ULL;
  const char* values[] = {record.commandId, record.leaseId};
  for (const char* value : values) {
    for (const unsigned char* cursor =
             reinterpret_cast<const unsigned char*>(value);
         *cursor != '\0'; ++cursor) {
      first = (first ^ *cursor) * 1099511628211ULL;
      second = (second + *cursor) * 0x9e3779b185ebca87ULL;
      second ^= second >> 29U;
    }
    first = (first ^ 0xffU) * 1099511628211ULL;
    second ^= 0xa5a5a5a5a5a5a5a5ULL;
  }
  const uint8_t status = static_cast<uint8_t>(record.status);
  first = (first ^ status) * 1099511628211ULL;
  second = (second + status) * 0x9e3779b185ebca87ULL;
  char candidate[37] = {};
  const int written = std::snprintf(
      candidate, sizeof(candidate), "ack-%016llx%016llx",
      static_cast<unsigned long long>(first),
      static_cast<unsigned long long>(second));
  if (written != 36) {
    return false;
  }
  std::memcpy(output, candidate, sizeof(candidate));
  return true;
}

bool CommandJournal::restore(const CommandJournalRecord& record) {
  if (!commandJournalRecordIsValid(record)) {
    return false;
  }
  record_ = record;
  hasRecord_ = true;
  return true;
}

CommandAcceptance CommandJournal::consider(
    const RemoteCommandEnvelope& command, uint64_t serverUtcMs) {
  if (!remoteCommandEnvelopeIsValid(command) || serverUtcMs > kMaxSigned64) {
    return CommandAcceptance::kInvalid;
  }
  if (hasRecord_ && std::strcmp(command.commandId, record_.commandId) == 0) {
    std::memcpy(record_.leaseId, command.leaseId, sizeof(record_.leaseId));
    return CommandAcceptance::kReplayExisting;
  }
  if (serverUtcMs >= command.expiresAtMs) {
    return CommandAcceptance::kExpired;
  }
  if (serverUtcMs >= command.leaseExpiresAtMs) {
    return CommandAcceptance::kLeaseExpired;
  }
  if (hasRecord_ && !commandExecutionStatusIsTerminal(record_.status)) {
    return CommandAcceptance::kBusy;
  }

  CommandJournalRecord candidate = {};
  std::memcpy(candidate.commandId, command.commandId,
              sizeof(candidate.commandId));
  std::memcpy(candidate.leaseId, command.leaseId,
              sizeof(candidate.leaseId));
  candidate.acceptedAtMs = serverUtcMs;
  candidate.expiresAtMs = command.expiresAtMs;
  candidate.action = command.action;
  candidate.status = CommandExecutionStatus::kAccepted;
  record_ = candidate;
  hasRecord_ = true;
  return CommandAcceptance::kAcceptedNew;
}

bool CommandJournal::transition(CommandExecutionStatus next) {
  if (!hasRecord_ || !validStatus(next)) {
    return false;
  }
  if (record_.status == next) {
    return true;
  }
  if (commandExecutionStatusIsTerminal(record_.status)) {
    return false;
  }
  const bool allowed =
      record_.status == CommandExecutionStatus::kAccepted
          ? (next == CommandExecutionStatus::kRunning ||
             commandExecutionStatusIsTerminal(next))
          : (record_.status == CommandExecutionStatus::kRunning &&
             commandExecutionStatusIsTerminal(next));
  if (!allowed) {
    return false;
  }
  record_.status = next;
  return true;
}

CommandJournalBlobError encodeCommandJournalRecord(
    const CommandJournalRecord& record, uint8_t* output,
    size_t outputCapacity) {
  if (output == nullptr) {
    return CommandJournalBlobError::kNullArgument;
  }
  if (outputCapacity < kCommandJournalBlobSize) {
    return CommandJournalBlobError::kWrongLength;
  }
  if (!commandJournalRecordIsValid(record)) {
    return CommandJournalBlobError::kInvalidRecord;
  }

  uint8_t blob[kCommandJournalBlobSize] = {};
  std::memcpy(blob, kMagic, sizeof(kMagic));
  putU16(blob + 4, kFormatVersion);
  putU16(blob + 6, kPayloadLength);
  const size_t idLength = std::strlen(record.commandId);
  blob[kIdLengthOffset] = static_cast<uint8_t>(idLength);
  std::memcpy(blob + kIdOffset, record.commandId, idLength);
  const size_t leaseIdLength = std::strlen(record.leaseId);
  blob[kLeaseIdLengthOffset] = static_cast<uint8_t>(leaseIdLength);
  std::memcpy(blob + kLeaseIdOffset, record.leaseId, leaseIdLength);
  blob[kActionOffset] = static_cast<uint8_t>(record.action);
  blob[kStatusOffset] = static_cast<uint8_t>(record.status);
  putU64(blob + kAcceptedAtOffset, record.acceptedAtMs);
  putU64(blob + kExpiresAtOffset, record.expiresAtMs);
  putU32(blob + kChecksumOffset, crc32(blob, kChecksumOffset));
  std::memcpy(output, blob, sizeof(blob));
  return CommandJournalBlobError::kNone;
}

CommandJournalBlobError decodeCommandJournalRecord(
    const uint8_t* input, size_t inputLength, CommandJournalRecord* output) {
  if (input == nullptr || output == nullptr) {
    return CommandJournalBlobError::kNullArgument;
  }
  if (inputLength != kCommandJournalBlobSize) {
    return CommandJournalBlobError::kWrongLength;
  }
  if (std::memcmp(input, kMagic, sizeof(kMagic)) != 0) {
    return CommandJournalBlobError::kBadMagic;
  }
  if (getU16(input + 4) != kFormatVersion) {
    return CommandJournalBlobError::kUnsupportedVersion;
  }
  if (getU16(input + 6) != kPayloadLength) {
    return CommandJournalBlobError::kBadPayloadLength;
  }
  if (getU32(input + kChecksumOffset) != crc32(input, kChecksumOffset)) {
    return CommandJournalBlobError::kChecksumMismatch;
  }
  const size_t idLength = input[kIdLengthOffset];
  if (idLength < 8 || idLength >= CommandJournalRecord::kIdCapacity) {
    return CommandJournalBlobError::kInvalidRecord;
  }
  const size_t leaseIdLength = input[kLeaseIdLengthOffset];
  if (leaseIdLength < 8 ||
      leaseIdLength >= CommandJournalRecord::kIdCapacity) {
    return CommandJournalBlobError::kInvalidRecord;
  }

  CommandJournalRecord candidate = {};
  std::memcpy(candidate.commandId, input + kIdOffset, idLength);
  candidate.commandId[idLength] = '\0';
  std::memcpy(candidate.leaseId, input + kLeaseIdOffset, leaseIdLength);
  candidate.leaseId[leaseIdLength] = '\0';
  candidate.action = static_cast<RemoteCommandAction>(input[kActionOffset]);
  candidate.status =
      static_cast<CommandExecutionStatus>(input[kStatusOffset]);
  candidate.acceptedAtMs = getU64(input + kAcceptedAtOffset);
  candidate.expiresAtMs = getU64(input + kExpiresAtOffset);
  if (!commandJournalRecordIsValid(candidate)) {
    return CommandJournalBlobError::kInvalidRecord;
  }
  *output = candidate;
  return CommandJournalBlobError::kNone;
}

#if defined(ARDUINO_ARCH_ESP32)

#include <Preferences.h>

namespace {

constexpr char kPreferencesNamespace[] = "m5cmd";
constexpr char kActiveSlotKey[] = "active";
constexpr char kSlotKeys[][7] = {"a_blob", "b_blob"};
constexpr uint8_t kNoSlot = 0xff;

enum class SlotReadResult : uint8_t { kOk, kMissing, kInvalid, kReadFailed };

struct SlotSnapshot {
  SlotReadResult result = SlotReadResult::kMissing;
  CommandJournalRecord record = {};
};

SlotSnapshot readSlot(Preferences& preferences, uint8_t slot) {
  SlotSnapshot snapshot;
  const size_t length = preferences.getBytesLength(kSlotKeys[slot]);
  if (length == 0) return snapshot;
  if (length != kCommandJournalBlobSize) {
    snapshot.result = SlotReadResult::kInvalid;
    return snapshot;
  }
  uint8_t blob[kCommandJournalBlobSize] = {};
  if (preferences.getBytes(kSlotKeys[slot], blob, sizeof(blob)) !=
      sizeof(blob)) {
    snapshot.result = SlotReadResult::kReadFailed;
    return snapshot;
  }
  if (decodeCommandJournalRecord(blob, sizeof(blob), &snapshot.record) !=
      CommandJournalBlobError::kNone) {
    snapshot.result = SlotReadResult::kInvalid;
    return snapshot;
  }
  snapshot.result = SlotReadResult::kOk;
  return snapshot;
}

uint8_t selectSlot(uint8_t active, const SlotSnapshot snapshots[2]) {
  if (active <= 1 && snapshots[active].result == SlotReadResult::kOk) {
    return active;
  }
  if (active <= 1) {
    const uint8_t fallback = active == 0 ? 1 : 0;
    return snapshots[fallback].result == SlotReadResult::kOk ? fallback
                                                             : kNoSlot;
  }
  if (snapshots[0].result == SlotReadResult::kOk &&
      snapshots[1].result == SlotReadResult::kOk) {
    return snapshots[1].record.acceptedAtMs >
                   snapshots[0].record.acceptedAtMs
               ? 1
               : 0;
  }
  return snapshots[0].result == SlotReadResult::kOk
             ? 0
             : snapshots[1].result == SlotReadResult::kOk ? 1 : kNoSlot;
}

void readSlots(Preferences& preferences, SlotSnapshot snapshots[2]) {
  snapshots[0] = readSlot(preferences, 0);
  snapshots[1] = readSlot(preferences, 1);
}

}  // namespace

CommandJournalStorageResult loadCommandJournalRecord(
    CommandJournalRecord* output) {
  if (output == nullptr) return CommandJournalStorageResult::kInvalidRecord;
  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, true)) {
    if (!preferences.begin(kPreferencesNamespace, false)) {
      return CommandJournalStorageResult::kOpenFailed;
    }
    preferences.end();
    return CommandJournalStorageResult::kNotStored;
  }
  SlotSnapshot snapshots[2];
  readSlots(preferences, snapshots);
  const uint8_t selected =
      selectSlot(preferences.getUChar(kActiveSlotKey, kNoSlot), snapshots);
  preferences.end();
  if (selected == kNoSlot) {
    if (snapshots[0].result == SlotReadResult::kReadFailed ||
        snapshots[1].result == SlotReadResult::kReadFailed) {
      return CommandJournalStorageResult::kReadFailed;
    }
    if (snapshots[0].result == SlotReadResult::kInvalid ||
        snapshots[1].result == SlotReadResult::kInvalid) {
      return CommandJournalStorageResult::kInvalidStoredData;
    }
    return CommandJournalStorageResult::kNotStored;
  }
  *output = snapshots[selected].record;
  return CommandJournalStorageResult::kOk;
}

CommandJournalStorageResult saveCommandJournalRecord(
    const CommandJournalRecord& record) {
  uint8_t blob[kCommandJournalBlobSize] = {};
  if (encodeCommandJournalRecord(record, blob, sizeof(blob)) !=
      CommandJournalBlobError::kNone) {
    return CommandJournalStorageResult::kInvalidRecord;
  }
  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, false)) {
    return CommandJournalStorageResult::kOpenFailed;
  }
  SlotSnapshot snapshots[2];
  readSlots(preferences, snapshots);
  const uint8_t selected =
      selectSlot(preferences.getUChar(kActiveSlotKey, kNoSlot), snapshots);
  const uint8_t target = selected == 0 ? 1 : 0;
  if (preferences.putBytes(kSlotKeys[target], blob, sizeof(blob)) !=
      sizeof(blob)) {
    preferences.end();
    return CommandJournalStorageResult::kWriteFailed;
  }
  const SlotSnapshot verified = readSlot(preferences, target);
  if (verified.result != SlotReadResult::kOk ||
      !recordsEqual(verified.record, record)) {
    preferences.end();
    return CommandJournalStorageResult::kVerifyFailed;
  }
  if (preferences.putUChar(kActiveSlotKey, target) != sizeof(uint8_t) ||
      preferences.getUChar(kActiveSlotKey, kNoSlot) != target) {
    preferences.end();
    return CommandJournalStorageResult::kWriteFailed;
  }
  preferences.end();
  return CommandJournalStorageResult::kOk;
}

#else

CommandJournalStorageResult loadCommandJournalRecord(CommandJournalRecord*) {
  return CommandJournalStorageResult::kUnsupportedPlatform;
}

CommandJournalStorageResult saveCommandJournalRecord(
    const CommandJournalRecord&) {
  return CommandJournalStorageResult::kUnsupportedPlatform;
}

#endif
