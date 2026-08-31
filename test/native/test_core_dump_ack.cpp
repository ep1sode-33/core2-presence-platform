#include <cassert>

#include "core_dump_ack.h"

int main() {
  constexpr char kCrash[] =
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  constexpr char kOk[] =
      "{\"crash_id\":\"0123456789abcdef0123456789abcdef0123456789abcdef"
      "0123456789abcdef\",\"duplicate\":false,\"durable\":true,"
      "\"server_utc_ms\":123,\"symbolication_status\":\"missing_elf\"}";
  const CoreDumpAckResult parsed =
      parseCoreDumpAck(kOk, sizeof(kOk) - 1, kCrash);
  assert(parsed.ok());
  assert(parsed.durable);
  assert(!parsed.duplicate);

  constexpr char kNotDurable[] =
      "{\"crash_id\":\"0123456789abcdef0123456789abcdef0123456789abcdef"
      "0123456789abcdef\",\"duplicate\":false,\"durable\":false,"
      "\"server_utc_ms\":123,\"symbolication_status\":\"missing_elf\"}";
  assert(parseCoreDumpAck(kNotDurable, sizeof(kNotDurable) - 1, kCrash)
             .error == CoreDumpAckError::kInvalidValue);
}
