#pragma once

#include <cstdint>

#include "dashboard_mailbox.h"
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
  bool environmentEnabled = false;
  char environmentUrl[193] = {};
  uint32_t environmentPollIntervalMs = 30 * 1000;
  bool weatherEnabled = false;
  char weatherUrl[513] = {};
  uint32_t weatherPollIntervalMs = 15 * 60 * 1000;
  PresenceConfig initialConfig = defaultPresenceConfig();
  uint64_t startAfterUptimeMs = 0;
};

// Starts one low-priority Core 0 worker. The settings and identity are copied;
// both queues and both mailboxes must remain alive for the firmware's lifetime.
// Invalid device identity is a hard failure: no envelope is written or sent.
bool startTelemetryUploader(const RuntimeIdentity& identity,
                            const TelemetryUploaderSettings& settings,
                            TelemetryQueue& queue,
                            DeviceConfigMailbox& configMailbox,
                            TouchFeedbackQueue& touchFeedbackQueue,
                            DashboardMailbox& dashboardMailbox);
