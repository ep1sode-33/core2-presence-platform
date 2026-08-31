# M5GO USB provisioning

`provision_core2.py` writes Wi-Fi and backend credentials over the M5GO USB
serial connection. It automatically looks for
`usbserial-588D0027491`; use `--port` to override that choice.

The script keeps its original filename for compatibility with existing setup
commands.

Install the only runtime dependency outside the repository, then run:

```sh
python3 -m pip install pyserial
python3 tools/provision_core2.py --ssid "Wi-Fi network"
```

The Wi-Fi password and API token are collected with hidden prompts. They are
never accepted as command-line arguments or printed. The backend defaults to
`http://192.168.0.46:8081`; override it with `--base-url` when needed.
Only `http://` URLs are accepted in this release because the firmware has no
certificate trust configuration yet. Use it only on the trusted LAN; bearer
authentication does not encrypt the token.

Provision only from a trusted computer through a trusted USB serial adapter.
The current challenge correlates one fresh transaction but does not authorize
it with a physical-button confirmation, so another local process with serial access is inside
this release's trust boundary.

Provisioning also creates one 32-byte development-OTA secret per device. The
operator copy defaults to
`~/.config/m5go-presence/ota-secrets/<device-id>.secret`; the directory is mode
`0700` and files are mode `0600`. Existing secrets are reused so routine
provisioning does not unexpectedly break OTA access. Use
`--rotate-ota-secret` to deliberately replace the attached device's secret, or
`--ota-secret-store PATH` to select another protected directory. Secret values
are never accepted on the command line or printed.

For non-interactive use, `--secrets-stdin` accepts exactly two lines: the Wi-Fi
password followed by the API token. Pass `--ssid` in this mode. Feed stdin from
a trusted secret manager or another process; do not commit a secrets file or put
secret values in shell arguments.

Run the hardware-independent protocol tests with:

```sh
python3 -m unittest discover -s tools/tests -v
```

## Signed production OTA bundles

`ota_release.py` creates and verifies one immutable ZIP bundle containing, in
this exact order:

1. `firmware.bin`
2. `firmware.elf`
3. `manifest.bin`
4. `manifest.sig`

The tool requires the `openssl` executable but no extra Python crypto package.
It never generates a private key. Supply a P-256 PEM key from a developer or CI
secret store; do not put that key on devb or inside this repository. An encrypted
PEM is supported by naming (not providing the value of) its password environment
variable with `--private-key-pass-env`.

Before building, generate the compile-time trust header from one current public
PEM and, optionally, one next rotation key:

```sh
python3 tools/ota_release.py trust-header \
  --key release-2026-a=/secure/path/m5go-release-public.pem
```

The default output is `src/ota_trust_keys.generated.h`. It is deliberately
ignored by Git because each deployment chooses its trust roots, although the
file contains public material only. Use `--overwrite` only for an intentional
rotation, then perform a clean rebuild so no object compiled against the old
trust set is reused. A production firmware build without this generated header
has an empty trust set and rejects every production OTA manifest; USB serial
and the physically opened development-OTA recovery path remain available.

Build the exact BIN and ELF with the release identity injected into both
artifacts, then use the same values when creating the bundle:

```sh
M5GO_FIRMWARE_VERSION=0.7.0 \
M5GO_BUILD_ID=0123456789abcdef \
make firmware-build
```

```sh
python3 tools/ota_release.py create \
  --firmware-bin .pio/build/m5go/firmware.bin \
  --firmware-elf .pio/build/m5go/firmware.elf \
  --private-key /secure/path/m5go-release-key.pem \
  --output dist/m5go-0.7.0.ota.zip \
  --hardware m5go-classic-esp32-16m \
  --firmware-version 0.7.0 \
  --release-counter 7 \
  --build-id 0123456789abcdef \
  --signing-key-id release-2026-a
```

Before importing the artifact into devb, verify it with the corresponding public
key:

```sh
python3 tools/ota_release.py verify \
  --bundle dist/m5go-0.7.0.ota.zip \
  --public-key /secure/path/m5go-release-public.pem \
  --expected-hardware m5go-classic-esp32-16m
```

The backend import adapter should call the same `verify_bundle()` function (or
the CLI above) before copying an artifact into release storage. It must retain
the complete ZIP unchanged. The verifier rejects extra/duplicate members,
noncanonical manifests and signatures, wrong hardware, stale release counters,
and BIN/ELF length or digest mismatches. Both artifacts must also contain
exactly one versioned firmware identity record. The BIN record, ELF record, and
signed manifest must agree on `m5go-classic-esp32-16m`, firmware version, and
build ID; missing, duplicated, or ambiguous records are rejected before import.

Manifest v1 is a deterministic, length-prefixed binary record in network byte
order. It binds hardware, firmware version, a release counter in
`1..2^63-1`, build ID (maximum 64 ASCII bytes), signing key ID, signature-format
version, and the byte length and SHA-256 digest of both the BIN and ELF. The
signature is canonical low-S ECDSA P-256 over SHA-256 of the complete manifest,
stored as exactly 64 bytes `r || s`. Firmware is limited to the classic M5GO
OTA slot size (`0x640000` bytes), and ELF files are limited to 64 MiB. Use the
`public-key` subcommand to export the 65-byte SEC1 uncompressed public point for
the firmware's current/next-key trust set; it prints public material only.

## Development OTA upload

The original `m5go` environment and the explicit `m5go-serial` alias remain USB
recovery paths. `m5go-ota` uses PlatformIO `espota` only while firmware has
opened the physically confirmed 120-second development window. The first
OTA-capable firmware must therefore still be installed over USB.

Host and per-device secret are injected through the environment. The upload
guard accepts only a literal address in `192.168.0.0/24`. The secret contract is
exactly 32 random bytes encoded as 43 characters of unpadded base64url. Enter it
without placing the value in shell history:

```sh
export M5GO_OTA_HOST=192.168.0.123
read -r -s M5GO_OTA_SECRET
export M5GO_OTA_SECRET
PLATFORMIO_CORE_DIR="$PWD/.platformio" ./.venv/bin/pio run \
  -e m5go-ota --target upload
unset M5GO_OTA_SECRET
```

The v0.7 USB provisioning transaction appends `ota_dev_secret` as a fifth
base64url-encoded settings field and saves it atomically with Wi-Fi/backend
settings. Firmware stores at most 43 characters plus NUL. Rotation takes effect
after the device restarts. Never log or display the secret.
