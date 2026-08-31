#include "ota_manifest.h"

#include <cstring>

namespace {

constexpr uint8_t kMagic[] = {'M', '5', 'O', 'T'};
constexpr size_t kHeaderSize = 8;

bool canonicalTextByte(uint8_t value) {
  return (value >= 'a' && value <= 'z') ||
         (value >= 'A' && value <= 'Z') ||
         (value >= '0' && value <= '9') || value == '.' || value == '_' ||
         value == '+' || value == '-';
}

class Reader {
 public:
  Reader(const uint8_t* bytes, size_t size)
      : bytes_(bytes), size_(size), position_(kHeaderSize) {}

  bool readU16(uint16_t& output) {
    if (!has(2)) {
      return false;
    }
    output = static_cast<uint16_t>(bytes_[position_]) << 8U |
             static_cast<uint16_t>(bytes_[position_ + 1]);
    position_ += 2;
    return true;
  }

  bool readU32(uint32_t& output) {
    if (!has(4)) {
      return false;
    }
    output = 0;
    for (size_t index = 0; index < 4; ++index) {
      output = (output << 8U) | bytes_[position_ + index];
    }
    position_ += 4;
    return true;
  }

  bool readU64(uint64_t& output) {
    if (!has(8)) {
      return false;
    }
    output = 0;
    for (size_t index = 0; index < 8; ++index) {
      output = (output << 8U) | bytes_[position_ + index];
    }
    position_ += 8;
    return true;
  }

  OtaManifestParseError readText(char* output, size_t capacity,
                                 size_t maximumLength) {
    uint16_t length = 0;
    if (!readU16(length)) {
      return OtaManifestParseError::kTruncated;
    }
    if (length == 0 || length > maximumLength ||
        static_cast<size_t>(length) + 1 > capacity) {
      return OtaManifestParseError::kStringLengthOutOfRange;
    }
    if (!has(length)) {
      return OtaManifestParseError::kTruncated;
    }
    for (size_t index = 0; index < length; ++index) {
      if (!canonicalTextByte(bytes_[position_ + index])) {
        return OtaManifestParseError::kNonCanonicalString;
      }
    }
    std::memcpy(output, bytes_ + position_, length);
    output[length] = '\0';
    position_ += length;
    return OtaManifestParseError::kNone;
  }

  OtaManifestParseError readDigest(uint8_t output[kOtaSha256Size]) {
    uint16_t length = 0;
    if (!readU16(length)) {
      return OtaManifestParseError::kTruncated;
    }
    if (length != kOtaSha256Size) {
      return OtaManifestParseError::kDigestLengthMismatch;
    }
    if (!has(kOtaSha256Size)) {
      return OtaManifestParseError::kTruncated;
    }
    std::memcpy(output, bytes_ + position_, kOtaSha256Size);
    position_ += kOtaSha256Size;
    return OtaManifestParseError::kNone;
  }

  size_t position() const { return position_; }

 private:
  bool has(size_t length) const {
    return position_ <= size_ && length <= size_ - position_;
  }

  const uint8_t* bytes_;
  size_t size_;
  size_t position_;
};

OtaManifestParseResult failure(OtaManifestParseError error) {
  return {OtaManifest{}, error};
}

}  // namespace

