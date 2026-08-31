#pragma once

#include <cstddef>

#include "health_snapshot.h"

using HealthJsonWrite = bool (*)(void* context, const char* data, size_t size);

struct HealthJsonSink {
  void* context = nullptr;
  HealthJsonWrite write = nullptr;
};

bool writeDeviceHealthJson(const DeviceHealthSnapshot& snapshot,
                           const HealthJsonSink& sink);
