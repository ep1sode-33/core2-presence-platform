#include <array>
#include <cassert>
#include <cstring>

#include "command_journal.h"

namespace {

RemoteCommandEnvelope command(const char* id,
                              RemoteCommandAction action =
                                  RemoteCommandAction::kRetryUpload) {
  RemoteCommandEnvelope value = {};
  std::strcpy(value.commandId, id);
  std::strcpy(value.leaseId, "lease-01234567");
  value.createdAtMs = 1000;
  value.expiresAtMs = 10000;
  value.leaseExpiresAtMs = 5000;
  value.action = action;
  value.requiresLocalConfirmation =
      action == RemoteCommandAction::kOpenDevOta;
  return value;
}

}  // namespace

int main() {
  RemoteCommandEnvelope first = command("command-00000001");
  assert(remoteCommandEnvelopeIsValid(first));

  CommandJournal journal;
  assert(journal.consider(first, 2000) == CommandAcceptance::kAcceptedNew);
  assert(journal.hasRecord());
  assert(journal.record().status == CommandExecutionStatus::kAccepted);
  assert(journal.consider(first, 2001) == CommandAcceptance::kReplayExisting);
  RemoteCommandEnvelope reLeased = first;
  std::strcpy(reLeased.leaseId, "lease-76543210");
  assert(journal.consider(reLeased, 2001) ==
         CommandAcceptance::kReplayExisting);
  assert(std::strcmp(journal.record().leaseId, "lease-76543210") == 0);

  RemoteCommandEnvelope second = command("command-00000002");
  assert(journal.consider(second, 2002) == CommandAcceptance::kBusy);
  assert(journal.transition(CommandExecutionStatus::kRunning));
  assert(journal.transition(CommandExecutionStatus::kSucceeded));
  assert(journal.transition(CommandExecutionStatus::kSucceeded));
  assert(!journal.transition(CommandExecutionStatus::kFailed));
  assert(journal.consider(second, 2003) == CommandAcceptance::kAcceptedNew);

  RemoteCommandEnvelope expired = command("command-00000003");
  assert(journal.transition(CommandExecutionStatus::kRejected));
  assert(journal.consider(expired, expired.expiresAtMs) ==
         CommandAcceptance::kExpired);
  expired.expiresAtMs = 20000;
  assert(journal.consider(expired, expired.leaseExpiresAtMs) ==
         CommandAcceptance::kLeaseExpired);

  RemoteCommandEnvelope log =
      command("command-00000004", RemoteCommandAction::kSetLogLevel);
  assert(!remoteCommandEnvelopeIsValid(log));
  log.durationSeconds = 600;
  log.detailedLog = true;
  assert(remoteCommandEnvelopeIsValid(log));
  log.durationSeconds = 601;
  assert(!remoteCommandEnvelopeIsValid(log));

  RemoteCommandEnvelope ota =
      command("command-00000005", RemoteCommandAction::kOpenDevOta);
  assert(remoteCommandEnvelopeIsValid(ota));
  ota.requiresLocalConfirmation = false;
  assert(!remoteCommandEnvelopeIsValid(ota));

  const CommandJournalRecord durable = journal.record();
  std::array<uint8_t, kCommandJournalBlobSize> blob = {};
  assert(encodeCommandJournalRecord(durable, blob.data(), blob.size()) ==
         CommandJournalBlobError::kNone);
  assert(blob[0] == 'M' && blob[1] == '5' && blob[2] == 'C' &&
         blob[3] == 'J');
  CommandJournalRecord decoded = {};
  assert(decodeCommandJournalRecord(blob.data(), blob.size(), &decoded) ==
         CommandJournalBlobError::kNone);
  assert(std::strcmp(decoded.commandId, durable.commandId) == 0);
  assert(std::strcmp(decoded.leaseId, durable.leaseId) == 0);
  assert(decoded.acceptedAtMs == durable.acceptedAtMs);
  assert(decoded.expiresAtMs == durable.expiresAtMs);
  assert(decoded.action == durable.action);
  assert(decoded.status == durable.status);
  assert(commandJournalRecoveryAction(decoded) ==
         CommandJournalRecoveryAction::kAckPersistedTerminal);
  char firstAckId[CommandJournalRecord::kIdCapacity] = {};
  char secondAckId[CommandJournalRecord::kIdCapacity] = {};
  assert(buildCommandAckId(decoded, firstAckId, sizeof(firstAckId)));
  assert(buildCommandAckId(decoded, secondAckId, sizeof(secondAckId)));
  assert(std::strcmp(firstAckId, secondAckId) == 0);
  decoded.status = CommandExecutionStatus::kRunning;
  assert(commandJournalRecoveryAction(decoded) ==
         CommandJournalRecoveryAction::kFailInterruptedThenAck);
  assert(buildCommandAckId(decoded, secondAckId, sizeof(secondAckId)));
  assert(std::strcmp(firstAckId, secondAckId) != 0);
  decoded.status = durable.status;

  // Simulate reset at the exact boundary after terminal persistence and before
  // the HTTP ACK. Decoding the durable record must still schedule an ACK-only
  // recovery, never wait for the backend to lease the command again.
  CommandJournalRecord afterTerminalWrite = {};
  assert(decodeCommandJournalRecord(blob.data(), blob.size(),
                                    &afterTerminalWrite) ==
         CommandJournalBlobError::kNone);
  assert(commandJournalRecoveryAction(afterTerminalWrite) ==
         CommandJournalRecoveryAction::kAckPersistedTerminal);

  CommandJournal restored;
  assert(restored.restore(decoded));
  assert(restored.consider(second, 3000) ==
         CommandAcceptance::kReplayExisting);

  blob[20] ^= 0x20;
  assert(decodeCommandJournalRecord(blob.data(), blob.size(), &decoded) ==
         CommandJournalBlobError::kChecksumMismatch);
  assert(loadCommandJournalRecord(&decoded) ==
         CommandJournalStorageResult::kUnsupportedPlatform);
  assert(saveCommandJournalRecord(durable) ==
         CommandJournalStorageResult::kUnsupportedPlatform);
  return 0;
}