OtaManifestParseResult parseOtaManifest(const uint8_t* bytes, size_t size) {
  if (bytes == nullptr) {
    return failure(OtaManifestParseError::kNullArgument);
  }
  if (size < kHeaderSize || size > kOtaManifestMaximumSize) {
    return failure(OtaManifestParseError::kLengthOutOfRange);
  }
  if (std::memcmp(bytes, kMagic, sizeof(kMagic)) != 0) {
    return failure(OtaManifestParseError::kBadMagic);
  }
  if (bytes[4] != kOtaManifestFormatVersion) {
    return failure(OtaManifestParseError::kUnsupportedManifestVersion);
  }
  if (bytes[5] != kOtaSignatureFormatVersion) {
    return failure(OtaManifestParseError::kUnsupportedSignatureVersion);
  }
  const uint16_t declaredLength =
      static_cast<uint16_t>(bytes[6]) << 8U | bytes[7];
  if (declaredLength != size) {
    return failure(OtaManifestParseError::kTotalLengthMismatch);
  }

  Reader reader(bytes, size);
  OtaManifest candidate = {};
  candidate.signatureFormatVersion = bytes[5];
  OtaManifestParseError error = reader.readText(
      candidate.hardware, sizeof(candidate.hardware), kOtaHardwareMaximumLength);
  if (error != OtaManifestParseError::kNone) {
    return failure(error);
  }
  error = reader.readText(candidate.firmwareVersion,
                          sizeof(candidate.firmwareVersion),
                          kOtaFirmwareVersionMaximumLength);
  if (error != OtaManifestParseError::kNone) {
    return failure(error);
  }
  if (!reader.readU64(candidate.releaseCounter)) {
    return failure(OtaManifestParseError::kTruncated);
  }
  if (candidate.releaseCounter == 0 ||
      candidate.releaseCounter > kOtaMaximumReleaseCounter) {
    return failure(OtaManifestParseError::kReleaseCounterOutOfRange);
  }
  error = reader.readText(candidate.buildId, sizeof(candidate.buildId),
                          kOtaBuildIdMaximumLength);
  if (error != OtaManifestParseError::kNone) {
    return failure(error);
  }
  error = reader.readText(candidate.signingKeyId,
                          sizeof(candidate.signingKeyId),
                          kOtaSigningKeyIdMaximumLength);
  if (error != OtaManifestParseError::kNone) {
    return failure(error);
  }
  if (!reader.readU32(candidate.firmwareSize)) {
    return failure(OtaManifestParseError::kTruncated);
  }
  if (candidate.firmwareSize == 0 ||
      candidate.firmwareSize > kOtaFirmwareMaximumSize) {
    return failure(OtaManifestParseError::kFirmwareSizeOutOfRange);
  }
  error = reader.readDigest(candidate.firmwareSha256);
  if (error != OtaManifestParseError::kNone) {
    return failure(error);
  }
  if (!reader.readU32(candidate.elfSize)) {
    return failure(OtaManifestParseError::kTruncated);
  }
  if (candidate.elfSize == 0 || candidate.elfSize > kOtaElfMaximumSize) {
    return failure(OtaManifestParseError::kElfSizeOutOfRange);
  }
  error = reader.readDigest(candidate.elfSha256);
  if (error != OtaManifestParseError::kNone) {
    return failure(error);
  }
  if (reader.position() != size) {
    return failure(OtaManifestParseError::kTrailingData);
  }
  return {candidate, OtaManifestParseError::kNone};
}

const char* otaManifestParseErrorName(OtaManifestParseError error) {
  switch (error) {
    case OtaManifestParseError::kNone:
      return "none";
    case OtaManifestParseError::kNullArgument:
      return "null_argument";
    case OtaManifestParseError::kLengthOutOfRange:
      return "length_out_of_range";
    case OtaManifestParseError::kBadMagic:
      return "bad_magic";
    case OtaManifestParseError::kUnsupportedManifestVersion:
      return "unsupported_manifest_version";
    case OtaManifestParseError::kUnsupportedSignatureVersion:
      return "unsupported_signature_version";
    case OtaManifestParseError::kTotalLengthMismatch:
      return "total_length_mismatch";
    case OtaManifestParseError::kTruncated:
      return "truncated";
    case OtaManifestParseError::kStringLengthOutOfRange:
      return "string_length_out_of_range";
    case OtaManifestParseError::kNonCanonicalString:
      return "noncanonical_string";
    case OtaManifestParseError::kReleaseCounterOutOfRange:
      return "release_counter_out_of_range";
    case OtaManifestParseError::kFirmwareSizeOutOfRange:
      return "firmware_size_out_of_range";
    case OtaManifestParseError::kElfSizeOutOfRange:
      return "elf_size_out_of_range";
    case OtaManifestParseError::kDigestLengthMismatch:
      return "digest_length_mismatch";
    case OtaManifestParseError::kTrailingData:
      return "trailing_data";
  }
  return "unknown";
}
