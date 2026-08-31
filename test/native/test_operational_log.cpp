#include <cassert>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "operational_log.h"

namespace {

OperationalLogEvent eventFor(uint64_t sequence, OperationalLogLevel level) {
  OperationalLogEvent event = {};
  event.sequence = sequence;
  event.uptimeMs = 1000 + sequence;
  event.level = level;
  event.code = OperationalLogCode::kBackendRequest;
  event.value0 = static_cast<int32_t>(sequence);
  event.value1 = -1;
  return event;
}

}  // namespace

int main() {
  static_assert(std::is_standard_layout<OperationalLogEvent>::value,
                "log event must remain standard-layout");
  static_assert(std::is_trivially_copyable<OperationalLogEvent>::value,
                "log event must remain trivially copyable");

  OperationalLogRing ring;
  assert(ring.size() == 0);
  assert(ring.capacity() == 64);
  assert(ring.copyPrefix(nullptr, 1) == 0);

  for (uint64_t index = 0;
       index < OperationalLogRing::kCapacity -
                   OperationalLogRing::kReservedCriticalSlots;
       ++index) {
    assert(ring.push(eventFor(index, OperationalLogLevel::kInfo)) ==
           OperationalLogPushResult::kStored);
  }
  assert(ring.push(eventFor(100, OperationalLogLevel::kDebug)) ==
         OperationalLogPushResult::kDroppedVerbose);
  assert(ring.droppedVerbose() == 1);

  for (uint64_t index = 0;
       index < OperationalLogRing::kReservedCriticalSlots; ++index) {
    assert(ring.push(eventFor(200 + index, OperationalLogLevel::kWarning)) ==
           OperationalLogPushResult::kStored);
  }
  assert(ring.size() == OperationalLogRing::kCapacity);
  assert(ring.push(eventFor(300, OperationalLogLevel::kError)) ==
         OperationalLogPushResult::kDroppedCritical);
  assert(ring.droppedCritical() == 1);

  OperationalLogEvent prefix[9] = {};
  assert(ring.copyPrefix(prefix, 9) == 9);
  OperationalLogEvent changed[9] = {};
  for (size_t index = 0; index < 9; ++index) {
    changed[index] = prefix[index];
  }
  ++changed[4].value0;
  assert(!ring.commitPrefix(changed, 9));
  assert(ring.commitPrefix(prefix, 9));
  assert(ring.size() == OperationalLogRing::kCapacity - 9);

  OperationalLogEvent invalid = eventFor(500, OperationalLogLevel::kInfo);
  invalid.level = static_cast<OperationalLogLevel>(255);
  assert(ring.push(invalid) == OperationalLogPushResult::kInvalid);
  assert(ring.rejectedInvalid() == 1);
  invalid = eventFor(std::numeric_limits<uint64_t>::max(),
                     OperationalLogLevel::kInfo);
  assert(ring.push(invalid) == OperationalLogPushResult::kInvalid);

  RemoteLogSession session;
  assert(session.mode(10) == RemoteLogMode::kOperational);
  assert(!session.accepts(OperationalLogLevel::kSensorDetail, 10));
  assert(!session.beginDetailed(10, 0));
  assert(session.beginDetailed(1000, 2000));
  assert(session.mode(2999) == RemoteLogMode::kDetailed);
  assert(session.remainingMs(2999) == 1);
  assert(session.accepts(OperationalLogLevel::kSensorDetail, 2999));
  assert(session.mode(3000) == RemoteLogMode::kOperational);
  assert(!session.accepts(OperationalLogLevel::kSensorDetail, 3000));

  assert(session.beginDetailed(5000, RemoteLogSession::kMaximumDurationMs * 2));
  assert(session.remainingMs(5000) ==
         RemoteLogSession::kMaximumDurationMs);
  session.stop();
  assert(session.remainingMs(5000) == 0);
  assert(session.accepts(OperationalLogLevel::kDebug, 5000));

  return 0;
}
