#pragma once

#include <cstddef>

#include "operational_log.h"

struct OperationalLogBatchContext {
  const char* batchId = nullptr;
  const char* bootId = nullptr;
  const char* buildId = nullptr;
};

using OperationalLogJsonWrite = bool (*)(void* context, const char* data,
                                         size_t size);

struct OperationalLogJsonSink {
  void* context = nullptr;
  OperationalLogJsonWrite write = nullptr;
};

bool writeOperationalLogBatchJson(const OperationalLogBatchContext& context,
                                  const OperationalLogEvent* events,
                                  size_t eventCount,
                                  const OperationalLogJsonSink& sink);
