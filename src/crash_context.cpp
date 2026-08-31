#include "crash_context.h"

#include <cstring>

#if defined(ARDUINO_ARCH_ESP32)
#include <Preferences.h>
#endif

namespace {

constexpr uint8_t kMagic[] = {'M', '5', 'C', 'C'};
constexpr uint16_t kFormatVersion = 1;
constexpr uint16_t kPayloadLength = 108;
constexpr size_t kGenerationOffset = 8;
constexpr size_t kBootLengthOffset = 16;
constexpr size_t kBuildLengthOffset = 17;
constexpr size_t kReservedOffset = 18;
constexpr size_t kBootOffset = 20;
constexpr size_t kBuildOffset = 52;
constexpr size_t kChecksumOffset = 116;

constexpr uint8_t kAttributionMagic[] = {'M', '5', 'C', 'A'};
constexpr uint16_t kAttributionFormatVersion = 1;
constexpr uint16_t kAttributionPayloadLength = 164;
constexpr size_t kAttributionBootLengthOffset = 8;
constexpr size_t kAttributionBuildLengthOffset = 9;
constexpr size_t kAttributionReasonLengthOffset = 10;
constexpr size_t kAttributionReservedOffset = 11;
constexpr size_t kAttributionBootOffset = 12;
constexpr size_t kAttributionBuildOffset = 44;
constexpr size_t kAttributionReasonOffset = 108;
constexpr size_t kAttributionChecksumOffset = 172;

size_t boundedLength(const char* value, size_t capacity) {
  if (value == nullptr) {
    return capacity;
  }
  const void* end = std::memchr(value, '\0', capacity);
  return end == nullptr ? capacity
                        : static_cast<const char*>(end) - value;
}

bool validBootId(const char* value) {
  if (boundedLength(value, kCrashContextBootIdCapacity) !=
      kCrashContextBootIdLength) {
    return false;
  }
  for (size_t index = 0; index < kCrashContextBootIdLength; ++index) {
    if (!((value[index] >= '0' && value[index] <= '9') ||
          (value[index] >= 'a' && value[index] <= 'f'))) {
      return false;
    }
  }
  return true;
}

bool validBuildId(const char* value) {
  const size_t length =
      boundedLength(value, kCrashContextBuildIdCapacity);
  if (length == 0 || length > kCrashContextBuildIdMaximumLength) {
    return false;
  }
  for (size_t index = 0; index < length; ++index) {
    const char byte = value[index];
    if ((byte >= 'A' && byte <= 'Z') ||
        (byte >= 'a' && byte <= 'z') ||
        (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' ||
        byte == '+' || byte == '-') {
      continue;
    }
    return false;
  }
  return true;
}

bool validResetReason(const char* value) {
  const size_t length = boundedLength(value, kCrashResetReasonCapacity);
  if (length == 0 || length > kCrashResetReasonMaximumLength) {
    return false;
  }
  for (size_t index = 0; index < length; ++index) {
    const char byte = value[index];
    if ((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
        byte == '_') {
      continue;
    }
    return false;
  }
  return true;
}

void putU16(uint8_t* output, uint16_t value) {
  output[0] = static_cast<uint8_t>(value);
  output[1] = static_cast<uint8_t>(value >> 8U);
}

void putU32(uint8_t* output, uint32_t value) {
  for (size_t index = 0; index < sizeof(value); ++index) {
    output[index] = static_cast<uint8_t>(value >> (index * 8U));
  }
}

void putU64(uint8_t* output, uint64_t value) {
  for (size_t index = 0; index < sizeof(value); ++index) {
    output[index] = static_cast<uint8_t>(value >> (index * 8U));
  }
}

uint16_t getU16(const uint8_t* input) {
  return static_cast<uint16_t>(input[0]) |
         (static_cast<uint16_t>(input[1]) << 8U);
}

uint32_t getU32(const uint8_t* input) {
  uint32_t value = 0;
  for (size_t index = 0; index < sizeof(value); ++index) {
    value |= static_cast<uint32_t>(input[index]) << (index * 8U);
  }
  return value;
}

uint64_t getU64(const uint8_t* input) {
  uint64_t value = 0;
  for (size_t index = 0; index < sizeof(value); ++index) {
    value |= static_cast<uint64_t>(input[index]) << (index * 8U);
  }
  return value;
}

uint32_t crc32(const uint8_t* bytes, size_t length) {
  uint32_t crc = 0xffffffffU;
  for (size_t index = 0; index < length; ++index) {
    crc ^= bytes[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      const uint32_t mask = static_cast<uint32_t>(0U - (crc & 1U));
      crc = (crc >> 1U) ^ (0xedb88320U & mask);
    }
  }
  return crc ^ 0xffffffffU;
}

bool zeroFilled(const uint8_t* bytes, size_t begin, size_t end) {
  for (size_t index = begin; index < end; ++index) {
    if (bytes[index] != 0) {
      return false;
    }
  }
  return true;
}

struct SlotSnapshot {
  CrashContextSlotReadStatus readStatus =
      CrashContextSlotReadStatus::kMissing;
  CrashContextBlobError decodeError = CrashContextBlobError::kNone;
  CrashBootContext context = {};

  bool valid() const {
    return readStatus == CrashContextSlotReadStatus::kOk &&
           decodeError == CrashContextBlobError::kNone;
  }
  bool invalid() const {
    return readStatus == CrashContextSlotReadStatus::kOk &&
           decodeError != CrashContextBlobError::kNone;
  }
};

SlotSnapshot readSlot(const CrashContextStorageOps& storage, uint8_t slot) {
  SlotSnapshot snapshot;
  uint8_t blob[kCrashContextBlobSize] = {};
  size_t length = 0;
  snapshot.readStatus = storage.readSlot(
      storage.context, slot, blob, sizeof(blob), &length);
  if (snapshot.readStatus == CrashContextSlotReadStatus::kOk) {
    snapshot.decodeError =
        decodeCrashBootContext(blob, length, &snapshot.context);
  }
  return snapshot;
}

bool writeAndVerify(const CrashContextStorageOps& storage, uint8_t slot,
                    const uint8_t blob[kCrashContextBlobSize],
                    const CrashBootContext& expected,
                    CrashContextStorageResult* failure) {
  if (!storage.writeSlot(storage.context, slot, blob,
                         kCrashContextBlobSize)) {
    *failure = CrashContextStorageResult::kWriteFailed;
    return false;
  }
  const SlotSnapshot verified = readSlot(storage, slot);
  if (!verified.valid() ||
      !crashBootContextsEqual(verified.context, expected)) {
    *failure = CrashContextStorageResult::kVerifyFailed;
    return false;
  }
  return true;
}

}  // namespace

bool crashBootContextIsValid(const CrashBootContext& context) {
  return context.generation > 0 &&
         context.generation <= kCrashContextMaximumGeneration &&
         validBootId(context.bootId) && validBuildId(context.buildId);
}

bool crashBootContextsEqual(const CrashBootContext& left,
                            const CrashBootContext& right) {
  return left.generation == right.generation &&
         boundedLength(left.bootId, sizeof(left.bootId)) <
             sizeof(left.bootId) &&
         boundedLength(right.bootId, sizeof(right.bootId)) <
             sizeof(right.bootId) &&
         boundedLength(left.buildId, sizeof(left.buildId)) <
             sizeof(left.buildId) &&
         boundedLength(right.buildId, sizeof(right.buildId)) <
             sizeof(right.buildId) &&
         std::strcmp(left.bootId, right.bootId) == 0 &&
         std::strcmp(left.buildId, right.buildId) == 0;
}

CrashContextBlobError encodeCrashBootContext(
    const CrashBootContext& context, uint8_t* output,
    size_t outputCapacity) {
  if (output == nullptr) {
    return CrashContextBlobError::kNullArgument;
  }
  if (outputCapacity < kCrashContextBlobSize) {
    return CrashContextBlobError::kWrongLength;
  }
  if (!crashBootContextIsValid(context)) {
    return CrashContextBlobError::kInvalidRecord;
  }

  uint8_t blob[kCrashContextBlobSize] = {};
  std::memcpy(blob, kMagic, sizeof(kMagic));
  putU16(blob + 4, kFormatVersion);
  putU16(blob + 6, kPayloadLength);
  putU64(blob + kGenerationOffset, context.generation);
  blob[kBootLengthOffset] = kCrashContextBootIdLength;
  const size_t buildLength = std::strlen(context.buildId);
  blob[kBuildLengthOffset] = static_cast<uint8_t>(buildLength);
  std::memcpy(blob + kBootOffset, context.bootId,
              kCrashContextBootIdLength);
  std::memcpy(blob + kBuildOffset, context.buildId, buildLength);
  putU32(blob + kChecksumOffset, crc32(blob, kChecksumOffset));
  std::memcpy(output, blob, sizeof(blob));
  return CrashContextBlobError::kNone;
}

CrashContextBlobError decodeCrashBootContext(
    const uint8_t* input, size_t inputLength, CrashBootContext* output) {
  if (input == nullptr || output == nullptr) {
    return CrashContextBlobError::kNullArgument;
  }
  if (inputLength != kCrashContextBlobSize) {
    return CrashContextBlobError::kWrongLength;
  }
  if (std::memcmp(input, kMagic, sizeof(kMagic)) != 0) {
    return CrashContextBlobError::kBadMagic;
  }
  if (getU16(input + 4) != kFormatVersion) {
    return CrashContextBlobError::kUnsupportedVersion;
  }
  if (getU16(input + 6) != kPayloadLength) {
    return CrashContextBlobError::kBadPayloadLength;
  }
  if (getU32(input + kChecksumOffset) != crc32(input, kChecksumOffset)) {
    return CrashContextBlobError::kChecksumMismatch;
  }
  const size_t bootLength = input[kBootLengthOffset];
  const size_t buildLength = input[kBuildLengthOffset];
  if (getU16(input + kReservedOffset) != 0 ||
      bootLength != kCrashContextBootIdLength || buildLength == 0 ||
      buildLength > kCrashContextBuildIdMaximumLength ||
      !zeroFilled(input, kBuildOffset + buildLength, kChecksumOffset)) {
    return CrashContextBlobError::kInvalidRecord;
  }

  CrashBootContext candidate = {};
  candidate.generation = getU64(input + kGenerationOffset);
  std::memcpy(candidate.bootId, input + kBootOffset, bootLength);
  candidate.bootId[bootLength] = '\0';
  std::memcpy(candidate.buildId, input + kBuildOffset, buildLength);
  candidate.buildId[buildLength] = '\0';
  if (!crashBootContextIsValid(candidate)) {
    return CrashContextBlobError::kInvalidRecord;
  }
  *output = candidate;
  return CrashContextBlobError::kNone;
}

bool crashDumpAttributionIsValid(
    const CrashDumpAttribution& attribution) {
  return validBootId(attribution.bootId) &&
         validBuildId(attribution.buildId) &&
         validResetReason(attribution.resetReason);
}

bool crashDumpAttributionsEqual(const CrashDumpAttribution& left,
                                const CrashDumpAttribution& right) {
  return crashDumpAttributionIsValid(left) &&
         crashDumpAttributionIsValid(right) &&
         std::strcmp(left.bootId, right.bootId) == 0 &&
         std::strcmp(left.buildId, right.buildId) == 0 &&
         std::strcmp(left.resetReason, right.resetReason) == 0;
}

CrashContextBlobError encodeCrashDumpAttribution(
    const CrashDumpAttribution& attribution, uint8_t* output,
    size_t outputCapacity) {
  if (output == nullptr) {
    return CrashContextBlobError::kNullArgument;
  }
  if (outputCapacity < kCrashDumpAttributionBlobSize) {
    return CrashContextBlobError::kWrongLength;
  }
  if (!crashDumpAttributionIsValid(attribution)) {
    return CrashContextBlobError::kInvalidRecord;
  }
  uint8_t blob[kCrashDumpAttributionBlobSize] = {};
  std::memcpy(blob, kAttributionMagic, sizeof(kAttributionMagic));
  putU16(blob + 4, kAttributionFormatVersion);
  putU16(blob + 6, kAttributionPayloadLength);
  const size_t buildLength = std::strlen(attribution.buildId);
  const size_t reasonLength = std::strlen(attribution.resetReason);
  blob[kAttributionBootLengthOffset] = kCrashContextBootIdLength;
  blob[kAttributionBuildLengthOffset] =
      static_cast<uint8_t>(buildLength);
  blob[kAttributionReasonLengthOffset] =
      static_cast<uint8_t>(reasonLength);
  std::memcpy(blob + kAttributionBootOffset, attribution.bootId,
              kCrashContextBootIdLength);
  std::memcpy(blob + kAttributionBuildOffset, attribution.buildId,
              buildLength);
  std::memcpy(blob + kAttributionReasonOffset, attribution.resetReason,
              reasonLength);
  putU32(blob + kAttributionChecksumOffset,
         crc32(blob, kAttributionChecksumOffset));
  std::memcpy(output, blob, sizeof(blob));
  return CrashContextBlobError::kNone;
}

CrashContextBlobError decodeCrashDumpAttribution(
    const uint8_t* input, size_t inputLength,
    CrashDumpAttribution* output) {
  if (input == nullptr || output == nullptr) {
    return CrashContextBlobError::kNullArgument;
  }
  if (inputLength != kCrashDumpAttributionBlobSize) {
    return CrashContextBlobError::kWrongLength;
  }
  if (std::memcmp(input, kAttributionMagic,
                  sizeof(kAttributionMagic)) != 0) {
    return CrashContextBlobError::kBadMagic;
  }
  if (getU16(input + 4) != kAttributionFormatVersion) {
    return CrashContextBlobError::kUnsupportedVersion;
  }
  if (getU16(input + 6) != kAttributionPayloadLength) {
    return CrashContextBlobError::kBadPayloadLength;
  }
  if (getU32(input + kAttributionChecksumOffset) !=
      crc32(input, kAttributionChecksumOffset)) {
    return CrashContextBlobError::kChecksumMismatch;
  }
  const size_t bootLength = input[kAttributionBootLengthOffset];
  const size_t buildLength = input[kAttributionBuildLengthOffset];
  const size_t reasonLength = input[kAttributionReasonLengthOffset];
  if (input[kAttributionReservedOffset] != 0 ||
      bootLength != kCrashContextBootIdLength || buildLength == 0 ||
      buildLength > kCrashContextBuildIdMaximumLength || reasonLength == 0 ||
      reasonLength > kCrashResetReasonMaximumLength ||
      !zeroFilled(input, kAttributionBuildOffset + buildLength,
                  kAttributionReasonOffset) ||
      !zeroFilled(input, kAttributionReasonOffset + reasonLength,
                  kAttributionChecksumOffset)) {
    return CrashContextBlobError::kInvalidRecord;
  }
  CrashDumpAttribution candidate = {};
  std::memcpy(candidate.bootId, input + kAttributionBootOffset, bootLength);
  candidate.bootId[bootLength] = '\0';
  std::memcpy(candidate.buildId, input + kAttributionBuildOffset,
              buildLength);
  candidate.buildId[buildLength] = '\0';
  std::memcpy(candidate.resetReason, input + kAttributionReasonOffset,
              reasonLength);
  candidate.resetReason[reasonLength] = '\0';
  if (!crashDumpAttributionIsValid(candidate)) {
    return CrashContextBlobError::kInvalidRecord;
  }
  *output = candidate;
  return CrashContextBlobError::kNone;
}

CrashContextRotationResult rotateCrashContextWithStorage(
    const char* currentBootId, const char* currentBuildId,
    const CrashContextStorageOps& storage) {
  CrashContextRotationResult result;
  if (storage.readSlot == nullptr || storage.writeSlot == nullptr) {
    result.storage = CrashContextStorageResult::kInvalidStorageOps;
    return result;
  }

  CrashBootContext current = {};
  current.generation = 1;
  if (currentBootId == nullptr || currentBuildId == nullptr ||
      boundedLength(currentBootId, sizeof(current.bootId)) >=
          sizeof(current.bootId) ||
      boundedLength(currentBuildId, sizeof(current.buildId)) >=
          sizeof(current.buildId)) {
    result.storage = CrashContextStorageResult::kInvalidCurrentContext;
    return result;
  }
  std::memcpy(current.bootId, currentBootId,
              std::strlen(currentBootId) + 1);
  std::memcpy(current.buildId, currentBuildId,
              std::strlen(currentBuildId) + 1);
  if (!crashBootContextIsValid(current)) {
    result.storage = CrashContextStorageResult::kInvalidCurrentContext;
    return result;
  }

  SlotSnapshot slots[2] = {readSlot(storage, 0), readSlot(storage, 1)};
  if (slots[0].readStatus == CrashContextSlotReadStatus::kReadFailed ||
      slots[1].readStatus == CrashContextSlotReadStatus::kReadFailed) {
    result.storage = CrashContextStorageResult::kReadFailed;
    result.previousStatus = PreviousCrashContextStatus::kReadFailed;
    return result;
  }

  const bool ambiguousEqual =
      slots[0].valid() && slots[1].valid() &&
      slots[0].context.generation == slots[1].context.generation &&
      !crashBootContextsEqual(slots[0].context, slots[1].context);
  const bool invalidStoredData =
      slots[0].invalid() || slots[1].invalid() || ambiguousEqual;

  uint64_t highestGeneration = 0;
  int newestSlot = -1;
  for (int slot = 0; slot < 2; ++slot) {
    if (slots[slot].valid() &&
        (newestSlot < 0 ||
         slots[slot].context.generation > highestGeneration)) {
      newestSlot = slot;
      highestGeneration = slots[slot].context.generation;
    }
  }

  if (invalidStoredData) {
    // A corrupt or contradictory slot may be newer than the readable one.
    // Do not guess which boot produced a pending dump. We can still repair the
    // journal by superseding every ambiguous slot with the current identity.
    result.previousStatus =
        PreviousCrashContextStatus::kInvalidStoredData;
  } else if (newestSlot >= 0) {
    result.previousStatus = PreviousCrashContextStatus::kAvailable;
    result.previous = slots[newestSlot].context;
  } else {
    result.previousStatus = PreviousCrashContextStatus::kNotStored;
  }

  if (highestGeneration >= kCrashContextMaximumGeneration) {
    result.storage = CrashContextStorageResult::kGenerationExhausted;
    result.previous = {};
    return result;
  }
  current.generation = highestGeneration + 1;
  uint8_t blob[kCrashContextBlobSize] = {};
  if (encodeCrashBootContext(current, blob, sizeof(blob)) !=
      CrashContextBlobError::kNone) {
    result.storage = CrashContextStorageResult::kInvalidCurrentContext;
    result.previous = {};
    return result;
  }

  uint8_t targetSlot = 0;
  if (invalidStoredData) {
    if (slots[0].invalid() || ambiguousEqual) {
      targetSlot = 0;
    } else if (slots[1].invalid()) {
      targetSlot = 1;
    } else {
      targetSlot = newestSlot == 0 ? 1 : 0;
    }
  } else if (newestSlot >= 0) {
    targetSlot = newestSlot == 0 ? 1 : 0;
  }

  CrashContextStorageResult failure = CrashContextStorageResult::kOk;
  if (!writeAndVerify(storage, targetSlot, blob, current, &failure)) {
    result.storage = failure;
    result.previous = {};
    return result;
  }

  // If both old slots were invalid or mutually contradictory, overwrite the
  // second one too. Equal duplicate records are an unambiguous repaired state.
  const uint8_t otherSlot = targetSlot == 0 ? 1 : 0;
  const bool otherNeedsRepair =
      slots[otherSlot].invalid() || ambiguousEqual;
  if (otherNeedsRepair &&
      !writeAndVerify(storage, otherSlot, blob, current, &failure)) {
    result.storage = failure;
    result.previous = {};
    return result;
  }

  result.storage = CrashContextStorageResult::kOk;
  result.persistedGeneration = current.generation;
  return result;
}

#if defined(ARDUINO_ARCH_ESP32)

namespace {

constexpr char kPreferencesNamespace[] = "m5crashctx";
constexpr char kSlotKeys[][6] = {"ctx_a", "ctx_b"};
constexpr char kAttributionKey[] = "dump_pin";

struct PreferencesStorage {
  Preferences* preferences;
};

CrashContextSlotReadStatus preferencesReadSlot(
    void* opaque, uint8_t slot, uint8_t* output, size_t outputCapacity,
    size_t* outputLength) {
  if (opaque == nullptr || slot > 1 || output == nullptr ||
      outputLength == nullptr) {
    return CrashContextSlotReadStatus::kReadFailed;
  }
  Preferences& preferences =
      *static_cast<PreferencesStorage*>(opaque)->preferences;
  const size_t length = preferences.getBytesLength(kSlotKeys[slot]);
  *outputLength = length;
  if (length == 0) {
    return CrashContextSlotReadStatus::kMissing;
  }
  if (length > outputCapacity) {
    // Report the real length so the canonical decoder rejects it without an
    // out-of-bounds read into output.
    return CrashContextSlotReadStatus::kOk;
  }
  return preferences.getBytes(kSlotKeys[slot], output, length) == length
             ? CrashContextSlotReadStatus::kOk
             : CrashContextSlotReadStatus::kReadFailed;
}

bool preferencesWriteSlot(void* opaque, uint8_t slot,
                          const uint8_t* input, size_t inputLength) {
  if (opaque == nullptr || slot > 1 || input == nullptr) {
    return false;
  }
  Preferences& preferences =
      *static_cast<PreferencesStorage*>(opaque)->preferences;
  return preferences.putBytes(kSlotKeys[slot], input, inputLength) ==
         inputLength;
}

}  // namespace

CrashContextRotationResult captureAndRotateCrashContext(
    const char* currentBootId, const char* currentBuildId) {
  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, false)) {
    CrashContextRotationResult result;
    result.storage = CrashContextStorageResult::kOpenFailed;
    return result;
  }
  PreferencesStorage context = {&preferences};
  const CrashContextStorageOps storage = {
      &context, preferencesReadSlot, preferencesWriteSlot};
  CrashContextRotationResult result = rotateCrashContextWithStorage(
      currentBootId, currentBuildId, storage);
  preferences.end();
  return result;
}

