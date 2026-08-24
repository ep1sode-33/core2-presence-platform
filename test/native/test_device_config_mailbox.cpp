#include <cassert>

#include "device_config_mailbox.h"

namespace {

PresenceConfig configAtRevision(uint64_t revision) {
  PresenceConfig config = defaultPresenceConfig();
  config.revision = revision;
  return config;
}

void testNewestPendingRevisionWins() {
  DeviceConfigMailbox mailbox;
  assert(!mailbox.hasPending());
  assert(mailbox.publish(configAtRevision(1)) ==
         DeviceConfigPublishResult::kPublished);
  assert(mailbox.publish(configAtRevision(3)) ==
         DeviceConfigPublishResult::kReplacedPending);
  assert(mailbox.publish(configAtRevision(2)) ==
         DeviceConfigPublishResult::kIgnoredStalePending);

  PresenceConfig taken = {};
  assert(mailbox.take(&taken));
  assert(taken.revision == 3);
  assert(!mailbox.hasPending());
  assert(!mailbox.take(&taken));
  assert(!mailbox.take(nullptr));
}

void testAppliedAcknowledgementIsMonotonic() {
  DeviceConfigMailbox mailbox;
  mailbox.acknowledgeAppliedRevision(4);
  mailbox.acknowledgeAppliedRevision(2);
  assert(mailbox.acknowledgedAppliedRevision() == 4);
  assert(mailbox.publish(configAtRevision(4)) ==
         DeviceConfigPublishResult::kIgnoredAlreadyApplied);
  assert(mailbox.publish(configAtRevision(3)) ==
         DeviceConfigPublishResult::kIgnoredAlreadyApplied);
  assert(mailbox.publish(configAtRevision(5)) ==
         DeviceConfigPublishResult::kPublished);

  // If the worker republishes while main is applying the same revision, the
  // acknowledgement clears that redundant pending snapshot.
  PresenceConfig taken = {};
  assert(mailbox.take(&taken));
  assert(taken.revision == 5);
  assert(mailbox.publish(configAtRevision(5)) ==
         DeviceConfigPublishResult::kPublished);
  mailbox.acknowledgeAppliedRevision(5);
  assert(mailbox.acknowledgedAppliedRevision() == 5);
  assert(!mailbox.hasPending());
}

void testInvalidConfigNeverEntersMailbox() {
  DeviceConfigMailbox mailbox;
  PresenceConfig invalid = configAtRevision(8);
  invalid.pirHoldMs = 999;
  assert(mailbox.publish(invalid) ==
         DeviceConfigPublishResult::kInvalidConfig);
  assert(!mailbox.hasPending());
}

}  // namespace

int main() {
  testNewestPendingRevisionWins();
  testAppliedAcknowledgementIsMonotonic();
  testInvalidConfigNeverEntersMailbox();
  return 0;
}
