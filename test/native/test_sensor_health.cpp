#include <cassert>

#include "sensor_health.h"

int main() {
  MicrophoneWindowHealthInput input = {};
  assert(classifyMicrophoneWindow(input) == SensorHealthStatus::kFault);

  input.driverStarted = true;
  input.sampleCount = 128;
  input.repeatedSampleCount = 8;
  input.rawMinimum = 1900;
  input.rawMaximum = 2200;
  input.expectedUs = 16000;
  input.elapsedUs = 16100;
  assert(classifyMicrophoneWindow(input) == SensorHealthStatus::kHealthy);

  input.railSampleCount = 40;
  assert(classifyMicrophoneWindow(input) == SensorHealthStatus::kFault);
  input.railSampleCount = 0;
  input.elapsedUs = 21000;
  assert(classifyMicrophoneWindow(input) == SensorHealthStatus::kDegraded);
  input.elapsedUs = 11000;
  assert(classifyMicrophoneWindow(input) == SensorHealthStatus::kDegraded);

  SensorHealthLatch latch(3, 2);
  assert(latch.observe(SensorHealthStatus::kHealthy) ==
         SensorHealthStatus::kHealthy);
  assert(latch.observe(SensorHealthStatus::kFault) ==
         SensorHealthStatus::kDegraded);
  assert(latch.observe(SensorHealthStatus::kFault) ==
         SensorHealthStatus::kDegraded);
  assert(latch.observe(SensorHealthStatus::kFault) ==
         SensorHealthStatus::kFault);
  assert(latch.observe(SensorHealthStatus::kHealthy) ==
         SensorHealthStatus::kFault);
  assert(latch.observe(SensorHealthStatus::kHealthy) ==
         SensorHealthStatus::kHealthy);

  PirHealthTracker pir;
  assert(pir.observe(false, 1000) == SensorHealthStatus::kHealthy);
  assert(pir.observe(true, 2000) == SensorHealthStatus::kHealthy);
  assert(pir.observe(true, 30ULL * 60ULL * 1000ULL + 2000) ==
         SensorHealthStatus::kFault);
  assert(pir.observe(false, 30ULL * 60ULL * 1000ULL + 2001) ==
         SensorHealthStatus::kHealthy);
  return 0;
}
