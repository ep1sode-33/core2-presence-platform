#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>

#include "crash_context.h"

namespace {

constexpr char kBootA[] = "00112233445566778899aabbccddeeff";
constexpr char kBootB[] = "102132435465768798a9babbdcddedef";
constexpr char kBootC[] = "ffeeddccbbaa99887766554433221100";

CrashBootContext context(const char* bootId, const char* buildId,
                         uint64_t generation) {
  CrashBootContext value = {};
  std::strcpy(value.bootId, bootId);
  std::strcpy(value.buildId, buildId);
  value.generation = generation;
  return value;
}

struct FakeSlot {
  std::array<uint8_t, kCrashContextBlobSize> bytes = {};
  size_t length = 0;
  bool present = false;
  bool readFails = false;
};

struct FakeStorage {
  FakeSlot slots[2];
  int writeFailureSlot = -1;
  int corruptAfterWriteSlot = -1;
  unsigned writes = 0;
};

CrashContextSlotReadStatus fakeRead(void* opaque, uint8_t slot,
                                    uint8_t* output,
                                    size_t outputCapacity,
                                    size_t* outputLength) {
  auto& storage = *static_cast<FakeStorage*>(opaque);
  if (slot > 1 || output == nullptr || outputLength == nullptr ||
      storage.slots[slot].readFails) {
    return CrashContextSlotReadStatus::kReadFailed;
  }
  const FakeSlot& source = storage.slots[slot];
  if (!source.present) {
    *outputLength = 0;
    return CrashContextSlotReadStatus::kMissing;
  }
  *outputLength = source.length;
  if (source.length <= outputCapacity) {
    std::memcpy(output, source.bytes.data(), source.length);
  }
  return CrashContextSlotReadStatus::kOk;
}

bool fakeWrite(void* opaque, uint8_t slot, const uint8_t* input,
               size_t inputLength) {
  auto& storage = *static_cast<FakeStorage*>(opaque);
  ++storage.writes;
  if (slot > 1 || input == nullptr ||
      inputLength != kCrashContextBlobSize ||
      storage.writeFailureSlot == slot) {
    return false;
  }
  FakeSlot& target = storage.slots[slot];
  std::memcpy(target.bytes.data(), input, inputLength);
  target.length = inputLength;
  target.present = true;
  target.readFails = false;
  if (storage.corruptAfterWriteSlot == slot) {
    target.bytes[44] ^= 0x01;
  }
  return true;
}

CrashContextStorageOps ops(FakeStorage* storage) {
  return {storage, fakeRead, fakeWrite};
}

void store(FakeStorage* storage, uint8_t slot,
           const CrashBootContext& value) {
  FakeSlot& target = storage->slots[slot];
  assert(encodeCrashBootContext(value, target.bytes.data(),
                                target.bytes.size()) ==
         CrashContextBlobError::kNone);
  target.length = target.bytes.size();
  target.present = true;
}

CrashBootContext load(const FakeStorage& storage, uint8_t slot) {
  CrashBootContext value = {};
  assert(decodeCrashBootContext(storage.slots[slot].bytes.data(),
                                storage.slots[slot].length, &value) ==
         CrashContextBlobError::kNone);
  return value;
}

void testStableBlobRoundTrip() {
  const CrashBootContext original =
      context(kBootA, "git.0123456789ab+release", 0x0102030405060708ULL);
  std::array<uint8_t, kCrashContextBlobSize> blob = {};
  assert(encodeCrashBootContext(original, blob.data(), blob.size()) ==
         CrashContextBlobError::kNone);
  assert(blob[0] == 'M' && blob[1] == '5' && blob[2] == 'C' &&
         blob[3] == 'C');
  assert(blob[4] == 1 && blob[5] == 0);
  assert(blob[6] == 108 && blob[7] == 0);
  assert(blob[8] == 0x08 && blob[9] == 0x07 && blob[15] == 0x01);
  assert(blob[16] == 32);
  assert(blob[17] == std::strlen(original.buildId));

  CrashBootContext decoded = {};
  assert(decodeCrashBootContext(blob.data(), blob.size(), &decoded) ==
         CrashContextBlobError::kNone);
  assert(crashBootContextsEqual(decoded, original));

  std::array<uint8_t, kCrashContextBlobSize> again = {};
  assert(encodeCrashBootContext(decoded, again.data(), again.size()) ==
         CrashContextBlobError::kNone);
  assert(again == blob);
}

void testBlobRejectsCorruptionAndInvalidIdentity() {
  const CrashBootContext valid = context(kBootA, "git.0123456789ab", 7);
  std::array<uint8_t, kCrashContextBlobSize> blob = {};
  assert(encodeCrashBootContext(valid, blob.data(), blob.size()) ==
         CrashContextBlobError::kNone);

  auto damaged = blob;
  damaged[40] ^= 1;
  CrashBootContext output = context(kBootC, "sentinel", 99);
  assert(decodeCrashBootContext(damaged.data(), damaged.size(), &output) ==
         CrashContextBlobError::kChecksumMismatch);
  // A failed decode never leaks a partial or guessed identity.
  assert(std::strcmp(output.bootId, kBootC) == 0);

  damaged = blob;
  damaged[0] = 'X';
  assert(decodeCrashBootContext(damaged.data(), damaged.size(), &output) ==
         CrashContextBlobError::kBadMagic);
  damaged = blob;
  damaged[4] = 2;
  assert(decodeCrashBootContext(damaged.data(), damaged.size(), &output) ==
         CrashContextBlobError::kUnsupportedVersion);
  damaged = blob;
  damaged[6] = 0;
  assert(decodeCrashBootContext(damaged.data(), damaged.size(), &output) ==
         CrashContextBlobError::kBadPayloadLength);
  assert(decodeCrashBootContext(blob.data(), blob.size() - 1, &output) ==
         CrashContextBlobError::kWrongLength);
  assert(decodeCrashBootContext(nullptr, blob.size(), &output) ==
         CrashContextBlobError::kNullArgument);

  auto invalid = valid;
  invalid.generation = 0;
  std::array<uint8_t, kCrashContextBlobSize> untouched;
  untouched.fill(0xa5);
  const auto before = untouched;
  assert(encodeCrashBootContext(invalid, untouched.data(), untouched.size()) ==
         CrashContextBlobError::kInvalidRecord);
  assert(untouched == before);
  invalid = valid;
  std::strcpy(invalid.bootId, "ABCDEF0123456789abcdef0123456789");
  assert(!crashBootContextIsValid(invalid));
  invalid = valid;
  std::strcpy(invalid.buildId, "git/bad");
  assert(!crashBootContextIsValid(invalid));
}

void testFirstBootAndNormalRotation() {
  FakeStorage storage;
  auto result = rotateCrashContextWithStorage(
      kBootA, "git.build-a", ops(&storage));
  assert(result.currentPersisted());
  assert(!result.hasPrevious());
  assert(result.previousStatus == PreviousCrashContextStatus::kNotStored);
  assert(result.persistedGeneration == 1);
  assert(storage.writes == 1);
  assert(crashBootContextsEqual(load(storage, 0),
                                context(kBootA, "git.build-a", 1)));

  result = rotateCrashContextWithStorage(kBootB, "git.build-b",
                                         ops(&storage));
  assert(result.currentPersisted());
  assert(result.hasPrevious());
  assert(std::strcmp(result.previous.bootId, kBootA) == 0);
  assert(std::strcmp(result.previous.buildId, "git.build-a") == 0);
  assert(result.previous.generation == 1);
  assert(result.persistedGeneration == 2);
  assert(crashBootContextsEqual(load(storage, 1),
                                context(kBootB, "git.build-b", 2)));

  result = rotateCrashContextWithStorage(kBootC, "git.build-c",
                                         ops(&storage));
  assert(result.hasPrevious());
  assert(std::strcmp(result.previous.bootId, kBootB) == 0);
  assert(result.previous.generation == 2);
  assert(result.persistedGeneration == 3);
  assert(crashBootContextsEqual(load(storage, 0),
                                context(kBootC, "git.build-c", 3)));
}

void testCorruptSlotFailsClosedAndHeals() {
  FakeStorage storage;
  store(&storage, 0, context(kBootA, "git.old", 4));
  store(&storage, 1, context(kBootB, "git.newer", 5));
  storage.slots[1].bytes[70] ^= 1;

  auto result = rotateCrashContextWithStorage(
      kBootC, "git.current", ops(&storage));
  assert(result.currentPersisted());
  assert(!result.hasPrevious());
  assert(result.previous.bootId[0] == '\0');
  assert(result.previousStatus ==
         PreviousCrashContextStatus::kInvalidStoredData);
  assert(result.persistedGeneration == 5);
  assert(crashBootContextsEqual(load(storage, 1),
                                context(kBootC, "git.current", 5)));

  // On the next boot the healed, newer record is safe to attribute.
  result = rotateCrashContextWithStorage(kBootB, "git.next",
                                         ops(&storage));
  assert(result.hasPrevious());
  assert(std::strcmp(result.previous.bootId, kBootC) == 0);
  assert(std::strcmp(result.previous.buildId, "git.current") == 0);
}

void testBothCorruptAndContradictorySlotsHealWithoutAttribution() {
  FakeStorage corrupt;
  corrupt.slots[0].present = true;
  corrupt.slots[0].length = kCrashContextBlobSize;
  corrupt.slots[0].bytes.fill(0xa5);
  corrupt.slots[1] = corrupt.slots[0];
  auto result = rotateCrashContextWithStorage(
      kBootA, "git.repair", ops(&corrupt));
  assert(result.currentPersisted());
  assert(!result.hasPrevious());
  assert(result.previousStatus ==
         PreviousCrashContextStatus::kInvalidStoredData);
  assert(corrupt.writes == 2);
  assert(crashBootContextsEqual(load(corrupt, 0), load(corrupt, 1)));

  FakeStorage contradictory;
  store(&contradictory, 0, context(kBootA, "git.one", 9));
  store(&contradictory, 1, context(kBootB, "git.two", 9));
  result = rotateCrashContextWithStorage(kBootC, "git.supersede",
                                         ops(&contradictory));
  assert(result.currentPersisted());
  assert(!result.hasPrevious());
  assert(result.previousStatus ==
         PreviousCrashContextStatus::kInvalidStoredData);
  assert(result.persistedGeneration == 10);
}

void testIoAndValidationFailuresAreConservative() {
  FakeStorage storage;
  storage.slots[0].readFails = true;
  auto result = rotateCrashContextWithStorage(
      kBootA, "git.build", ops(&storage));
  assert(result.storage == CrashContextStorageResult::kReadFailed);
  assert(result.previousStatus == PreviousCrashContextStatus::kReadFailed);
  assert(!result.hasPrevious());
  assert(storage.writes == 0);

  storage = {};
  storage.writeFailureSlot = 0;
  result = rotateCrashContextWithStorage(kBootA, "git.build",
                                         ops(&storage));
  assert(result.storage == CrashContextStorageResult::kWriteFailed);
  assert(!result.hasPrevious());

  storage = {};
  storage.corruptAfterWriteSlot = 0;
  result = rotateCrashContextWithStorage(kBootA, "git.build",
                                         ops(&storage));
  assert(result.storage == CrashContextStorageResult::kVerifyFailed);
  assert(!result.hasPrevious());

  storage = {};
  result = rotateCrashContextWithStorage("short", "git.build",
                                         ops(&storage));
  assert(result.storage ==
         CrashContextStorageResult::kInvalidCurrentContext);
  assert(storage.writes == 0);

  CrashContextStorageOps invalidOps = {};
  result = rotateCrashContextWithStorage(kBootA, "git.build", invalidOps);
  assert(result.storage == CrashContextStorageResult::kInvalidStorageOps);

  store(&storage, 0,
        context(kBootA, "git.max", kCrashContextMaximumGeneration));
  storage.slots[1] = {};
  result = rotateCrashContextWithStorage(kBootB, "git.next",
                                         ops(&storage));
  assert(result.storage == CrashContextStorageResult::kGenerationExhausted);
  assert(!result.hasPrevious());

  assert(captureAndRotateCrashContext(kBootA, "git.native").storage ==
         CrashContextStorageResult::kUnsupportedPlatform);
}

void testPinnedDumpAttributionRoundTrip() {
  CrashDumpAttribution attribution = {};
  std::strcpy(attribution.bootId, kBootA);
  std::strcpy(attribution.buildId, "git.crashed-build+release");
  std::strcpy(attribution.resetReason, "task_watchdog");
  assert(crashDumpAttributionIsValid(attribution));

  std::array<uint8_t, kCrashDumpAttributionBlobSize> blob = {};
  assert(encodeCrashDumpAttribution(attribution, blob.data(), blob.size()) ==
         CrashContextBlobError::kNone);
  assert(blob[0] == 'M' && blob[1] == '5' && blob[2] == 'C' &&
         blob[3] == 'A');
  CrashDumpAttribution decoded = {};
  assert(decodeCrashDumpAttribution(blob.data(), blob.size(), &decoded) ==
         CrashContextBlobError::kNone);
  assert(crashDumpAttributionsEqual(attribution, decoded));

  auto damaged = blob;
  damaged[100] ^= 1;
  assert(decodeCrashDumpAttribution(damaged.data(), damaged.size(),
                                    &decoded) ==
         CrashContextBlobError::kChecksumMismatch);
  std::strcpy(attribution.resetReason, "Bad Reason");
  assert(!crashDumpAttributionIsValid(attribution));

  assert(loadPinnedCrashDumpAttribution(&decoded) ==
         CrashDumpAttributionStorageResult::kUnsupportedPlatform);
  assert(savePinnedCrashDumpAttribution(decoded) ==
         CrashDumpAttributionStorageResult::kUnsupportedPlatform);
  assert(clearPinnedCrashDumpAttribution() ==
         CrashDumpAttributionStorageResult::kUnsupportedPlatform);
}

}  // namespace

int main() {
  testStableBlobRoundTrip();
  testBlobRejectsCorruptionAndInvalidIdentity();
  testFirstBootAndNormalRotation();
  testCorruptSlotFailsClosedAndHeals();
  testBothCorruptAndContradictorySlotsHealWithoutAttribution();
  testIoAndValidationFailuresAreConservative();
  testPinnedDumpAttributionRoundTrip();
  return 0;
}
