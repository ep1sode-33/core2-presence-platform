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

  for (uint64_t expected = 0; expected < TelemetryQueue::kCapacity; ++expected) {
    assert(queue.front() != nullptr);
    assert(queue.front()->seq == expected);
    assert(queue.pop());
  }
  assert(queue.front() == nullptr);
  assert(!queue.pop());

  // Reuse every physical slot after head has wrapped around. Samples should
  // regain their full non-critical allowance once transitions are drained.
  for (size_t index = 0; index < sampleLimit; ++index) {
    assert(queue.push(sample(1000 + index)) == QueuePushResult::kStored);
  }
  assert(queue.push(sample(2000)) == QueuePushResult::kSampleDropped);
  for (uint64_t expected = 1000; expected < 1000 + sampleLimit; ++expected) {
    assert(queue.front() != nullptr);
    assert(queue.front()->seq == expected);
    assert(queue.pop());
  }
  assert(queue.size() == 0);

  // A transition may still consume a slot immediately after that wraparound.
  assert(queue.push(transition(3000)) == QueuePushResult::kStored);
  assert(queue.front()->seq == 3000);
  assert(queue.pop());

  assert(std::strcmp(presenceStateWireName(PresenceState::kPresent),
                     "present") == 0);
  assert(std::strcmp(transitionReasonWireName(TransitionReason::kSoundBridge),
                     "sound_bridge") == 0);
  return 0;
}
