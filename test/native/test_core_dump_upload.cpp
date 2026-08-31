#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core_dump_json.h"
#include "core_dump_upload.h"

namespace {

struct MemoryDump {
  std::vector<uint8_t> bytes;
  CoreDumpProbeStatus status = CoreDumpProbeStatus::kPresent;
  size_t descriptorSize = 0;
  bool failRead = false;
  bool failErase = false;
  size_t readCalls = 0;
  size_t eraseCalls = 0;
};

CoreDumpProbeStatus probe(void* context,
                          CoreDumpImageDescriptor* descriptor) {
  auto& dump = *static_cast<MemoryDump*>(context);
  if (descriptor == nullptr) {
    return CoreDumpProbeStatus::kIoError;
  }
  descriptor->storageAddress = 0x1000;
  descriptor->size = dump.descriptorSize == 0 ? dump.bytes.size()
                                                : dump.descriptorSize;
  return dump.status;
}

bool read(void* context, const CoreDumpImageDescriptor* descriptor,
          size_t offset, uint8_t* destination, size_t size) {
  auto& dump = *static_cast<MemoryDump*>(context);
  ++dump.readCalls;
  if (dump.failRead || descriptor == nullptr ||
      (destination == nullptr && size != 0) || offset > dump.bytes.size() ||
      size > dump.bytes.size() - offset) {
    return false;
  }
  if (size != 0) {
    std::memcpy(destination, dump.bytes.data() + offset, size);
  }
  return true;
}

bool erase(void* context, const CoreDumpImageDescriptor*) {
  auto& dump = *static_cast<MemoryDump*>(context);
  ++dump.eraseCalls;
  return !dump.failErase;
}

CoreDumpStorageBackend backend(MemoryDump& dump) {
  return {&dump, probe, read, erase};
}

CoreDumpReportContext context() {
  return {"core2-001122334455", "0123456789abcdef", "git:abc+dirty",
          "panic"};
}

std::string readAll(CoreDumpJsonReader& reader, size_t requestSize) {
  std::string output;
  std::vector<uint8_t> buffer(requestSize);
  while (reader.available() != 0) {
    const size_t size = reader.read(buffer.data(), buffer.size());
    if (size == 0) {
      break;
    }
    output.append(reinterpret_cast<const char*>(buffer.data()), size);
  }
  return output;
}

bool appendSink(void* context, const uint8_t* data, size_t size) {
  auto& output = *static_cast<std::string*>(context);
  output.append(reinterpret_cast<const char*>(data), size);
  return true;
}

bool failingSink(void*, const uint8_t*, size_t) { return false; }

void testExactJsonAndDeterministicMetadata() {
  MemoryDump dump{{0x00, 0x01, 0x02, 0xfa, 0xff}};
  PendingCoreDump pending;
  assert(pending.prepare(backend(dump), context()) ==
         CoreDumpPrepareResult::kReady);
  assert(pending.ready());
  assert(dump.eraseCalls == 0);

  const CoreDumpReportMetadata& metadata = pending.metadata();
  assert(std::strcmp(
             metadata.dumpSha256Hex,
             "ad6170c304e1b0a6cfe3b1ba6b40c065f646428f623f664ad31d54b98735195c") ==
         0);
  assert(std::strcmp(
             metadata.crashId,
             "f39fabb132680629cd837051e74ea4ad3227845f2923bec92d6540c0e5b47379") ==
         0);
  assert(metadata.dumpSize == 5);

  const std::string expected =
      "{\"schema_version\":1,\"crash_id\":"
      "\"f39fabb132680629cd837051e74ea4ad3227845f2923bec92d6540c0e5b47379\","
      "\"boot_id\":\"0123456789abcdef\",\"build_id\":\"git:abc+dirty\","
      "\"reset_reason\":\"panic\",\"dump_size\":5,\"dump_sha256\":"
      "\"ad6170c304e1b0a6cfe3b1ba6b40c065f646428f623f664ad31d54b98735195c\","
      "\"dump_base64\":\"AAEC+v8=\"}";

  CoreDumpJsonReader reader;
  assert(reader.begin(pending));
  assert(reader.contentLength() == expected.size());
  assert(readAll(reader, 7) == expected);
  assert(reader.complete());
  assert(dump.eraseCalls == 0);

  std::string pushed;
  assert(writeCoreDumpJson(pending, {&pushed, appendSink}));
  assert(pushed == expected);
  assert(dump.eraseCalls == 0);
}

void testBase64PaddingAcrossTinyReads() {
  const std::vector<std::pair<std::vector<uint8_t>, std::string>> cases = {
      {{0xff}, "/w=="}, {{0xff, 0xee}, "/+4="}, {{0xff, 0xee, 0xdd}, "/+7d"}};
  for (const auto& entry : cases) {
    MemoryDump dump{entry.first};
    PendingCoreDump pending;
    assert(pending.prepare(backend(dump), context()) ==
           CoreDumpPrepareResult::kReady);
    CoreDumpJsonReader reader;
    assert(reader.begin(pending));
    const std::string json = readAll(reader, 1);
    assert(json.find("\"dump_base64\":\"" + entry.second + "\"}") !=
           std::string::npos);
    assert(reader.complete());
    assert(dump.eraseCalls == 0);
  }

  // The implementation reads 192 raw bytes at a time. Verify that the final
  // one-byte group after an exact chunk boundary is padded, not merged or
  // dropped.
  MemoryDump boundary;
  boundary.bytes.resize(193, 0);
  PendingCoreDump pending;
  assert(pending.prepare(backend(boundary), context()) ==
         CoreDumpPrepareResult::kReady);
  CoreDumpJsonReader reader;
  assert(reader.begin(pending));
  const std::string json = readAll(reader, 113);
  const std::string marker = "\"dump_base64\":\"";
  const size_t start = json.find(marker);
  assert(start != std::string::npos);
  const size_t dataStart = start + marker.size();
  const size_t dataEnd = json.find('"', dataStart);
  assert(dataEnd != std::string::npos);
  assert(json.substr(dataStart, dataEnd - dataStart) ==
         std::string(258, 'A') + "==");
  assert(reader.complete());
  assert(boundary.eraseCalls == 0);
}

void testMaximumSizeAndReadBoundaries() {
  MemoryDump dump;
  dump.bytes.resize(kCoreDumpMaximumSize);
  for (size_t index = 0; index < dump.bytes.size(); ++index) {
    dump.bytes[index] = static_cast<uint8_t>(index);
  }
  PendingCoreDump pending;
  assert(pending.prepare(backend(dump), context()) ==
         CoreDumpPrepareResult::kReady);
  assert(pending.metadata().dumpSize == kCoreDumpMaximumSize);
  assert(dump.readCalls ==
         kCoreDumpMaximumSize / kCoreDumpMaximumReadChunk);

  uint8_t last = 0;
  assert(pending.readChunk(kCoreDumpMaximumSize - 1, &last, 1));
  assert(last == 0xff);
  assert(pending.readChunk(kCoreDumpMaximumSize, nullptr, 0));
  assert(!pending.readChunk(kCoreDumpMaximumSize, &last, 1));
  std::vector<uint8_t> tooLarge(kCoreDumpMaximumReadChunk + 1);
  assert(!pending.readChunk(0, tooLarge.data(), tooLarge.size()));

  CoreDumpJsonReader reader;
  assert(reader.begin(pending));
  assert(reader.contentLength() < 90'000);
  const std::string json = readAll(reader, 1'461);
  assert(json.size() == reader.contentLength());
  assert(reader.complete());
  assert(json.rfind("\"}") == json.size() - 2);
  assert(dump.eraseCalls == 0);

  MemoryDump oversized;
  oversized.bytes = {1};
  oversized.descriptorSize = kCoreDumpMaximumSize + 1;
  PendingCoreDump rejected;
  assert(rejected.prepare(backend(oversized), context()) ==
         CoreDumpPrepareResult::kDumpTooLarge);
  assert(oversized.readCalls == 0);
  assert(oversized.eraseCalls == 0);

  MemoryDump empty;
  PendingCoreDump emptyPending;
  assert(emptyPending.prepare(backend(empty), context()) ==
         CoreDumpPrepareResult::kCorruptDump);
}

void testReadAndSinkFailuresNeverErase() {
  MemoryDump prepareFailure{{1, 2, 3}};
  prepareFailure.failRead = true;
  PendingCoreDump notReady;
  assert(notReady.prepare(backend(prepareFailure), context()) ==
         CoreDumpPrepareResult::kReadFailed);
  assert(!notReady.ready());
  assert(prepareFailure.eraseCalls == 0);

  MemoryDump streamFailure{{1, 2, 3, 4, 5, 6}};
  PendingCoreDump pending;
  assert(pending.prepare(backend(streamFailure), context()) ==
         CoreDumpPrepareResult::kReady);
  streamFailure.failRead = true;
  CoreDumpJsonReader reader;
  assert(reader.begin(pending));
  const std::string partial = readAll(reader, 64);
  assert(!partial.empty());
  assert(reader.failed());
  assert(!reader.complete());
  assert(streamFailure.eraseCalls == 0);

  streamFailure.failRead = false;
  assert(!writeCoreDumpJson(pending, {nullptr, failingSink}));
  assert(streamFailure.eraseCalls == 0);
}

void testOnlyDurableMatchingAckErases() {
  MemoryDump dump{{9, 8, 7, 6}};
  PendingCoreDump pending;
  assert(pending.prepare(backend(dump), context()) ==
         CoreDumpPrepareResult::kReady);
  const std::string crashId = pending.metadata().crashId;

  assert(pending.acknowledgeDurable(crashId.c_str(), false) ==
         CoreDumpAcknowledgeResult::kNotDurable);
  assert(pending.acknowledgeDurable("different-crash", true) ==
         CoreDumpAcknowledgeResult::kCrashIdMismatch);
  assert(dump.eraseCalls == 0);
  assert(pending.ready());

  dump.failErase = true;
  assert(pending.acknowledgeDurable(crashId.c_str(), true) ==
         CoreDumpAcknowledgeResult::kEraseFailed);
  assert(dump.eraseCalls == 1);
  assert(pending.ready());

  dump.failErase = false;
  assert(pending.acknowledgeDurable(crashId.c_str(), true) ==
         CoreDumpAcknowledgeResult::kErased);
  assert(dump.eraseCalls == 2);
  assert(!pending.ready());
  assert(pending.acknowledgeDurable(crashId.c_str(), true) ==
         CoreDumpAcknowledgeResult::kNotPrepared);
  assert(dump.eraseCalls == 2);

  MemoryDump cleared{{1}};
  PendingCoreDump abandoned;
  assert(abandoned.prepare(backend(cleared), context()) ==
         CoreDumpPrepareResult::kReady);
  abandoned.clear();
  assert(cleared.eraseCalls == 0);
}

void testProbeAndContextFailures() {
  MemoryDump noDump{{1}};
  noDump.status = CoreDumpProbeStatus::kNotFound;
  PendingCoreDump pending;
  assert(pending.prepare(backend(noDump), context()) ==
         CoreDumpPrepareResult::kNoDump);

  noDump.status = CoreDumpProbeStatus::kCorrupt;
  assert(pending.prepare(backend(noDump), context()) ==
         CoreDumpPrepareResult::kCorruptDump);
  noDump.status = CoreDumpProbeStatus::kIoError;
  assert(pending.prepare(backend(noDump), context()) ==
         CoreDumpPrepareResult::kIoError);
  assert(noDump.eraseCalls == 0);

  CoreDumpReportContext invalid = context();
  invalid.resetReason = "bad reason";
  assert(pending.prepare(backend(noDump), invalid) ==
         CoreDumpPrepareResult::kInvalidContext);
  char unterminatedBootId[CoreDumpReportMetadata::kBootIdCapacity] = {};
  std::memset(unterminatedBootId, 'a', sizeof(unterminatedBootId));
  invalid = context();
  invalid.bootId = unterminatedBootId;
  assert(pending.prepare(backend(noDump), invalid) ==
         CoreDumpPrepareResult::kInvalidContext);
  assert(noDump.readCalls == 0);
  assert(noDump.eraseCalls == 0);

  assert(coreDumpPrepareResultName(CoreDumpPrepareResult::kReady) ==
         std::string("ready"));
  assert(coreDumpAcknowledgeResultName(CoreDumpAcknowledgeResult::kErased) ==
         std::string("erased"));
}

}  // namespace

int main() {
  testExactJsonAndDeterministicMetadata();
  testBase64PaddingAcrossTinyReads();
  testMaximumSizeAndReadBoundaries();
  testReadAndSinkFailuresNeverErase();
  testOnlyDurableMatchingAckErases();
  testProbeAndContextFailures();
  return 0;
}
