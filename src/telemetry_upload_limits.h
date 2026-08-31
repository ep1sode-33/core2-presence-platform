#pragma once

#include <cstddef>

#include "device_config.h"

inline constexpr size_t kTelemetryUploadMaximumRecords =
    kDeviceTelemetryBatchCapacity;
inline constexpr size_t kTelemetryUploadMaximumPayloadBytes = 14 * 1024;

static_assert(kTelemetryUploadMaximumRecords ==
              kPresenceConfigContractMaxUploadBatchSize);
