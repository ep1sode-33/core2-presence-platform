#include <cassert>
#include <cstdint>
#include <string_view>

#include "ingest_ack.h"

namespace {

constexpr std::string_view kBatchId = "batch-001";

IngestAckParseResult parse(std::string_view body, uint64_t expectedCount = 3,
                           uint64_t expectedMaxSeq = 12) {
  return parseIngestAck(body.data(), body.size(), kBatchId.data(),
                        kBatchId.size(), expectedCount, expectedMaxSeq);
}

void expectError(std::string_view body, IngestAckParseError expected,
                 uint64_t expectedCount = 3, uint64_t expectedMaxSeq = 12) {
  const IngestAckParseResult result =
      parse(body, expectedCount, expectedMaxSeq);
  assert(!result.ok());
  assert(result.error == expected);
}

}  // namespace

int main() {
  const IngestAckParseResult success = parse(
      R"({"batch_id":"batch-001","stored":2,"duplicates":1,"max_seq":12,"server_utc_ms":1800000000123,"desired_config_revision":7})");
  assert(success.ok());
  assert(success.ack.stored == 2);
  assert(success.ack.duplicates == 1);
  assert(success.ack.maxSeq == 12);
  assert(success.ack.serverUtcMs == 1800000000123ULL);
  assert(success.ack.desiredConfigRevision == 7);

  const IngestAckParseResult allDuplicates = parse(
      " \n{ \"desired_config_revision\" : 8, \"server_utc_ms\" : 9, "
      "\"max_seq\" : 12, \"duplicates\" : 3, \"stored\" : 0, "
      "\"batch_id\" : \"batch-001\" }\r\n");
  assert(allDuplicates.ok());
  assert(allDuplicates.ack.stored == 0);
  assert(allDuplicates.ack.duplicates == 3);

  expectError(
      R"({"batch_id":"batch-002","stored":2,"duplicates":1,"max_seq":12,"server_utc_ms":9,"desired_config_revision":0})",
      IngestAckParseError::kBatchIdMismatch);
  expectError(
      R"({"batch_id":"batch-001","stored":1,"duplicates":1,"max_seq":12,"server_utc_ms":9,"desired_config_revision":0})",
      IngestAckParseError::kCountMismatch);
  expectError(
      R"({"batch_id":"batch-001","stored":2,"duplicates":1,"max_seq":13,"server_utc_ms":9,"desired_config_revision":0})",
      IngestAckParseError::kMaxSeqMismatch);

  expectError(
      R"({"batch_id":"batch-001","stored":18446744073709551616,"duplicates":0,"max_seq":12,"server_utc_ms":9,"desired_config_revision":0})",
      IngestAckParseError::kIntegerOverflow);
  expectError(
      R"({"batch_id":"batch-001","stored":2,"duplicates":1,"max_seq":12,"server_utc_ms":9223372036854775808,"desired_config_revision":0})",
      IngestAckParseError::kIntegerOutOfRange);
  expectError(
      R"({"batch_id":"batch-001","stored":18446744073709551615,"duplicates":1,"max_seq":12,"server_utc_ms":9,"desired_config_revision":0})",
      IngestAckParseError::kCountOverflow,
      UINT64_MAX);
  expectError(
      R"({"batch_id":"batch-001","stored":2,"duplicates":-1,"max_seq":12,"server_utc_ms":9,"desired_config_revision":0})",
      IngestAckParseError::kNegativeInteger);
  expectError(
      R"({"batch_id":"batch-001","stored":"2","duplicates":1,"max_seq":12,"server_utc_ms":9,"desired_config_revision":0})",
      IngestAckParseError::kWrongType);
  expectError(
      R"({"batch_id":"batch-001","stored":2.0,"duplicates":1,"max_seq":12,"server_utc_ms":9,"desired_config_revision":0})",
      IngestAckParseError::kWrongType);

  expectError(
      R"({"batch_id":"batch-001","stored":2,"stored":2,"duplicates":1,"max_seq":12,"server_utc_ms":9,"desired_config_revision":0})",
      IngestAckParseError::kDuplicateField);
  expectError(
      R"({"batch_id":"batch-001","stored":2,"duplicates":1,"max_seq":12,"server_utc_ms":9,"desired_config_revision":0}garbage)",
      IngestAckParseError::kTrailingData);
  expectError(
      R"({"batch_id":"batch-001","stored":2,"duplicates":1,"max_seq":12,"server_utc_ms":9})",
      IngestAckParseError::kMissingField);
  expectError(
      R"({"batch_id":"batch\u002d001","stored":2,"duplicates":1,"max_seq":12,"server_utc_ms":9,"desired_config_revision":0})",
      IngestAckParseError::kStringEscapeNotAllowed);
  expectError(
      R"({"batch_id":"batch-001","stored":2,"duplicates":1,"max_seq":12,"server_utc_ms":9,"desired_config_revision":0,"future":true})",
      IngestAckParseError::kUnknownField);

  return 0;
}
