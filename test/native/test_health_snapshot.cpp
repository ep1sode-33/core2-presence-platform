#include <cassert>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "health_json.h"
#include "health_snapshot.h"

namespace {

bool append(void* context, const char* data, size_t size) {
  static_cast<std::string*>(context)->append(data, size);
  return true;
}

DeviceHealthSnapshot healthySnapshot() {
  DeviceHealthSnapshot snapshot = {};
  std::strcpy(snapshot.deviceId, "core2-010203040506");
  std::strcpy(snapshot.bootId, "0123456789abcdef0123456789abcdef");
  std::strcpy(snapshot.firmwareVersion, "0.7.0-dev");
  std::strcpy(snapshot.buildId, "abc123");
  std::strcpy(snapshot.resetReason, "power_on");
  std::strcpy(snapshot.localIp, "192.168.0.12");
  snapshot.initialized = true;
  snapshot.filesystemReady = true;
  snapshot.wifiConnected = true;
  snapshot.clockSynchronized = true;
  snapshot.pirStatus = SensorHealthStatus::kHealthy;
  snapshot.microphoneStatus = SensorHealthStatus::kHealthy;
  snapshot.uploaderStatus = UploaderHealthStatus::kReady;
  snapshot.telemetryQueueCapacity = 120;
  snapshot.feedbackQueueCapacity = 16;
  snapshot.mainHeartbeatAgeMs = 0;
  snapshot.uploaderHeartbeatAgeMs = 0;
  snapshot.level = evaluateDeviceHealth(snapshot);
  return snapshot;
}

}  // namespace

