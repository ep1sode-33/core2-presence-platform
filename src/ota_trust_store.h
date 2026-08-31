#pragma once

#include <cstddef>

#include "ota_release.h"

// Release builds may inject this ignored/generated header. It must define:
//   inline constexpr OtaTrustKey kCompiledOtaTrustKeys[] = {...};
//   inline constexpr size_t kCompiledOtaTrustKeyCount = ...;
// No private key is ever compiled into the device. Source builds without a
// generated trust set deliberately fail closed for production wireless OTA.
#if __has_include("ota_trust_keys.generated.h")
#include "ota_trust_keys.generated.h"
#else
inline constexpr OtaTrustKey kCompiledOtaTrustKeys[1] = {};
inline constexpr size_t kCompiledOtaTrustKeyCount = 0;
#endif

static_assert(kCompiledOtaTrustKeyCount <= kOtaMaximumTrustedKeys,
              "at most two OTA release public keys may be compiled");
