#pragma once

#include <cstdint>

#include "feedback_protocol.h"

struct FeedbackSpoolFileMetadata {
  // `path` is the telemetry-wait stage. `readyPath` is the same immutable
  // bundle after its exact one-record telemetry batch has been acknowledged.
  char path[96] = {};
  char readyPath[96] = {};
  char telemetryBatchId[96] = {};
  char feedbackId[52] = {};
  char bootId[33] = {};
  uint64_t seq = 0;
  uint64_t linkedSampleUptimeMs = 0;
  ActualPresence actualPresence = ActualPresence::kAbsent;
  PresenceState observedState = PresenceState::kIdle;
};

// Canonical immutable bundle paths:
//   /feedback/wait/q_<boot32>_<seq16>_<uptime16>_<actual1>_<state1>.m5fb
//   /feedback/ready/q_<boot32>_<seq16>_<uptime16>_<actual1>_<state1>.m5fb
// The filename retains every non-constant request/ACK expectation, allowing a
// rebooted uploader to validate the exact response without parsing or mutating
// the immutable request body. telemetryBatchId is the deterministic one-record
// batch ID b:<boot>:<seq>:<seq>:001.
bool buildFeedbackSpoolFileMetadata(const FeedbackRecord& record,
                                    FeedbackSpoolFileMetadata& output);

// Accepts a canonical basename in any bounded wait/ready directory and rebuilds
// both canonical paths plus both request identities. Inputs without a NUL
// within 95 bytes are rejected.
bool parseFeedbackSpoolFileMetadata(const char* path,
                                    FeedbackSpoolFileMetadata& output);

// Reconstructs the exact request/ACK expectation after reboot. Mutated
// identity or enum metadata is rejected and output is left unchanged.
bool feedbackRecordFromSpoolFileMetadata(
    const FeedbackSpoolFileMetadata& metadata, FeedbackRecord& output);