int main() {
  DeviceHealthSnapshot snapshot = {};
  assert(evaluateDeviceHealth(snapshot) == DeviceHealthLevel::kUnknown);

  snapshot = healthySnapshot();
  assert(evaluateDeviceHealth(snapshot) == DeviceHealthLevel::kHealthy);
  snapshot.wifiConnected = false;
  assert(evaluateDeviceHealth(snapshot) == DeviceHealthLevel::kDegraded);
  snapshot.wifiConnected = true;
  snapshot.telemetryDroppedCritical = 1;
  assert(evaluateDeviceHealth(snapshot) ==
         DeviceHealthLevel::kActionRequired);
  snapshot.telemetryDroppedCritical = 0;
  snapshot.lastTelemetryAckAgeMs = 120000;
  assert(evaluateDeviceHealth(snapshot) == DeviceHealthLevel::kDegraded);
  snapshot.lastTelemetryAckAgeMs = UINT64_MAX;
  snapshot.spoolFiles = 1;
  snapshot.oldestBacklogAgeMs = 1000;
  assert(evaluateDeviceHealth(snapshot) == DeviceHealthLevel::kHealthy);
  snapshot.oldestBacklogAgeMs = 120000;
  assert(evaluateDeviceHealth(snapshot) == DeviceHealthLevel::kDegraded);

  DeviceHealthMailbox mailbox;
  snapshot = healthySnapshot();
  assert(mailbox.publish(snapshot));
  assert(mailbox.snapshot().version == 1);

  DeviceHealthSnapshot concurrent = snapshot;
  std::thread writer([&]() {
    for (uint64_t sequence = 1; sequence <= 2000; ++sequence) {
      concurrent.sequence = sequence;
      concurrent.uptimeMs = sequence * 10;
      assert(mailbox.publish(concurrent));
    }
  });
  for (size_t index = 0; index < 2000; ++index) {
    const DeviceHealthSnapshot copy = mailbox.snapshot();
    assert(copy.uptimeMs == copy.sequence * 10 || copy.sequence == 0);
  }
  writer.join();

  DeviceHealthMainUpdate mainUpdate = {};
  std::strcpy(mainUpdate.deviceId, "core2-010203040506");
  std::strcpy(mainUpdate.bootId, "0123456789abcdef0123456789abcdef");
  std::strcpy(mainUpdate.firmwareVersion, "0.7.0-dev");
  std::strcpy(mainUpdate.buildId, "abc123");
  std::strcpy(mainUpdate.resetReason, "power_on");
  mainUpdate.uptimeMs = 30000;
  mainUpdate.mainHeartbeatAgeMs = 0;
  mainUpdate.initialized = true;
  assert(mailbox.publishMain(mainUpdate));
  DeviceHealthWorkerUpdate workerUpdate = {};
  workerUpdate.uptimeMs = 31000;
  workerUpdate.uploaderHeartbeatAgeMs = 0;
  mailbox.publishWorker(workerUpdate);
  assert(mailbox.snapshot().mainHeartbeatAgeMs == 1000);
  mainUpdate.uptimeMs = 32000;
  assert(mailbox.publishMain(mainUpdate));
  assert(mailbox.snapshot().uploaderHeartbeatAgeMs == 1000);
  assert(mailbox.publishRuntimeActivity(true, "downloading", true,
                                        "debug_sensor"));
  assert(mailbox.snapshot().otaActive);
  assert(std::strcmp(mailbox.snapshot().otaState, "downloading") == 0);
  assert(!mailbox.publishRuntimeActivity(true, "", false, "inactive"));
  assert(mailbox.publishRuntimeActivity(false, "failed", false, "inactive"));
  assert(mailbox.snapshot().level == DeviceHealthLevel::kActionRequired);

  snapshot = healthySnapshot();
  snapshot.sequence = 7;
  snapshot.uptimeMs = 1234;
  snapshot.lastTelemetryAckAgeMs = UINT64_MAX;
  snapshot.lastConfigAttemptAgeMs = 10;
  snapshot.telemetryAckResult = HealthOperationResult::kRetrying;
  snapshot.appliedConfigRevision = 11;
  snapshot.storedConfigRevision = 12;
  snapshot.desiredConfigRevision = 13;
  snapshot.mainStackHighWaterBytes = 1001;
  snapshot.uploaderStackHighWaterBytes = 1002;
  snapshot.telemetryQueueDepth = 3;
  snapshot.feedbackQueueDepth = 4;
  snapshot.spoolFiles = 5;
  snapshot.feedbackWaitFiles = 6;
  snapshot.feedbackReadyFiles = 7;
  snapshot.deadFiles = 8;
  snapshot.oldestBacklogAgeMs = 9000;
  snapshot.littlefsTotalBytes = 10000;
  snapshot.littlefsUsedBytes = 2500;
  snapshot.freeHeapBytes = 3000;
  snapshot.minimumFreeHeapBytes = 2000;
  snapshot.largestFreeBlockBytes = 1000;
  std::string json;
  const HealthJsonSink sink{&json, append};
  assert(writeDeviceHealthJson(snapshot, sink));
  assert(json.find("\"schema_version\":1") != std::string::npos);
  assert(json.find("\"last_telemetry_ack_ms\":null") !=
         std::string::npos);
  assert(json.find("\"last_config_attempt_ms\":10") !=
         std::string::npos);
  assert(json.find("\"telemetry_ack_result\":\"retrying\"") !=
         std::string::npos);
  assert(json.find("\"applied_revision\":11") != std::string::npos);
  assert(json.find("\"stored_revision\":12") != std::string::npos);
  assert(json.find("\"desired_revision\":13") != std::string::npos);
  assert(json.find("\"telemetry_depth\":3") != std::string::npos);
  assert(json.find("\"feedback_depth\":4") != std::string::npos);
  assert(json.find("\"spool_files\":5") != std::string::npos);
  assert(json.find("\"littlefs_free_bytes\":7500") != std::string::npos);
  assert(json.find("\"free_heap_bytes\":3000") != std::string::npos);
  assert(json.find("password") == std::string::npos);
  assert(json.find("token") == std::string::npos);
  if (std::getenv("EMIT_HEALTH_JSON") != nullptr) {
    std::cout << json;
  }
  return 0;
}
