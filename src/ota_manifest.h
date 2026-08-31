#pragma once

#include <cstddef>
#include <cstdint>

constexpr uint8_t kOtaManifestFormatVersion = 1;
constexpr uint8_t kOtaSignatureFormatVersion = 1;
constexpr size_t kOtaSha256Size = 32;
constexpr size_t kOtaP256SignatureSize = 64;
constexpr size_t kOtaP256PublicKeySize = 65;
constexpr size_t kOtaManifestMaximumSize = 320;
constexpr size_t kOtaHardwareMaximumLength = 48;
constexpr size_t kOtaFirmwareVersionMaximumLength = 32;
constexpr size_t kOtaBuildIdMaximumLength = 64;
constexpr size_t kOtaSigningKeyIdMaximumLength = 32;
constexpr uint32_t kOtaFirmwareMaximumSize = UINT32_C(0x640000);
constexpr uint32_t kOtaElfMaximumSize = 64U * 1024U * 1024U;
constexpr uint64_t kOtaMaximumReleaseCounter = UINT64_C(0x7fffffffffffffff);

// Parsed view of the canonical v1 manifest. Every retained string is NUL
// terminated, but the wire record uses explicit 16-bit network-order lengths.
struct OtaManifest {
  uint8_t signatureFormatVersion = 0;
  char hardware[kOtaHardwareMaximumLength + 1] = {};
  char firmwareVersion[kOtaFirmwareVersionMaximumLength + 1] = {};
  uint64_t releaseCounter = 0;
  char buildId[kOtaBuildIdMaximumLength + 1] = {};
  char signingKeyId[kOtaSigningKeyIdMaximumLength + 1] = {};
  uint32_t firmwareSize = 0;
  uint8_t firmwareSha256[kOtaSha256Size] = {};
  uint32_t elfSize = 0;
  uint8_t elfSha256[kOtaSha256Size] = {};
};

enum class OtaManifestParseError : uint8_t {
  kNone = 0,
  kNullArgument,
  kLengthOutOfRange,
  kBadMagic,
  kUnsupportedManifestVersion,
  kUnsupportedSignatureVersion,
  kTotalLengthMismatch,
  kTruncated,
  kStringLengthOutOfRange,
  kNonCanonicalString,
  kReleaseCounterOutOfRange,
  kFirmwareSizeOutOfRange,
  kElfSizeOutOfRange,
  kDigestLengthMismatch,
  kTrailingData,
};

struct OtaManifestParseResult {
  OtaManifest manifest = {};
  OtaManifestParseError error = OtaManifestParseError::kNone;

  bool ok() const { return error == OtaManifestParseError::kNone; }
  explicit operator bool() const { return ok(); }
};

// Parses only one exact, canonical v1 record. Unknown/trailing fields and a
// second textual spelling are rejected so the signed byte interpretation cannot
// vary between the packager, backend, and device.
OtaManifestParseResult parseOtaManifest(const uint8_t* bytes, size_t size);

const char* otaManifestParseErrorName(OtaManifestParseError error);