CrashDumpAttributionStorageResult loadPinnedCrashDumpAttribution(
    CrashDumpAttribution* output) {
  if (output == nullptr) {
    return CrashDumpAttributionStorageResult::kInvalidAttribution;
  }
  *output = {};
  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, true)) {
    return CrashDumpAttributionStorageResult::kNotStored;
  }
  const size_t length = preferences.getBytesLength(kAttributionKey);
  if (length == 0) {
    preferences.end();
    return CrashDumpAttributionStorageResult::kNotStored;
  }
  if (length != kCrashDumpAttributionBlobSize) {
    preferences.end();
    return CrashDumpAttributionStorageResult::kInvalidStoredData;
  }
  uint8_t blob[kCrashDumpAttributionBlobSize] = {};
  const size_t read =
      preferences.getBytes(kAttributionKey, blob, sizeof(blob));
  preferences.end();
  if (read != sizeof(blob)) {
    return CrashDumpAttributionStorageResult::kReadFailed;
  }
  CrashDumpAttribution decoded = {};
  if (decodeCrashDumpAttribution(blob, sizeof(blob), &decoded) !=
      CrashContextBlobError::kNone) {
    return CrashDumpAttributionStorageResult::kInvalidStoredData;
  }
  *output = decoded;
  return CrashDumpAttributionStorageResult::kOk;
}

