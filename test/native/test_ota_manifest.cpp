#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "ota_crypto.h"
#include "ota_manifest.h"

namespace {

std::vector<uint8_t> decodeHex(const char* hex) {
  const size_t length = std::strlen(hex);
  assert(length % 2 == 0);
  std::vector<uint8_t> bytes;
  bytes.reserve(length / 2);
  auto digit = [](char value) -> uint8_t {
    if (value >= '0' && value <= '9') {
      return static_cast<uint8_t>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
      return static_cast<uint8_t>(value - 'a' + 10);
    }
    assert(false);
    return 0;
  };
  for (size_t index = 0; index < length; index += 2) {
    bytes.push_back(static_cast<uint8_t>((digit(hex[index]) << 4U) |
                                         digit(hex[index + 1])));
  }
  return bytes;
}

std::vector<uint8_t> canonicalManifest() {
  // Exact bytes emitted by tools/ota_release.py for the shared v1 fixture.
  return decodeHex(
      "4d354f540101009d00166d35676f2d636c61737369632d65737033322d31366d"
      "0005302e372e300000000000000007001030313233343536373839616263646566"
      "000e72656c656173652d323032362d61000000030020ba7816bf8f01cfea414140"
      "de5dae2223b00361a396177a9cb410ff61f20015ad000000040020000102030405"
      "060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
}

std::string hexDigest(const uint8_t digest[kOtaSha256Size]) {
  constexpr char digits[] = "0123456789abcdef";
  std::string output;
  output.reserve(kOtaSha256Size * 2);
  for (size_t index = 0; index < kOtaSha256Size; ++index) {
    output.push_back(digits[digest[index] >> 4U]);
    output.push_back(digits[digest[index] & 0x0fU]);
  }
  return output;
}

void testCanonicalManifestParses() {
  const std::vector<uint8_t> bytes = canonicalManifest();
  const OtaManifestParseResult parsed =
      parseOtaManifest(bytes.data(), bytes.size());
  assert(parsed.ok());
  assert(std::strcmp(parsed.manifest.hardware,
                     "m5go-classic-esp32-16m") == 0);
  assert(std::strcmp(parsed.manifest.firmwareVersion, "0.7.0") == 0);
  assert(parsed.manifest.releaseCounter == 7);
  assert(std::strcmp(parsed.manifest.buildId, "0123456789abcdef") == 0);
  assert(std::strcmp(parsed.manifest.signingKeyId, "release-2026-a") == 0);
  assert(parsed.manifest.firmwareSize == 3);
  assert(parsed.manifest.elfSize == 4);
  assert(parsed.manifest.firmwareSha256[0] == 0xba);
  assert(parsed.manifest.elfSha256[31] == 31);
}

void testManifestRejectsMalformedRecords() {
  std::vector<uint8_t> bytes = canonicalManifest();
  assert(parseOtaManifest(nullptr, bytes.size()).error ==
         OtaManifestParseError::kNullArgument);
  assert(parseOtaManifest(bytes.data(), bytes.size() - 1).error ==
         OtaManifestParseError::kTotalLengthMismatch);

  bytes[0] ^= 1;
  assert(parseOtaManifest(bytes.data(), bytes.size()).error ==
         OtaManifestParseError::kBadMagic);
  bytes = canonicalManifest();
  bytes[4] = 2;
  assert(parseOtaManifest(bytes.data(), bytes.size()).error ==
         OtaManifestParseError::kUnsupportedManifestVersion);
  bytes = canonicalManifest();
  bytes[8] = 0;
  bytes[9] = 0;
  assert(parseOtaManifest(bytes.data(), bytes.size()).error ==
         OtaManifestParseError::kStringLengthOutOfRange);
  bytes = canonicalManifest();
  bytes[10] = '/';
  assert(parseOtaManifest(bytes.data(), bytes.size()).error ==
         OtaManifestParseError::kNonCanonicalString);
  bytes = canonicalManifest();
  bytes[39] = 0x80;
  assert(parseOtaManifest(bytes.data(), bytes.size()).error ==
         OtaManifestParseError::kReleaseCounterOutOfRange);
  bytes = canonicalManifest();
  bytes[81] = 0x00;
  bytes[82] = 0x64;
  bytes[83] = 0x00;
  bytes[84] = 0x01;
  assert(parseOtaManifest(bytes.data(), bytes.size()).error ==
         OtaManifestParseError::kFirmwareSizeOutOfRange);
}

void testSha256KnownVectorsAndStreaming() {
  uint8_t digest[kOtaSha256Size] = {};
  OtaSha256 empty;
  assert(empty.update(nullptr, 0));
  assert(empty.finish(digest));
  assert(hexDigest(digest) ==
         "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  assert(!empty.finish(digest));

  OtaSha256 abc;
  assert(abc.update(reinterpret_cast<const uint8_t*>("a"), 1));
  assert(abc.update(reinterpret_cast<const uint8_t*>("bc"), 2));
  assert(abc.bytesHashed() == 3);
  assert(abc.finish(digest));
  assert(hexDigest(digest) ==
         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  assert(!abc.update(reinterpret_cast<const uint8_t*>("x"), 1));
}

void testCanonicalSignatureRules() {
  uint8_t signature[kOtaP256SignatureSize] = {};
  signature[31] = 1;
  signature[63] = 1;
  assert(otaP256SignatureIsCanonical(signature));
  signature[32] = 0x80;
  assert(!otaP256SignatureIsCanonical(signature));
  std::memset(signature, 0, sizeof(signature));
  assert(!otaP256SignatureIsCanonical(signature));
}

}  // namespace

int main() {
  testCanonicalManifestParses();
  testManifestRejectsMalformedRecords();
  testSha256KnownVectorsAndStreaming();
  testCanonicalSignatureRules();
  return 0;
}
