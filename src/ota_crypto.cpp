#include "ota_crypto.h"

#include <cstring>
#include <limits>

#if defined(ARDUINO_ARCH_ESP32)
#include <tinycrypt/ecc.h>
#include <tinycrypt/ecc_dsa.h>
#endif

namespace {

constexpr uint32_t kRoundConstants[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
    0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
    0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
    0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
    0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
    0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
    0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
    0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

constexpr uint8_t kP256Order[kOtaSha256Size] = {
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17, 0x9e, 0x84,
    0xf3, 0xb9, 0xca, 0xc2, 0xfc, 0x63, 0x25, 0x51,
};

constexpr uint8_t kP256HalfOrder[kOtaSha256Size] = {
    0x7f, 0xff, 0xff, 0xff, 0x80, 0x00, 0x00, 0x00,
    0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xde, 0x73, 0x7d, 0x56, 0xd3, 0x8b, 0xcf, 0x42,
    0x79, 0xdc, 0xe5, 0x61, 0x7e, 0x31, 0x92, 0xa8,
};

uint32_t rotateRight(uint32_t value, uint8_t count) {
  return (value >> count) | (value << (32U - count));
}

bool allZero(const uint8_t* value, size_t size) {
  uint8_t combined = 0;
  for (size_t index = 0; index < size; ++index) {
    combined |= value[index];
  }
  return combined == 0;
}

int compareBigEndian(const uint8_t* left, const uint8_t* right, size_t size) {
  for (size_t index = 0; index < size; ++index) {
    if (left[index] < right[index]) {
      return -1;
    }
    if (left[index] > right[index]) {
      return 1;
    }
  }
  return 0;
}

}  // namespace

OtaSha256::OtaSha256() { reset(); }

void OtaSha256::reset() {
  state_[0] = 0x6a09e667U;
  state_[1] = 0xbb67ae85U;
  state_[2] = 0x3c6ef372U;
  state_[3] = 0xa54ff53aU;
  state_[4] = 0x510e527fU;
  state_[5] = 0x9b05688cU;
  state_[6] = 0x1f83d9abU;
  state_[7] = 0x5be0cd19U;
  totalBytes_ = 0;
  bufferSize_ = 0;
  finalized_ = false;
  std::memset(buffer_, 0, sizeof(buffer_));
}

bool OtaSha256::update(const uint8_t* bytes, size_t size) {
  if (finalized_ || (bytes == nullptr && size != 0) ||
      size > std::numeric_limits<uint64_t>::max() - totalBytes_) {
    return false;
  }
  totalBytes_ += size;
  size_t consumed = 0;
  if (bufferSize_ != 0) {
    const size_t needed = sizeof(buffer_) - bufferSize_;
    const size_t copied = size < needed ? size : needed;
    if (copied != 0) {
      std::memcpy(buffer_ + bufferSize_, bytes, copied);
      bufferSize_ += copied;
      consumed += copied;
    }
    if (bufferSize_ == sizeof(buffer_)) {
      transform(buffer_);
      bufferSize_ = 0;
    }
  }
  while (size - consumed >= sizeof(buffer_)) {
    transform(bytes + consumed);
    consumed += sizeof(buffer_);
  }
  if (consumed < size) {
    bufferSize_ = size - consumed;
    std::memcpy(buffer_, bytes + consumed, bufferSize_);
  }
  return true;
}

bool OtaSha256::finish(uint8_t output[kOtaSha256Size]) {
  if (output == nullptr || finalized_ ||
      totalBytes_ > std::numeric_limits<uint64_t>::max() / 8U) {
    return false;
  }
  const uint64_t bitLength = totalBytes_ * 8U;
  buffer_[bufferSize_++] = 0x80;
  if (bufferSize_ > 56) {
    std::memset(buffer_ + bufferSize_, 0, sizeof(buffer_) - bufferSize_);
    transform(buffer_);
    bufferSize_ = 0;
  }
  std::memset(buffer_ + bufferSize_, 0, 56 - bufferSize_);
  for (size_t index = 0; index < 8; ++index) {
    buffer_[56 + index] =
        static_cast<uint8_t>(bitLength >> ((7U - index) * 8U));
  }
  transform(buffer_);
  for (size_t word = 0; word < 8; ++word) {
    for (size_t byte = 0; byte < 4; ++byte) {
      output[word * 4 + byte] =
          static_cast<uint8_t>(state_[word] >> ((3U - byte) * 8U));
    }
  }
  std::memset(buffer_, 0, sizeof(buffer_));
  bufferSize_ = 0;
  finalized_ = true;
  return true;
}

void OtaSha256::transform(const uint8_t block[64]) {
  uint32_t words[64] = {};
  for (size_t index = 0; index < 16; ++index) {
    words[index] = static_cast<uint32_t>(block[index * 4]) << 24U |
                   static_cast<uint32_t>(block[index * 4 + 1]) << 16U |
                   static_cast<uint32_t>(block[index * 4 + 2]) << 8U |
                   static_cast<uint32_t>(block[index * 4 + 3]);
  }
  for (size_t index = 16; index < 64; ++index) {
    const uint32_t first = rotateRight(words[index - 15], 7) ^
                           rotateRight(words[index - 15], 18) ^
                           (words[index - 15] >> 3U);
    const uint32_t second = rotateRight(words[index - 2], 17) ^
                            rotateRight(words[index - 2], 19) ^
                            (words[index - 2] >> 10U);
    words[index] = words[index - 16] + first + words[index - 7] + second;
  }

  uint32_t a = state_[0];
  uint32_t b = state_[1];
  uint32_t c = state_[2];
  uint32_t d = state_[3];
  uint32_t e = state_[4];
  uint32_t f = state_[5];
  uint32_t g = state_[6];
  uint32_t h = state_[7];
  for (size_t index = 0; index < 64; ++index) {
    const uint32_t sigmaOne =
        rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
    const uint32_t choose = (e & f) ^ (~e & g);
    const uint32_t first =
        h + sigmaOne + choose + kRoundConstants[index] + words[index];
    const uint32_t sigmaZero =
        rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
    const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t second = sigmaZero + majority;
    h = g;
    g = f;
    f = e;
    e = d + first;
    d = c;
    c = b;
    b = a;
    a = first + second;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
  std::memset(words, 0, sizeof(words));
}

bool otaConstantTimeEqual(const uint8_t* left, const uint8_t* right,
                          size_t size) {
  if (left == nullptr || right == nullptr) {
    return false;
  }
  uint8_t difference = 0;
  for (size_t index = 0; index < size; ++index) {
    difference |= left[index] ^ right[index];
  }
  return difference == 0;
}

bool otaP256SignatureIsCanonical(
    const uint8_t signature[kOtaP256SignatureSize]) {
  if (signature == nullptr) {
    return false;
  }
  const uint8_t* r = signature;
  const uint8_t* s = signature + kOtaSha256Size;
  return !allZero(r, kOtaSha256Size) && !allZero(s, kOtaSha256Size) &&
         compareBigEndian(r, kP256Order, kOtaSha256Size) < 0 &&
         compareBigEndian(s, kP256HalfOrder, kOtaSha256Size) <= 0;
}

OtaSignatureVerificationResult otaVerifyP256Sha256Digest(
    const uint8_t digest[kOtaSha256Size],
    const uint8_t signature[kOtaP256SignatureSize],
    const uint8_t publicKey[kOtaP256PublicKeySize]) {
  if (digest == nullptr || signature == nullptr || publicKey == nullptr) {
    return OtaSignatureVerificationResult::kNullArgument;
  }
  if (publicKey[0] != 0x04) {
    return OtaSignatureVerificationResult::kBadPublicKeyEncoding;
  }
  if (!otaP256SignatureIsCanonical(signature)) {
    return OtaSignatureVerificationResult::kNonCanonicalSignature;
  }
#if defined(ARDUINO_ARCH_ESP32)
  // ESP-IDF's bundled TinyCrypt verifier uses bounded stack/static arithmetic
  // rather than heap-backed multi-precision integers. The wire key includes
  // SEC1's 0x04 prefix; TinyCrypt accepts the following X || Y bytes.
  const uECC_Curve curve = uECC_secp256r1();
  if (curve == nullptr) {
    return OtaSignatureVerificationResult::kCryptoFailure;
  }
  if (uECC_valid_public_key(publicKey + 1, curve) != 0) {
    return OtaSignatureVerificationResult::kBadPublicKeyEncoding;
  }
  return uECC_verify(publicKey + 1, digest, kOtaSha256Size, signature, curve) ==
                 1
             ? OtaSignatureVerificationResult::kOk
             : OtaSignatureVerificationResult::kInvalidSignature;
#else
  return OtaSignatureVerificationResult::kUnsupportedPlatform;
#endif
}

const char* otaSignatureVerificationResultName(
    OtaSignatureVerificationResult result) {
  switch (result) {
    case OtaSignatureVerificationResult::kOk:
      return "ok";
    case OtaSignatureVerificationResult::kNullArgument:
      return "null_argument";
    case OtaSignatureVerificationResult::kBadPublicKeyEncoding:
      return "bad_public_key_encoding";
    case OtaSignatureVerificationResult::kNonCanonicalSignature:
      return "noncanonical_signature";
    case OtaSignatureVerificationResult::kInvalidSignature:
      return "invalid_signature";
    case OtaSignatureVerificationResult::kCryptoFailure:
      return "crypto_failure";
    case OtaSignatureVerificationResult::kUnsupportedPlatform:
      return "unsupported_platform";
  }
  return "unknown";
}
