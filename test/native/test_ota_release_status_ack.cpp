#include <cassert>

#include "ota_release_status_ack.h"

int main() {
  constexpr char kStatusId[] = "status-0123456789abcdef";
  constexpr char kValid[] =
      "{\"status_id\":\"status-0123456789abcdef\",\"duplicate\":false,"
      "\"server_utc_ms\":123,\"desired_release_completed\":true}";
  const OtaReleaseStatusAckResult valid =
      parseOtaReleaseStatusAck(kValid, sizeof(kValid) - 1, kStatusId);
  assert(valid.ok());
  assert(!valid.ack.duplicate);
  assert(valid.ack.serverUtcMs == 123);
  assert(valid.ack.desiredReleaseCompleted);

  constexpr char kMismatch[] =
      "{\"status_id\":\"status-fedcba9876543210\",\"duplicate\":false,"
      "\"server_utc_ms\":123,\"desired_release_completed\":false}";
  assert(parseOtaReleaseStatusAck(kMismatch, sizeof(kMismatch) - 1,
                                 kStatusId)
             .error == OtaReleaseStatusAckError::kMismatch);

  constexpr char kUnknown[] =
      "{\"status_id\":\"status-0123456789abcdef\",\"duplicate\":false,"
      "\"server_utc_ms\":123,\"desired_release_completed\":false,"
      "\"extra\":0}";
  assert(parseOtaReleaseStatusAck(kUnknown, sizeof(kUnknown) - 1, kStatusId)
             .error == OtaReleaseStatusAckError::kUnknownField);

  constexpr char kDuplicate[] =
      "{\"status_id\":\"status-0123456789abcdef\",\"duplicate\":false,"
      "\"duplicate\":true,\"server_utc_ms\":123,"
      "\"desired_release_completed\":false}";
  assert(parseOtaReleaseStatusAck(kDuplicate, sizeof(kDuplicate) - 1,
                                 kStatusId)
             .error == OtaReleaseStatusAckError::kDuplicateField);

  constexpr char kTrailing[] =
      "{\"status_id\":\"status-0123456789abcdef\",\"duplicate\":false,"
      "\"server_utc_ms\":123,\"desired_release_completed\":false}x";
  assert(parseOtaReleaseStatusAck(kTrailing, sizeof(kTrailing) - 1,
                                 kStatusId)
             .error == OtaReleaseStatusAckError::kTrailingData);
}
