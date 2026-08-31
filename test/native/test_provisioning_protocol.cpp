#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "device_settings.h"
#include "provisioning_protocol.h"

namespace {

constexpr char kChallenge[] = "deadbeef";

std::string encode(const uint8_t* value, size_t length) {
  std::vector<char> output(((length + 2) / 3) * 4 + 1);
  size_t outputLength = 0;
  assert(encodeBase64UrlNoPadding(value, length, output.data(), output.size(),
                                  &outputLength) == Base64UrlError::kOk);
  return std::string(output.data(), outputLength);
}

std::string encode(const std::string& value) {
  return encode(reinterpret_cast<const uint8_t*>(value.data()), value.size());
}

std::string command(const std::string& ssid, const std::string& password,
                    const std::string& baseUrl, const std::string& token,
                    const std::string& challenge = kChallenge) {
  return "PROVISION,SET," + challenge + "," + encode(ssid) + "," +
         encode(password) + "," + encode(baseUrl) + "," + encode(token);
}

std::string commandV2(const std::string& ssid, const std::string& password,
                      const std::string& baseUrl, const std::string& token,
                      const std::string& otaSecret,
                      const std::string& challenge = kChallenge) {
  return command(ssid, password, baseUrl, token, challenge) + "," +
         encode(otaSecret);
}

ProvisioningError parse(const std::string& value, DeviceSettings* settings) {
  return parseProvisioningSetCommand(value.data(), value.size(), kChallenge,
                                     sizeof(kChallenge) - 1, settings);
}

void expectSettingsUnchanged(const DeviceSettings& settings) {
  assert(std::strcmp(settings.ssid, "unchanged") == 0);
}

}  // namespace

