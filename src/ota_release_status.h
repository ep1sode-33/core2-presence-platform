#pragma once

#include <cstdint>

// The release-status id is an idempotency key for the complete semantic
// payload, excluding status_id itself. It deliberately has no boot identifier:
// retrying the same status after a reboot must produce the same id.
struct OtaReleaseStatusIdentity {
  const char* deviceId = nullptr;
  const char* desiredReleaseId = nullptr;
  const char* runningReleaseId = nullptr;
  const char* previousReleaseId = nullptr;
  const char* lastKnownGoodReleaseId = nullptr;
  const char* phase = nullptr;
  int progressPercent = -1;
  const char* lastError = nullptr;
  const char* rollbackOutcome = nullptr;
  const char* firmwareVersion = nullptr;
  const char* buildId = nullptr;
};

uint64_t otaReleaseStatusIdentityHash(
    const OtaReleaseStatusIdentity& identity);
