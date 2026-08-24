#include <cassert>
#include <cstdint>
#include <cstring>

#include "presence_types.h"
#include "telemetry.h"

namespace {

TelemetryRecord sample(uint64_t seq) {
  TelemetryRecord record;
  record.kind = TelemetryKind::kSample;
  record.seq = seq;
  return record;
}

TelemetryRecord transition(uint64_t seq) {
  TelemetryRecord record;
  record.kind = TelemetryKind::kTransition;
  record.seq = seq;
  return record;
}

}  // namespace

int main() {
  TelemetryQueue queue;
  const size_t sampleLimit =
      TelemetryQueue::kCapacity - TelemetryQueue::kReservedTransitionSlots;
  for (size_t index = 0; index < sampleLimit; ++index) {
    assert(queue.push(sample(index)) == QueuePushResult::kStored);
  }
  assert(queue.push(sample(sampleLimit)) == QueuePushResult::kSampleDropped);
  assert(queue.droppedSamples() == 1);

  for (size_t index = sampleLimit; index < TelemetryQueue::kCapacity; ++index) {
    assert(queue.push(transition(index)) == QueuePushResult::kStored);
  }
  assert(queue.size() == TelemetryQueue::kCapacity);
  assert(queue.push(transition(TelemetryQueue::kCapacity)) ==
         QueuePushResult::kCriticalDropped);
  assert(queue.droppedCritical() == 1);

  TelemetryRecord copied[TelemetryQueue::kCapacity] = {};
  assert(queue.copyPrefix(copied, TelemetryQueue::kCapacity) ==
         TelemetryQueue::kCapacity);
  for (uint64_t expected = 0; expected < TelemetryQueue::kCapacity; ++expected) {
    assert(copied[expected].seq == expected);
  }
  assert(queue.commitPrefix(copied, TelemetryQueue::kCapacity));
  TelemetryRecord front;
  assert(!queue.peek(front));
  assert(!queue.commitPrefix(copied, 0));

  // Reuse every physical slot after head has wrapped around. Samples should
  // regain their full non-critical allowance once transitions are drained.
  for (size_t index = 0; index < sampleLimit; ++index) {
    assert(queue.push(sample(1000 + index)) == QueuePushResult::kStored);
  }
  assert(queue.push(sample(2000)) == QueuePushResult::kSampleDropped);
  assert(queue.copyPrefix(copied, sampleLimit) == sampleLimit);
  for (uint64_t expected = 1000; expected < 1000 + sampleLimit; ++expected) {
    assert(copied[expected - 1000].seq == expected);
  }
  assert(queue.commitPrefix(copied, sampleLimit));
  assert(queue.size() == 0);

  // A transition may still consume a slot immediately after that wraparound.
  assert(queue.push(transition(3000)) == QueuePushResult::kStored);
  assert(queue.peek(front));
  assert(front.seq == 3000);

  TelemetryRecord wrong = front;
  wrong.appliedConfigRevision = 1;
  assert(!queue.commitPrefix(&wrong, 1));
  assert(queue.size() == 1);
  assert(queue.commitPrefix(&front, 1));

  TelemetryRecord revisions[4] = {sample(4000), sample(4001), sample(4002),
                                  sample(4003)};
  revisions[0].appliedConfigRevision = 7;
  revisions[1].appliedConfigRevision = 7;
  revisions[2].appliedConfigRevision = 8;
  revisions[3].appliedConfigRevision = 8;
  assert(contiguousRevisionPrefix(revisions, 4) == 2);
  assert(contiguousRevisionPrefix(revisions + 2, 2) == 2);
  assert(contiguousRevisionPrefix(nullptr, 4) == 0);
  assert(contiguousRevisionPrefix(revisions, 0) == 0);

  assert(std::strcmp(presenceStateWireName(PresenceState::kPresent),
                     "present") == 0);
  assert(std::strcmp(transitionReasonWireName(TransitionReason::kSoundBridge),
                     "sound_bridge") == 0);
  return 0;
}
