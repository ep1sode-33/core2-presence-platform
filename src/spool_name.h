#pragma once

#include <cstddef>
#include <cstdint>

struct SpoolFileMetadata {
  char path[96] = {};
  char batchId[96] = {};
  char bootId[33] = {};
  uint64_t firstSeq = 0;
  uint64_t maxSeq = 0;
  uint16_t recordCount = 0;
};

bool buildSpoolFileMetadata(const char* bootId, uint64_t firstSeq,
                            uint64_t maxSeq, uint16_t recordCount,
                            SpoolFileMetadata& output);
bool parseSpoolFileMetadata(const char* path, SpoolFileMetadata& output);
