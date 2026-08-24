#pragma once

#include <cstddef>
#include <cstdint>

struct RuntimeIdentity {
  static constexpr size_t kDeviceIdSize = 19;
  static constexpr size_t kBootIdSize = 33;

  char deviceId[kDeviceIdSize] = {};
  char bootId[kBootIdSize] = {};
  bool deviceIdValid = false;
};

uint64_t monotonicMillis();
RuntimeIdentity createRuntimeIdentity();
