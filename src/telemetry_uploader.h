#pragma once

#include <cstdint>

#include "device_config.h"
#include "device_config_mailbox.h"
#include "runtime_identity.h"
#include "telemetry.h"
#include "touch_feedback_queue.h"

struct TelemetryUploaderSettings {
  bool configured = false;
  char wifiSsid[33] = {};
  char wifiPassword[64] = {};
  char serverBaseUrl[129] = {};
  char apiToken[257] = {};
  PresenceConfig initialConfig = defaultPresenceConfig();
  uint64_t startAfterUptimeMs = 0;
};

// Starts one low-priority Core 0 worker. The settings and identity are copied;
// both queues and the config mailbox must remain alive for the firmware's
// lifetime.
// Invalid device identity is a hard failure: no envelope is written or sent.
bool startTelemetryUploader(const RuntimeIdentity& identity,
                            const TelemetryUploaderSettings& settings,
                            TelemetryQueue& queue,
                            DeviceConfigMailbox& configMailbox,
                            TouchFeedbackQueue& touchFeedbackQueue);
