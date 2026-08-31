#include <cassert>
#include <cstring>
#include <string>

#include "command_ack_protocol.h"

namespace {

bool append(void* context, const char* data, size_t size) {
  static_cast<std::string*>(context)->append(data, size);
  return true;
}

}  // namespace

int main() {
  CommandJournalRecord record = {};
  std::strcpy(record.commandId, "cmd-0123456789abcdef0123456789abcdef");
  std::strcpy(record.leaseId, "lease-0123456789abcdef0123456789abcdef");
  record.acceptedAtMs = 100;
  record.expiresAtMs = 1000;
  record.action = RemoteCommandAction::kRetryUpload;
  record.status = CommandExecutionStatus::kRunning;

  std::string body;
  assert(writeCommandAckJson(record, {&body, append}));
  char ackId[CommandJournalRecord::kIdCapacity] = {};
  assert(buildCommandAckId(record, ackId, sizeof(ackId)));
  assert(body.find(std::string("\"ack_id\":\"") + ackId + "\"") !=
         std::string::npos);
  assert(body.find("\"status\":\"running\"") != std::string::npos);
  assert(body.find("\"result\":null") != std::string::npos);

  const std::string response =
      std::string("{\"ack_id\":\"") + ackId +
      "\",\"command_id\":\"" + record.commandId +
      "\",\"status\":\"running\",\"duplicate\":false,"
      "\"server_utc_ms\":1700000000000}";
  const CommandAckResponseResult parsed =
      parseCommandAckResponse(response.data(), response.size(), record);
  assert(parsed.ok());
  assert(!parsed.duplicate);
  assert(parsed.serverUtcMs == 1700000000000ULL);

  std::string wrong = response;
  wrong.replace(wrong.find("running"), 7, "failed");
  assert(parseCommandAckResponse(wrong.data(), wrong.size(), record).error ==
         CommandAckResponseError::kMismatch);

  wrong = response;
  wrong.insert(wrong.size() - 1, ",\"extra\":1");
  assert(parseCommandAckResponse(wrong.data(), wrong.size(), record).error ==
         CommandAckResponseError::kUnknownField);
  return 0;
}