CrashDumpAttributionStorageResult savePinnedCrashDumpAttribution(
    const CrashDumpAttribution& attribution) {
  uint8_t blob[kCrashDumpAttributionBlobSize] = {};
  if (encodeCrashDumpAttribution(attribution, blob, sizeof(blob)) !=
      CrashContextBlobError::kNone) {
    return CrashDumpAttributionStorageResult::kInvalidAttribution;
  }
  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, false)) {
    return CrashDumpAttributionStorageResult::kOpenFailed;
  }
  const size_t written =
      preferences.putBytes(kAttributionKey, blob, sizeof(blob));
  if (written != sizeof(blob)) {
    preferences.end();
    return CrashDumpAttributionStorageResult::kWriteFailed;
  }
  uint8_t verified[kCrashDumpAttributionBlobSize] = {};
  const size_t read =
      preferences.getBytes(kAttributionKey, verified, sizeof(verified));
  preferences.end();
  if (read != sizeof(verified) ||
      std::memcmp(blob, verified, sizeof(blob)) != 0) {
    return CrashDumpAttributionStorageResult::kVerifyFailed;
  }
  return CrashDumpAttributionStorageResult::kOk;
}

CrashDumpAttributionStorageResult clearPinnedCrashDumpAttribution() {
  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, false)) {
    return CrashDumpAttributionStorageResult::kOpenFailed;
  }
  if (preferences.getBytesLength(kAttributionKey) == 0) {
    preferences.end();
    return CrashDumpAttributionStorageResult::kOk;
  }
  const bool removed = preferences.remove(kAttributionKey);
  preferences.end();
  return removed ? CrashDumpAttributionStorageResult::kOk
                 : CrashDumpAttributionStorageResult::kRemoveFailed;
}

#else

CrashContextRotationResult captureAndRotateCrashContext(const char*,
                                                        const char*) {
  CrashContextRotationResult result;
  result.storage = CrashContextStorageResult::kUnsupportedPlatform;
  return result;
}

CrashDumpAttributionStorageResult loadPinnedCrashDumpAttribution(
    CrashDumpAttribution* output) {
  if (output != nullptr) {
    *output = {};
  }
  return CrashDumpAttributionStorageResult::kUnsupportedPlatform;
}

CrashDumpAttributionStorageResult savePinnedCrashDumpAttribution(
    const CrashDumpAttribution&) {
  return CrashDumpAttributionStorageResult::kUnsupportedPlatform;
}

CrashDumpAttributionStorageResult clearPinnedCrashDumpAttribution() {
  return CrashDumpAttributionStorageResult::kUnsupportedPlatform;
}

#endif
