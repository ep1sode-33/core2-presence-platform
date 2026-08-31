#include "ota_release.h"

#include <cstring>

namespace {

bool boundedCanonicalText(const char* text, size_t maximumLength) {
  if (text == nullptr) {
    return false;
  }
  const void* terminator = std::memchr(text, '\0', maximumLength + 1);
  if (terminator == nullptr || terminator == text) {
    return false;
  }
  const size_t length = static_cast<const char*>(terminator) - text;
  for (size_t index = 0; index < length; ++index) {
    const char value = text[index];
    if (!((value >= 'a' && value <= 'z') ||
          (value >= 'A' && value <= 'Z') ||
          (value >= '0' && value <= '9') || value == '.' || value == '_' ||
          value == '+' || value == '-')) {
      return false;
    }
  }
  return true;
}

OtaReleaseValidationResult failure(
    OtaReleaseValidationError error,
    OtaManifestParseError manifestError = OtaManifestParseError::kNone,
    OtaSignatureVerificationResult signatureResult =
        OtaSignatureVerificationResult::kOk) {
  return {OtaVerifiedRelease{}, error, manifestError, signatureResult};
}

}  // namespace

bool OtaVerifiedRelease::valid() const {
  return marker_ == kVerificationMarker && manifest_.releaseCounter != 0 &&
         manifest_.firmwareSize != 0;
}

OtaReleaseValidationResult validateOtaRelease(
    const uint8_t* manifestBytes, size_t manifestSize,
    const uint8_t* signature, size_t signatureSize,
    const OtaTrustKey* trustedKeys, size_t trustedKeyCount,
    const char* expectedHardware, uint64_t confirmedReleaseCounter,
    uint32_t maximumImageSize, OtaSignatureVerifier verifier) {
  if (manifestBytes == nullptr || signature == nullptr || trustedKeys == nullptr ||
      expectedHardware == nullptr || verifier == nullptr) {
    return failure(OtaReleaseValidationError::kNullArgument);
  }
  const OtaManifestParseResult parsed =
      parseOtaManifest(manifestBytes, manifestSize);
  if (!parsed.ok()) {
    return failure(OtaReleaseValidationError::kManifestInvalid, parsed.error);
  }
  if (signatureSize != kOtaP256SignatureSize) {
    return failure(OtaReleaseValidationError::kSignatureLengthMismatch);
  }
  if (!boundedCanonicalText(expectedHardware, kOtaHardwareMaximumLength)) {
    return failure(OtaReleaseValidationError::kExpectedHardwareInvalid);
  }
  if (std::strcmp(parsed.manifest.hardware, expectedHardware) != 0) {
    return failure(OtaReleaseValidationError::kHardwareMismatch);
  }
  if (parsed.manifest.releaseCounter <= confirmedReleaseCounter) {
    return failure(OtaReleaseValidationError::kReleaseNotNewer);
  }
  if (maximumImageSize == 0 || parsed.manifest.firmwareSize > maximumImageSize) {
    return failure(OtaReleaseValidationError::kImageTooLarge);
  }
  if (trustedKeyCount == 0 || trustedKeyCount > kOtaMaximumTrustedKeys) {
    return failure(OtaReleaseValidationError::kTrustSetSizeOutOfRange);
  }

  const OtaTrustKey* signingKey = nullptr;
  size_t matchingKeyCount = 0;
  for (size_t index = 0; index < trustedKeyCount; ++index) {
    if (!boundedCanonicalText(trustedKeys[index].keyId,
                              kOtaSigningKeyIdMaximumLength) ||
        trustedKeys[index].publicKey[0] != 0x04) {
      return failure(OtaReleaseValidationError::kTrustKeyInvalid);
    }
    if (std::strcmp(trustedKeys[index].keyId,
                    parsed.manifest.signingKeyId) == 0) {
      signingKey = &trustedKeys[index];
      ++matchingKeyCount;
    }
  }
  if (matchingKeyCount == 0) {
    return failure(OtaReleaseValidationError::kSigningKeyNotTrusted);
  }
  if (matchingKeyCount != 1) {
    return failure(OtaReleaseValidationError::kSigningKeyAmbiguous);
  }

  OtaSha256 hasher;
  uint8_t manifestDigest[kOtaSha256Size] = {};
  if (!hasher.update(manifestBytes, manifestSize) ||
      !hasher.finish(manifestDigest)) {
    return failure(OtaReleaseValidationError::kHashFailure);
  }
  const OtaSignatureVerificationResult signatureResult = verifier(
      manifestDigest, signature, signingKey->publicKey);
  if (signatureResult != OtaSignatureVerificationResult::kOk) {
    return failure(OtaReleaseValidationError::kSignatureRejected,
                   OtaManifestParseError::kNone, signatureResult);
  }

  OtaVerifiedRelease verified;
  verified.manifest_ = parsed.manifest;
  std::memcpy(verified.manifestDigest_, manifestDigest,
              sizeof(verified.manifestDigest_));
  verified.marker_ = OtaVerifiedRelease::kVerificationMarker;
  return {verified, OtaReleaseValidationError::kNone,
          OtaManifestParseError::kNone,
          OtaSignatureVerificationResult::kOk};
}

const char* otaReleaseValidationErrorName(OtaReleaseValidationError error) {
  switch (error) {
    case OtaReleaseValidationError::kNone:
      return "none";
    case OtaReleaseValidationError::kNullArgument:
      return "null_argument";
    case OtaReleaseValidationError::kManifestInvalid:
      return "manifest_invalid";
    case OtaReleaseValidationError::kSignatureLengthMismatch:
      return "signature_length_mismatch";
    case OtaReleaseValidationError::kExpectedHardwareInvalid:
      return "expected_hardware_invalid";
    case OtaReleaseValidationError::kHardwareMismatch:
      return "hardware_mismatch";
    case OtaReleaseValidationError::kReleaseNotNewer:
      return "release_not_newer";
    case OtaReleaseValidationError::kImageTooLarge:
      return "image_too_large";
    case OtaReleaseValidationError::kTrustSetSizeOutOfRange:
      return "trust_set_size_out_of_range";
    case OtaReleaseValidationError::kTrustKeyInvalid:
      return "trust_key_invalid";
    case OtaReleaseValidationError::kSigningKeyNotTrusted:
      return "signing_key_not_trusted";
    case OtaReleaseValidationError::kSigningKeyAmbiguous:
      return "signing_key_ambiguous";
    case OtaReleaseValidationError::kHashFailure:
      return "hash_failure";
    case OtaReleaseValidationError::kSignatureRejected:
      return "signature_rejected";
  }
  return "unknown";
}
