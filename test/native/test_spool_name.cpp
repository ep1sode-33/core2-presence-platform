#include <cassert>
#include <cstring>

#include "spool_name.h"

int main() {
  constexpr const char* kBootId = "0123456789abcdef0123456789abcdef";
  SpoolFileMetadata metadata;
  assert(buildSpoolFileMetadata(kBootId, 1, 42, 30, metadata));
  assert(std::strcmp(
             metadata.path,
             "/spool/q_0123456789abcdef0123456789abcdef_0000000000000001_"
             "000000000000002a_01e.json") == 0);
  assert(std::strcmp(
             metadata.batchId,
             "b:0123456789abcdef0123456789abcdef:0000000000000001:"
             "000000000000002a:01e") == 0);

  SpoolFileMetadata parsed;
  assert(parseSpoolFileMetadata(metadata.path, parsed));
  assert(parsed.firstSeq == 1);
  assert(parsed.maxSeq == 42);
  assert(parsed.recordCount == 30);
  assert(std::strcmp(parsed.bootId, kBootId) == 0);
  assert(std::strcmp(parsed.batchId, metadata.batchId) == 0);

  assert(parseSpoolFileMetadata(metadata.path + 7, parsed));
  assert(!buildSpoolFileMetadata("bad", 1, 2, 1, metadata));
  assert(!buildSpoolFileMetadata(kBootId, 3, 2, 1, metadata));
  assert(!buildSpoolFileMetadata(kBootId, 1, 2, 0, metadata));
  assert(!buildSpoolFileMetadata(kBootId, 1, 2, 257, metadata));
  assert(!parseSpoolFileMetadata(
      "q_0123456789abcdef0123456789abcdef_0000000000000001_"
      "0000000000000002_000.json",
      parsed));
  assert(!parseSpoolFileMetadata(
      "q_0123456789abcdef0123456789abcdef_0000000000000001_"
      "0000000000000002_001.json.extra",
      parsed));
  assert(!parseSpoolFileMetadata(
      "q_0123456789abcdef0123456789abcdef_000000000000000g_"
      "0000000000000002_001.json",
      parsed));
  return 0;
}
