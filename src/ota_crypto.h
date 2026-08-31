#pragma once

#include <cstddef>
#include <cstdint>

#include "ota_manifest.h"

// Fixed-memory incremental SHA-256. It can hash a streamed application image
// without retaining the image or allocating from the heap.
class OtaSha256 {
 public:
  OtaSha256();

  void reset();
  bool update(const uint8_t* bytes, size_t size);
  bool finish(uint8_t output[kOtaSha256Size]);
  bool finalized() const { return finalized_; }
  uint64_t bytesHashed() const { return totalBytes_; }

 private:
  void transform(const uint8_t block[64]);

  uint32_t state_[8] = {};
  uint64_t totalBytes_ = 0;
  uint8_t buffer_[64] = {};
  size_t bufferSize_ = 0;
  bool finalized_ = false;
};

bool otaConstantTimeEqual(const uint8_t* left, const uint8_t* right,
                          size_t size);

// v1 signatures are a canonical fixed-width P-256 r||s pair. Low-S is
// required to remove ECDSA malleability from release artifacts.
bool otaP256SignatureIsCanonical(
    const uint8_t signature[kOtaP256SignatureSize]);

enum class OtaSignatureVerificationResult : uint8_t {
  kOk = 0,
  kNullArgument,
  kBadPublicKeyEncoding,
  kNonCanonicalSignature,
  kInvalidSignature,
  kCryptoFailure,
  kUnsupportedPlatform,
};

// The public key is a 65-byte SEC1 uncompressed point (04 || X || Y).
// The ESP32 implementation delegates curve arithmetic to the framework's
// fixed-workspace TinyCrypt P-256 verifier; this module performs no heap
// allocation on the classic ESP32.
OtaSignatureVerificationResult otaVerifyP256Sha256Digest(
    const uint8_t digest[kOtaSha256Size],
    const uint8_t signature[kOtaP256SignatureSize],
    const uint8_t publicKey[kOtaP256PublicKeySize]);

const char* otaSignatureVerificationResultName(
    OtaSignatureVerificationResult result);
