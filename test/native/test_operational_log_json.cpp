#include <cassert>
#include <string>

#include "operational_log_json.h"

namespace {

bool append(void* context, const char* data, size_t size) {
  static_cast<std::string*>(context)->append(data, size);
  return true;
}

}  // namespace

int main() {
  OperationalLogBatchContext context = {};
  context.batchId = "log-abc-0-2";
  context.bootId = "0123456789abcdef0123456789abcdef";
  context.buildId = "git:abc123+dirty";

  OperationalLogEvent events[2] = {};
  events[0].sequence = 1;
  events[0].uptimeMs = 100;
  events[0].level = OperationalLogLevel::kWarning;
  events[0].code = OperationalLogCode::kWifiChanged;
  events[0].value0 = -61;
  events[0].value1 = 2;
  events[1].sequence = 2;
  events[1].uptimeMs = 120;
  events[1].level = OperationalLogLevel::kSensorDetail;
  events[1].code = OperationalLogCode::kSensorChanged;
  events[1].value0 = 24;
  events[1].value1 = 30;

  std::string json;
  const OperationalLogJsonSink sink{&json, append};
  assert(writeOperationalLogBatchJson(context, events, 2, sink));
  assert(json ==
         "{\"schema_version\":1,\"batch_id\":\"log-abc-0-2\","
         "\"boot_id\":\"0123456789abcdef0123456789abcdef\","
         "\"build_id\":\"git:abc123+dirty\",\"records\":[{"
         "\"sequence\":1,\"uptime_ms\":100,\"level\":\"warning\","
         "\"event_type\":\"wifi_changed\",\"fields\":{\"value0\":-61,"
         "\"value1\":2}},{\"sequence\":2,\"uptime_ms\":120,"
         "\"level\":\"debug\",\"event_type\":\"sensor_changed\","
         "\"fields\":{\"value0\":24,\"value1\":30}}]}" );

  events[1].sequence = 1;
  assert(!writeOperationalLogBatchJson(context, events, 2, sink));
  events[1].sequence = 2;
  context.buildId = "bad build";
  assert(!writeOperationalLogBatchJson(context, events, 2, sink));
  assert(!writeOperationalLogBatchJson(context, nullptr, 0, sink));
  return 0;
}
