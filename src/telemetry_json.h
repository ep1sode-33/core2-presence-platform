#pragma once

#include <cstddef>
#include <cstdint>

#include "telemetry.h"

enum class ClockAnchorSource : uint8_t {
  kSntp,
  kRtc,
};

struct TelemetryBatchContext {
  const char* batchId = nullptr;
  const char* bootId = nullptr;
  const char* firmwareVersion = nullptr;
  uint64_t appliedConfigRevision = 0;
  bool hasClockAnchor = false;
  uint64_t anchorUtcMs = 0;
  uint64_t anchorUptimeMs = 0;
  ClockAnchorSource anchorSource = ClockAnchorSource::kSntp;
};

using TelemetryJsonWrite = bool (*)(void* context, const char* data,
                                    size_t size);

struct TelemetryJsonSink {
  void* context = nullptr;
  TelemetryJsonWrite write = nullptr;
};

// Validates the complete envelope before writing. A false return therefore
// never leaves a syntactically valid but semantically partial batch behind.
bool writeTelemetryBatchJson(const TelemetryBatchContext& context,
                             const TelemetryRecord* records,
                             size_t recordCount,
                             const TelemetryJsonSink& sink);
