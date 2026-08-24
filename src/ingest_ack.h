#pragma once

#include <cstddef>
#include <cstdint>

enum class IngestAckParseError : uint8_t {
  kNone,
  kTopLevelNotObject,
  kMalformedJson,
  kMissingField,
  kDuplicateField,
  kUnknownField,
  kWrongType,
  kNegativeInteger,
  kIntegerOverflow,
  kIntegerOutOfRange,
  kStringEscapeNotAllowed,
  kTrailingData,
  kBatchIdMismatch,
  kCountOverflow,
  kCountMismatch,
  kMaxSeqMismatch,
};

struct IngestAck {
  IngestAck()
      : stored(0),
        duplicates(0),
        maxSeq(0),
        serverUtcMs(0),
        desiredConfigRevision(0) {}

  uint64_t stored;
  uint64_t duplicates;
  uint64_t maxSeq;
  uint64_t serverUtcMs;
  uint64_t desiredConfigRevision;
};

struct IngestAckParseResult {
  IngestAckParseResult()
      : ack(), error(IngestAckParseError::kNone) {}
  IngestAckParseResult(const IngestAck& parsedAck,
                       IngestAckParseError parseError)
      : ack(parsedAck), error(parseError) {}

  IngestAck ack;
  IngestAckParseError error;

  bool ok() const { return error == IngestAckParseError::kNone; }
  explicit operator bool() const { return ok(); }
};

// Parses and validates the complete HTTP ingest response body. Unknown fields
// are rejected to match the backend's additionalProperties=false contract.
// JSON escapes are deliberately unsupported in batch_id and object keys: the
// wire protocol uses plain ASCII field names and batch identifiers.
IngestAckParseResult parseIngestAck(const char* responseBody,
                                    size_t responseBodyLength,
                                    const char* expectedBatchId,
                                    size_t expectedBatchIdLength,
                                    uint64_t expectedRecordCount,
                                    uint64_t expectedMaxSeq);

const char* ingestAckParseErrorName(IngestAckParseError error);
