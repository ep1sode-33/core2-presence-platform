#include <cassert>
#include <cstring>
#include <thread>

#include "control_mailbox.h"

int main() {
  ControlMailbox mailbox;
  MainControlRequest request = {};
  std::strcpy(request.commandId, "cmd-0123456789abcdef0123456789abcdef");
  request.action = RemoteCommandAction::kRecalibrateMicrophone;
  request.expiresAtMs = 1000;
  assert(mailbox.publishRequest(request));
  assert(mailbox.busy());
  assert(!mailbox.publishRequest(request));

  MainControlRequest received = {};
  std::thread consumer([&]() { assert(mailbox.takeRequest(&received)); });
  consumer.join();
  assert(received.version != 0);
  assert(std::strcmp(received.commandId, request.commandId) == 0);

  MainControlResult wrong = {};
  std::strcpy(wrong.commandId, request.commandId);
  wrong.code = MainControlResultCode::kSucceeded;
  wrong.requestVersion = received.version + 1;
  assert(!mailbox.publishResult(wrong));

  MainControlResult result = wrong;
  result.requestVersion = received.version;
  assert(mailbox.publishResult(result));
  assert(mailbox.busy());
  assert(!mailbox.publishResult(result));
  MainControlResult delivered = {};
  assert(mailbox.takeResult(&delivered));
  assert(delivered.code == MainControlResultCode::kSucceeded);
  assert(!mailbox.busy());
  assert(!mailbox.takeResult(&delivered));
  return 0;
}
