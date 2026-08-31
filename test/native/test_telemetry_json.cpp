#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string>

#include "telemetry_json.h"

namespace {

bool appendString(void* context, const char* data, size_t size) {
  static_cast<std::string*>(context)->append(data, size);
  return true;
}

TelemetryBatchContext context() {
  TelemetryBatchContext result;
  result.batchId = "b-bootbootbootboot-0-1";
  result.bootId = "bootbootbootboot";
  result.firmwareVersion = "0.3.0";
  result.buildId = "abc123";
  result.appliedConfigRevision = 2;
  result.hasClockAnchor = true;
  result.anchorUtcMs = 1700000000000ULL;
  result.anchorUptimeMs = 5000;
  return result;
}

}  // namespace

int main() {
  TelemetryRecord records[2];
  records[0].kind = TelemetryKind::kTransition;
  records[0].seq = 0;
  records[0].uptimeMs = 10;
  records[0].appliedConfigRevision = 2;
  records[0].transition.hasFromState = false;
  records[0].transition.toState = PresenceState::kCalibrating;
  records[0].transition.reason = TransitionReason::kBoot;
  records[0].transition.pir = false;
  records[0].transition.pirAgeMs = 10;
  records[0].transition.soundActive = false;
  records[0].transition.soundAgeMs = 10;
  records[0].transition.micEnvelope = 2.0f;
  records[0].transition.noiseFloor = 3.0f;
  records[0].transition.soundThreshold = 4.0f;
  records[0].transition.brightnessBefore = 255;
  records[0].transition.brightnessAfter = 255;

  records[1].kind = TelemetryKind::kSample;
  records[1].seq = 1;
  records[1].uptimeMs = 1000;
  records[1].appliedConfigRevision = 2;
  records[1].sample.pir = true;
  records[1].sample.micRms = 12.5f;
  records[1].sample.micEnvelope = 10.25f;
  records[1].sample.micMin = -20;
  records[1].sample.micMax = 30;
  records[1].sample.noiseFloor = 8.0f;
  records[1].sample.soundThreshold = 9.5f;
  records[1].sample.soundActive = true;
  records[1].sample.state = PresenceState::kPresent;
  records[1].sample.brightness = 255;

  std::string output;
  TelemetryJsonSink sink{&output, appendString};
  TelemetryBatchContext validContext = context();
  assert(writeTelemetryBatchJson(validContext, records, 2, sink));
  assert(output ==
         "{\"schema_version\":1,\"batch_id\":\"b-bootbootbootboot-0-1\","
         "\"boot_id\":\"bootbootbootboot\",\"firmware_version\":\"0.3.0\","
         "\"build_id\":\"abc123\","
         "\"applied_config_revision\":2,\"clock_anchor\":{\"utc_ms\":"
         "1700000000000,\"uptime_ms\":5000,\"source\":\"sntp\"},"
         "\"records\":[{\"seq\":0,\"kind\":\"transition\","
         "\"uptime_ms\":10,\"from_state\":null,\"to_state\":"
         "\"calibrating\",\"reason\":\"boot\",\"pir\":false,"
         "\"pir_age_ms\":10,\"sound_active\":false,\"sound_age_ms\":10,"
         "\"mic_envelope\":2.000,\"noise_floor\":3.000,"
         "\"sound_threshold\":4.000,\"brightness_before\":255,"
         "\"brightness_after\":255},{\"seq\":1,\"kind\":"
         "\"sample\",\"uptime_ms\":1000,\"pir\":true,\"mic_rms\":12.500,"
         "\"mic_envelope\":10.250,\"mic_min\":-20,\"mic_max\":30,"
         "\"noise_floor\":8.000,\"sound_threshold\":9.500,"
         "\"sound_active\":true,\"state\":\"present\","
         "\"brightness\":255}]}");
  if (std::getenv("EMIT_TELEMETRY_JSON") != nullptr) {
    std::cout << output;
  }

  output.clear();
  validContext.hasClockAnchor = false;
  assert(writeTelemetryBatchJson(validContext, records, 2, sink));
  assert(output.find("\"clock_anchor\":null") != std::string::npos);

  output.clear();
  validContext.buildId = nullptr;
  assert(!writeTelemetryBatchJson(validContext, records, 2, sink));
  assert(output.empty());
  validContext.buildId = "abc123";

  output.clear();
  records[1].appliedConfigRevision = 3;
  assert(!writeTelemetryBatchJson(validContext, records, 2, sink));
  assert(output.empty());

  records[1].appliedConfigRevision = 2;
  records[1].seq = records[0].seq;
  assert(!writeTelemetryBatchJson(validContext, records, 2, sink));
  assert(output.empty());

  records[1].seq = 1;
  records[1].sample.micRms = -1.0f;
  assert(!writeTelemetryBatchJson(validContext, records, 2, sink));
  assert(output.empty());

  records[1].sample.micRms = 1.0f;
  records[1].sample.micMin = 31;
  assert(!writeTelemetryBatchJson(validContext, records, 2, sink));
  assert(output.empty());

  records[1].sample.micMin = -32768;
  records[1].sample.micMax = 32767;
  records[1].sample.micRms = 999999936.0f;
  records[1].sample.micEnvelope = 999999936.0f;
  records[1].sample.noiseFloor = 999999936.0f;
  records[1].sample.soundThreshold = 999999936.0f;
  assert(writeTelemetryBatchJson(validContext, records, 2, sink));
  return 0;
}
