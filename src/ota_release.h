#pragma once

#include <cstddef>
#include <cstdint>

#include "ota_crypto.h"
#include "ota_manifest.h"

constexpr size_t kOtaMaximumTrustedKeys = 2;

struct OtaTrustKey {
  char keyId[kOtaSigningKeyIdMaximumLength + 1] = {};
  uint8_t publicKey[kOtaP256PublicKeySize] = {};
};

using OtaSignatureVerifier = OtaSignatureVerificationResult (*)(
    const uint8_t digest[kOtaSha256Size],
    const uint8_t signature[kOtaP256SignatureSize],
    const uint8_t publicKey[kOtaP256PublicKeySize]);

struct OtaReleaseValidationResult;

class OtaVerifiedRelease {
 public:
  OtaVerifiedRelease() = default;

  bool valid() const;
  const OtaManifest& manifest() const { return manifest_; }
  const uint8_t* manifestDigest() const { return manifestDigest_; }

 private:
  friend struct OtaReleaseValidationResult;
  friend OtaReleaseValidationResult validateOtaRelease(
      const uint8_t*, size_t, const uint8_t*, size_t, const OtaTrustKey*,
      size_t, const char*, uint64_t, uint32_t, OtaSignatureVerifier);

  static constexpr uint32_t kVerificationMarker = 0x4f544156U;  // "OTAV"
  OtaManifest manifest_ = {};
  uint8_t manifestDigest_[kOtaSha256Size] = {};
  uint32_t marker_ = 0;
};

enum class OtaReleaseValidationError : uint8_t {
  kNone = 0,
  kNullArgument,
  kManifestInvalid,
  kSignatureLengthMismatch,
  kExpectedHardwareInvalid,
  kHardwareMismatch,
  kReleaseNotNewer,
  kImageTooLarge,
  kTrustSetSizeOutOfRange,
  kTrustKeyInvalid,
  kSigningKeyNotTrusted,
  kSigningKeyAmbiguous,
  kHashFailure,
  kSignatureRejected,
};

struct OtaReleaseValidationResult {
  OtaVerifiedRelease release = {};
  OtaReleaseValidationError error = OtaReleaseValidationError::kNone;
  OtaManifestParseError manifestError = OtaManifestParseError::kNone;
  OtaSignatureVerificationResult signatureResult =
      OtaSignatureVerificationResult::kOk;

  bool ok() const { return error == OtaReleaseValidationError::kNone; }
  explicit operator bool() const { return ok(); }
};

// Verifies one immutable release before any flash write begins. The confirmed
// release counter is monotonic; wireless installs require a strictly greater
// value. USB recovery deliberately lives outside this API.
OtaReleaseValidationResult validateOtaRelease(
    const uint8_t* manifestBytes, size_t manifestSize,
    const uint8_t* signature, size_t signatureSize,
    const OtaTrustKey* trustedKeys, size_t trustedKeyCount,
    const char* expectedHardware, uint64_t confirmedReleaseCounter,
    uint32_t maximumImageSize,
    OtaSignatureVerifier verifier = otaVerifyP256Sha256Digest);

const char* otaReleaseValidationErrorName(OtaReleaseValidationError error);
