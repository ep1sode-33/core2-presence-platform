#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

#include "ota_release.h"
#include "ota_update.h"

namespace {

bool rejectSignature = false;

OtaSignatureVerificationResult fakeVerifier(
    const uint8_t digest[kOtaSha256Size],
    const uint8_t signature[kOtaP256SignatureSize],
    const uint8_t publicKey[kOtaP256PublicKeySize]) {
  if (rejectSignature || digest[0] == 0 || signature[31] != 1 ||
      signature[63] != 1 || publicKey[0] != 0x04) {
    return OtaSignatureVerificationResult::kInvalidSignature;
  }
  return OtaSignatureVerificationResult::kOk;
}

std::vector<uint8_t> decodeHex(const char* hex) {
  const size_t length = std::strlen(hex);
  std::vector<uint8_t> bytes;
  auto digit = [](char value) -> uint8_t {
    return value <= '9' ? static_cast<uint8_t>(value - '0')
                        : static_cast<uint8_t>(value - 'a' + 10);
  };
  for (size_t index = 0; index < length; index += 2) {
    bytes.push_back(static_cast<uint8_t>((digit(hex[index]) << 4U) |
                                         digit(hex[index + 1])));
  }
  return bytes;
}

std::vector<uint8_t> canonicalManifest() {
  return decodeHex(
      "4d354f540101009d00166d35676f2d636c61737369632d65737033322d31366d"
      "0005302e372e300000000000000007001030313233343536373839616263646566"
      "000e72656c656173652d323032362d61000000030020ba7816bf8f01cfea414140"
      "de5dae2223b00361a396177a9cb410ff61f20015ad000000040020000102030405"
      "060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
}

OtaReleaseValidationResult validate(uint64_t currentCounter = 6,
                                    const char* hardware =
                                        "m5go-classic-esp32-16m") {
  const std::vector<uint8_t> manifest = canonicalManifest();
  uint8_t signature[kOtaP256SignatureSize] = {};
  signature[31] = 1;
  signature[63] = 1;
  OtaTrustKey key = {};
  std::strcpy(key.keyId, "release-2026-a");
  key.publicKey[0] = 0x04;
  return validateOtaRelease(manifest.data(), manifest.size(), signature,
                            sizeof(signature), &key, 1, hardware,
                            currentCounter, 0x640000, fakeVerifier);
}

void testReleasePolicyAndTrust() {
  rejectSignature = false;
  const OtaReleaseValidationResult accepted = validate();
  assert(accepted.ok());
  assert(accepted.release.valid());
  assert(accepted.release.manifest().releaseCounter == 7);

  assert(validate(7).error == OtaReleaseValidationError::kReleaseNotNewer);
  assert(validate(8).error == OtaReleaseValidationError::kReleaseNotNewer);
  assert(validate(6, "core2").error ==
         OtaReleaseValidationError::kHardwareMismatch);

  rejectSignature = true;
  const OtaReleaseValidationResult rejected = validate();
  assert(rejected.error == OtaReleaseValidationError::kSignatureRejected);
  assert(rejected.signatureResult ==
         OtaSignatureVerificationResult::kInvalidSignature);
  rejectSignature = false;
}

struct FakeUpdateBackend {
  uint8_t bytes[16] = {};
  size_t size = 0;
  uint32_t expectedSize = 0;
  bool began = false;
  bool finished = false;
  bool aborted = false;
  bool shortWrite = false;
};

bool fakeBegin(void* context, uint32_t imageSize) {
  auto* backend = static_cast<FakeUpdateBackend*>(context);
  backend->began = true;
  backend->expectedSize = imageSize;
  return imageSize <= sizeof(backend->bytes);
}

size_t fakeWrite(void* context, const uint8_t* bytes, size_t size) {
  auto* backend = static_cast<FakeUpdateBackend*>(context);
  const size_t written = backend->shortWrite && size != 0 ? size - 1 : size;
  std::memcpy(backend->bytes + backend->size, bytes, written);
  backend->size += written;
  return written;
}

bool fakeFinish(void* context) {
  auto* backend = static_cast<FakeUpdateBackend*>(context);
  backend->finished = true;
  return backend->size == backend->expectedSize;
}

void fakeAbort(void* context) {
  static_cast<FakeUpdateBackend*>(context)->aborted = true;
}

OtaUpdateBackend callbacks(FakeUpdateBackend& backend) {
  return {&backend, fakeBegin, fakeWrite, fakeFinish, fakeAbort};
}

void testStreamedUpdateVerifiesBeforeFinalize() {
  const OtaVerifiedRelease release = validate().release;
  FakeUpdateBackend backend;
  OtaStreamUpdater updater;
  assert(updater.begin(release, callbacks(backend)));
  assert(backend.began);
  assert(updater.write(reinterpret_cast<const uint8_t*>("a"), 1));
  assert(updater.write(reinterpret_cast<const uint8_t*>("bc"), 2));
  assert(updater.progressPermille() == 1000);
  assert(!backend.finished);
  assert(updater.finish());
  assert(backend.finished);
  assert(!backend.aborted);
  assert(updater.state() == OtaUpdateState::kReadyToReboot);
}

void testStreamedUpdateRejectsCorruptionAndPartialWrites() {
  const OtaVerifiedRelease release = validate().release;
  {
    FakeUpdateBackend backend;
    OtaStreamUpdater updater;
    assert(updater.begin(release, callbacks(backend)));
    assert(updater.write(reinterpret_cast<const uint8_t*>("abd"), 3));
    assert(!updater.finish());
    assert(updater.error() == OtaUpdateError::kImageDigestMismatch);
    assert(backend.aborted);
    assert(!backend.finished);
  }
  {
    FakeUpdateBackend backend;
    OtaStreamUpdater updater;
    assert(updater.begin(release, callbacks(backend)));
    assert(updater.write(reinterpret_cast<const uint8_t*>("ab"), 2));
    assert(!updater.finish());
    assert(updater.error() == OtaUpdateError::kImageLengthMismatch);
    assert(backend.aborted);
  }
  {
    FakeUpdateBackend backend;
    backend.shortWrite = true;
    OtaStreamUpdater updater;
    assert(updater.begin(release, callbacks(backend)));
    assert(!updater.write(reinterpret_cast<const uint8_t*>("abc"), 3));
    assert(updater.error() == OtaUpdateError::kBackendShortWrite);
    assert(backend.aborted);
  }
}

void testStreamedUpdateRejectsOversizedChunks() {
  const OtaVerifiedRelease release = validate().release;
  FakeUpdateBackend backend;
  OtaStreamUpdater updater;
  assert(updater.begin(release, callbacks(backend)));
  std::vector<uint8_t> oversized(kOtaMaximumWriteChunkSize + 1, 0);
  assert(!updater.write(oversized.data(), oversized.size()));
  assert(updater.error() == OtaUpdateError::kChunkTooLarge);
  assert(backend.aborted);
}

}  // namespace

int main() {
  testReleasePolicyAndTrust();
  testStreamedUpdateVerifiesBeforeFinalize();
  testStreamedUpdateRejectsCorruptionAndPartialWrites();
  testStreamedUpdateRejectsOversizedChunks();
  return 0;
}
