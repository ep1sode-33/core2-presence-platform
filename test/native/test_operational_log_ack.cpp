#include <cassert>

#include "operational_log_ack.h"

int main() {
  constexpr char kBatch[] = "log-boot000000000001-10-11";
  constexpr char kOk[] =
      "{\"batch_id\":\"log-boot000000000001-10-11\",\"stored\":1,"
      "\"duplicates\":1,\"server_utc_ms\":123,\"retained_records\":9}";
  const auto parsed = parseOperationalLogAck(kOk, sizeof(kOk) - 1, kBatch, 2);
  assert(parsed.ok());
  assert(parsed.ack.stored == 1);
  assert(parsed.ack.duplicates == 1);

  constexpr char kWrongCount[] =
      "{\"batch_id\":\"log-boot000000000001-10-11\",\"stored\":1,"
      "\"duplicates\":0,\"server_utc_ms\":123,\"retained_records\":9}";
  assert(parseOperationalLogAck(kWrongCount, sizeof(kWrongCount) - 1, kBatch,
                                2)
             .error == OperationalLogAckError::kMismatch);

  constexpr char kUnknown[] =
      "{\"batch_id\":\"log-boot000000000001-10-11\",\"stored\":1,"
      "\"duplicates\":1,\"server_utc_ms\":123,\"retained_records\":9,"
      "\"extra\":0}";
  assert(parseOperationalLogAck(kUnknown, sizeof(kUnknown) - 1, kBatch, 2)
             .error == OperationalLogAckError::kUnknownField);
}
