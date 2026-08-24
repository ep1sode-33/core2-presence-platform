#include <cassert>
#include <cstdint>
#include <cstring>

#include "feedback_spool_name.h"

namespace {

FeedbackRecord makeRecord() {
  constexpr char kBootId[] = "0123456789abcdef0123456789abcdef";
  TelemetryRecord sample = {};
  sample.kind = TelemetryKind::kSample;
  sample.seq = 42;
  sample.uptimeMs = 12345;
  sample.sample.state = PresenceState::kIdle;
  QueuedSampleReference reference = {};
  assert(bindQueuedSampleForTouchFeedback(
      kBootId, sample, QueuePushResult::kStored, reference));
  FeedbackRecord record = {};
  assert(buildTouchFeedbackRecord(
      reference, TouchPresenceChoice::kRoomWasAbsent, record));
  return record;
}

bool sameMetadata(const FeedbackSpoolFileMetadata& left,
                  const FeedbackSpoolFileMetadata& right) {
  return std::strcmp(left.path, right.path) == 0 &&
         std::strcmp(left.readyPath, right.readyPath) == 0 &&
         std::strcmp(left.telemetryBatchId, right.telemetryBatchId) == 0 &&
         std::strcmp(left.feedbackId, right.feedbackId) == 0 &&
         std::strcmp(left.bootId, right.bootId) == 0 &&
         left.seq == right.seq &&
         left.linkedSampleUptimeMs == right.linkedSampleUptimeMs &&
         left.actualPresence == right.actualPresence &&
         left.observedState == right.observedState;
}

}  // namespace

int main() {
  const FeedbackRecord record = makeRecord();
  FeedbackSpoolFileMetadata metadata = {};
  assert(buildFeedbackSpoolFileMetadata(record, metadata));
  assert(std::strcmp(
             metadata.path,
             "/feedback/wait/q_0123456789abcdef0123456789abcdef_"
             "000000000000002a_0000000000003039_a_i.m5fb") == 0);
  assert(std::strcmp(
             metadata.readyPath,
             "/feedback/ready/q_0123456789abcdef0123456789abcdef_"
             "000000000000002a_0000000000003039_a_i.m5fb") == 0);
  assert(std::strcmp(
             metadata.telemetryBatchId,
             "b:0123456789abcdef0123456789abcdef:000000000000002a:"
             "000000000000002a:001") == 0);
  assert(std::strcmp(
             metadata.feedbackId,
             "f:0123456789abcdef0123456789abcdef:000000000000002a") ==
         0);
  assert(std::strcmp(metadata.bootId,
                     "0123456789abcdef0123456789abcdef") == 0);
  assert(metadata.seq == 42);
  assert(metadata.linkedSampleUptimeMs == 12345);
  assert(metadata.actualPresence == ActualPresence::kAbsent);
  assert(metadata.observedState == PresenceState::kIdle);

  FeedbackSpoolFileMetadata parsed = {};
  assert(parseFeedbackSpoolFileMetadata(metadata.path, parsed));
  assert(sameMetadata(parsed, metadata));
  assert(parseFeedbackSpoolFileMetadata(
      "/feedback/ready/q_0123456789abcdef0123456789abcdef_"
      "000000000000002a_0000000000003039_a_i.m5fb",
      parsed));
  assert(sameMetadata(parsed, metadata));
  assert(parseFeedbackSpoolFileMetadata(metadata.path + 10, parsed));

  FeedbackRecord restored = {};
  assert(feedbackRecordFromSpoolFileMetadata(parsed, restored));
  assert(std::strcmp(restored.feedbackId, record.feedbackId) == 0);
  assert(std::strcmp(restored.bootId, record.bootId) == 0);
  assert(restored.seq == record.seq);
  assert(restored.linkedSampleUptimeMs == record.linkedSampleUptimeMs);
  assert(restored.actualPresence == record.actualPresence);
  assert(restored.observedState == record.observedState);
  FeedbackSpoolFileMetadata mutatedMetadata = parsed;
  mutatedMetadata.feedbackId[2] = '1';
  assert(!feedbackRecordFromSpoolFileMetadata(mutatedMetadata, restored));
  mutatedMetadata = parsed;
  mutatedMetadata.path[0] = 'x';
  assert(!feedbackRecordFromSpoolFileMetadata(mutatedMetadata, restored));
  mutatedMetadata = parsed;
  mutatedMetadata.readyPath[0] = 'x';
  assert(!feedbackRecordFromSpoolFileMetadata(mutatedMetadata, restored));
  mutatedMetadata = parsed;
  mutatedMetadata.telemetryBatchId[0] = 'x';
  assert(!feedbackRecordFromSpoolFileMetadata(mutatedMetadata, restored));

  FeedbackRecord mutated = record;
  mutated.feedbackId[2] = '1';
  assert(!buildFeedbackSpoolFileMetadata(mutated, parsed));

  assert(!parseFeedbackSpoolFileMetadata(nullptr, parsed));
  assert(!parseFeedbackSpoolFileMetadata("", parsed));
  assert(!parseFeedbackSpoolFileMetadata(
      "q_0123456789abcdef0123456789abcdef_000000000000002A_"
      "0000000000003039_a_i.m5fb",
      parsed));
  assert(!parseFeedbackSpoolFileMetadata(
      "q_0123456789abcdef0123456789abcdef_8000000000000000_"
      "0000000000003039_a_i.m5fb",
      parsed));
  assert(!parseFeedbackSpoolFileMetadata(
      "q_0123456789abcdef0123456789abcdef_000000000000002a_"
      "0000000000003039_a_i.m5fb.extra",
      parsed));
  assert(!parseFeedbackSpoolFileMetadata(
      "q_0123456789abcdef0123456789abcdeg_000000000000002a_"
      "0000000000003039_a_i.m5fb",
      parsed));
  assert(!parseFeedbackSpoolFileMetadata(
      "x_0123456789abcdef0123456789abcdef_000000000000002a_"
      "0000000000003039_a_i.m5fb",
      parsed));
  assert(!parseFeedbackSpoolFileMetadata(
      "q_0123456789abcdef0123456789abcdef_000000000000002a_"
      "8000000000000000_a_i.m5fb",
      parsed));
  assert(!parseFeedbackSpoolFileMetadata(
      "q_0123456789abcdef0123456789abcdef_000000000000002a_"
      "0000000000003039_x_i.m5fb",
      parsed));
  assert(!parseFeedbackSpoolFileMetadata(
      "q_0123456789abcdef0123456789abcdef_000000000000002a_"
      "0000000000003039_a_x.m5fb",
      parsed));

  char unterminated[96];
  std::memset(unterminated, 'x', sizeof(unterminated));
  assert(!parseFeedbackSpoolFileMetadata(unterminated, parsed));

  return 0;
}
