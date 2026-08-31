#include <cassert>
#include <cstring>
#include <string>

#include "control_protocol.h"

namespace {

const char kRelease[] =
    "{\"release_id\":\"rel-0123456789abcdef0123456789abcdef\","
    "\"hardware_model\":\"m5go-classic-esp32-16m\","
    "\"firmware_version\":\"0.7.0\",\"release_counter\":7,"
    "\"build_id\":\"abc123\",\"image_size\":1200000,"
    "\"image_sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
    "\"elf_sha256\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\","
    "\"key_id\":\"release-2026-a\","
    "\"signature_format\":\"ecdsa-p256-sha256-raw\","
    "\"imported_at_ms\":1700000000000,\"imported_by\":\"test\","
    "\"verified\":true,"
    "\"manifest_url\":\"/v1/devices/core2-aabbccddeeff/releases/rel-0123456789abcdef0123456789abcdef/manifest\","
    "\"image_url\":\"/v1/devices/core2-aabbccddeeff/releases/rel-0123456789abcdef0123456789abcdef/image\"}";

std::string response(const std::string& release, const std::string& command) {
  return "{\"server_utc_ms\":1700000000100,\"poll_after_ms\":5000,"
         "\"desired_release\":" +
         release + ",\"command\":" + command + "}";
}

}  // namespace

int main() {
  std::string json = response(kRelease, "null");
  ControlPollParseResult parsed =
      parseControlPollResponse(json.data(), json.size());
  assert(parsed.ok());
  assert(parsed.value.pollAfterMs == 5000);
  assert(parsed.value.hasDesiredRelease);
  assert(parsed.value.desiredRelease.releaseCounter == 7);
  assert(parsed.value.desiredRelease.imageSize == 1200000);
  assert(std::strcmp(parsed.value.desiredRelease.hardwareModel,
                     "m5go-classic-esp32-16m") == 0);
  assert(!parsed.value.hasCommand);

  const std::string command =
      "{\"command_id\":\"cmd-0123456789abcdef0123456789abcdef\","
      "\"created_at_ms\":1700000000000,\"expires_at_ms\":1700000600000,"
      "\"lease_id\":\"lease-0123456789abcdef0123456789abcdef\","
      "\"lease_expires_at_ms\":1700000015000,\"delivery_attempt\":1,"
      "\"command\":{\"action\":\"set_log_level\","
      "\"level\":\"debug_sensor\",\"duration_seconds\":600}}";
  json = response("null", command);
  parsed = parseControlPollResponse(json.data(), json.size());
  assert(parsed.ok());
  assert(!parsed.value.hasDesiredRelease);
  assert(parsed.value.hasCommand);
  assert(parsed.value.command.action == RemoteCommandAction::kSetLogLevel);
  assert(parsed.value.command.detailedLog);
  assert(parsed.value.command.durationSeconds == 600);
  assert(remoteCommandEnvelopeIsValid(parsed.value.command));

  const std::string otaCommand =
      "{\"command_id\":\"cmd-1123456789abcdef0123456789abcdef\","
      "\"created_at_ms\":1700000000000,\"expires_at_ms\":1700000600000,"
      "\"lease_id\":\"lease-1123456789abcdef0123456789abcdef\","
      "\"lease_expires_at_ms\":1700000015000,\"delivery_attempt\":2,"
      "\"command\":{\"requires_local_confirmation\":true,"
      "\"action\":\"open_dev_ota\"}}";
  json = response("null", otaCommand);
  parsed = parseControlPollResponse(json.data(), json.size());
  assert(parsed.ok());
  assert(parsed.value.command.action == RemoteCommandAction::kOpenDevOta);
  assert(parsed.value.command.requiresLocalConfirmation);

  json = response(kRelease, "null");
  const size_t url = json.find("/v1/devices/");
  json.replace(url, 1, "h");
  parsed = parseControlPollResponse(json.data(), json.size());
  assert(parsed.error == ControlPollParseError::kInvalidValue);

  json = response("null", "null");
  json.replace(json.find("5000"), 4, "6000");
  assert(parseControlPollResponse(json.data(), json.size()).error ==
         ControlPollParseError::kInvalidValue);

  json = response("null", command);
  json.insert(json.size() - 1, ",\"unexpected\":1");
  assert(parseControlPollResponse(json.data(), json.size()).error ==
         ControlPollParseError::kUnknownField);

  assert(parseControlPollResponse(nullptr, 0).error ==
         ControlPollParseError::kNullArgument);
  return 0;
}