int main() {
  {
    std::vector<uint8_t> original(256);
    for (size_t index = 0; index < original.size(); ++index) {
      original[index] = static_cast<uint8_t>(index);
    }
    const std::string encoded = encode(original.data(), original.size());
    std::vector<uint8_t> decoded(original.size());
    size_t decodedLength = 0;
    assert(decodeBase64UrlNoPadding(encoded.data(), encoded.size(),
                                    decoded.data(), decoded.size(),
                                    &decodedLength) == Base64UrlError::kOk);
    assert(decodedLength == original.size());
    assert(decoded == original);
  }

  {
    DeviceSettings settings;
    const std::string value = command("Lab WiFi", "correct horse battery staple",
                                      "http://192.168.0.46:8081/", "secret");
    assert(parse(value, &settings) == ProvisioningError::kOk);
    assert(std::strcmp(settings.ssid, "Lab WiFi") == 0);
    assert(std::strcmp(settings.password, "correct horse battery staple") == 0);
    assert(std::strcmp(settings.baseUrl, "http://192.168.0.46:8081") == 0);
    assert(std::strcmp(settings.token, "secret") == 0);
    assert(settings.otaSecret[0] == '\0');
  }

  {
    DeviceSettings settings;
    const std::string otaSecret(DeviceSettings::kOtaSecretBytes, 'A');
    const std::string value =
        commandV2("Lab WiFi", "password", "http://192.168.0.46:8081",
                  "secret", otaSecret);
    assert(parse(value, &settings) == ProvisioningError::kOk);
    assert(std::strcmp(settings.otaSecret, otaSecret.c_str()) == 0);
    assert(parse(commandV2("Lab WiFi", "password",
                                  "http://192.168.0.46:8081", "secret",
                                  std::string(42, 'A')),
                 &settings) == ProvisioningError::kInvalidOtaSecret);
    assert(parse(commandV2("Lab WiFi", "password",
                                  "http://192.168.0.46:8081", "secret",
                                  std::string(43, '+')),
                 &settings) == ProvisioningError::kInvalidOtaSecret);
  }

  {
    DeviceSettings settings;
    const std::string value =
        command("Open Network", "", "http://example.test/presence///",
                "secret");
    assert(parse(value, &settings) == ProvisioningError::kOk);
    assert(settings.password[0] == '\0');
    assert(std::strcmp(settings.token, "secret") == 0);
    assert(std::strcmp(settings.baseUrl, "http://example.test/presence") == 0);
  }

  {
    DeviceSettings settings;
    assert(parse(command("ssid", "", "http://192.168.0.46:8081", ""),
                 &settings) == ProvisioningError::kInvalidToken);
    assert(parse(command("ssid", "password", "https://example.test", "token"),
                 &settings) == ProvisioningError::kInvalidBaseUrl);
  }

  {
    DeviceSettings settings;
    std::strcpy(settings.ssid, "unchanged");
    const std::string value = command("ssid", "password",
                                      "http://192.168.0.46:8081", "token",
                                      "feedface");
    assert(parse(value, &settings) == ProvisioningError::kInvalidChallenge);
    expectSettingsUnchanged(settings);
  }

  {
    DeviceSettings settings;
    std::strcpy(settings.ssid, "unchanged");
    std::string value = command("ssid", "password",
                                "http://192.168.0.46:8081", "token");
    const size_t firstEncodedField = value.find(',', value.find(',', 10) + 1) + 1;
    value[firstEncodedField] = '=';
    assert(parse(value, &settings) == ProvisioningError::kInvalidBase64Url);
    expectSettingsUnchanged(settings);
  }

  {
    DeviceSettings settings;
    const std::string value = command(std::string(33, 's'), "password",
                                      "http://192.168.0.46:8081", "token");
    assert(parse(value, &settings) ==
           ProvisioningError::kDecodedFieldTooLong);
  }

  {
    DeviceSettings settings;
    const std::string maxBaseUrl = "http://h/" + std::string(119, 'a');
    assert(maxBaseUrl.size() == DeviceSettings::kMaxBaseUrlBytes);
    const std::string value =
        command(std::string(DeviceSettings::kMaxSsidBytes, 's'),
                std::string(DeviceSettings::kMaxPasswordBytes, 'p'), maxBaseUrl,
                std::string(DeviceSettings::kMaxTokenBytes, 't'));
    assert(parse(value, &settings) == ProvisioningError::kOk);
    assert(std::strlen(settings.ssid) == DeviceSettings::kMaxSsidBytes);
    assert(std::strlen(settings.password) ==
           DeviceSettings::kMaxPasswordBytes);
    assert(std::strlen(settings.baseUrl) == DeviceSettings::kMaxBaseUrlBytes);
    assert(std::strlen(settings.token) == DeviceSettings::kMaxTokenBytes);
  }

  {
    DeviceSettings settings;
    const std::string value =
        command("ssid", "password", "http://192.168.0.46:8081",
                std::string(DeviceSettings::kMaxTokenBytes + 1, 't'));
    assert(parse(value, &settings) ==
           ProvisioningError::kDecodedFieldTooLong);
  }

  {
    const uint8_t ssidWithNul[] = {'a', '\0', 'b'};
    const std::string value = "PROVISION,SET," + std::string(kChallenge) + "," +
                              encode(ssidWithNul, sizeof(ssidWithNul)) + ",," +
                              encode("http://192.168.0.46:8081") + "," +
                              encode("token");
    DeviceSettings settings;
    assert(parse(value, &settings) == ProvisioningError::kEmbeddedNul);
  }

  {
    const char* invalidUrls[] = {
        "ftp://192.168.0.46:8081",
        "http://user@192.168.0.46:8081",
        "http://192.168.0.46:8081/path?query=1",
        "http://192.168.0.46:8081/path#fragment",
        "http:///missing-host",
    };
    for (const char* url : invalidUrls) {
      DeviceSettings settings;
      assert(parse(command("ssid", "password", url, "token"), &settings) ==
             ProvisioningError::kInvalidBaseUrl);
    }
  }

  {
    DeviceSettings settings;
    const std::string value = command("ssid", "password",
                                      "http://192.168.0.46:8081", "token") +
                              ",extra,extra";
    assert(parse(value, &settings) == ProvisioningError::kInvalidFieldCount);
  }

  {
    DeviceSettings settings;
    std::string value =
        command("ssid", "password", "http://192.168.0.46:8081", "token");
    value.insert(value.begin() + 4, '\0');
    assert(parse(value, &settings) == ProvisioningError::kEmbeddedNul);
  }

  {
    uint8_t decoded[4] = {};
    size_t decodedLength = 0;
    assert(decodeBase64UrlNoPadding("AB", 2, decoded, sizeof(decoded),
                                    &decodedLength) ==
           Base64UrlError::kNonCanonicalTrailingBits);
  }

  return 0;
}
