# M5GO Presence Platform

A presence-aware desk display built around an M5Stack M5GO IoT Kit. The
firmware combines the PORT.B PIR and M5GO Base analog microphone to control the
LCD, displays weather and room conditions, and uploads durable telemetry for
later calibration.

## Repository layout

- `src/` and `platformio.ini`: M5GO firmware.
- `backend/`: FastAPI + SQLite presence telemetry service.
- `contracts/`: versioned device/server payload schemas.
- `docs/`: architecture and hardware invariants.
- `deploy/devb/`: explicit dual-address Uvicorn launchers and systemd files.

The existing environmental sensor API stays on port 8080. The new presence API
uses port 8081 so both services can be deployed and rolled back independently.

## Presence API

The v1 backend accepts append-only sample and state-transition batches, stores
human feedback, and serves revisioned device configuration. Retries are
idempotent on `(device_id, boot_id, seq)`; conflicting reuse of an identity is
rejected instead of silently replacing history.

```sh
make backend-venv
make backend-test
make backend-run
```

See `contracts/telemetry-v1.schema.json`, `contracts/api-v1.openapi.json`, and
`docs/architecture.md` for the generated device/server contracts and runtime
boundaries.

## Firmware telemetry pipeline

The hardware loop uses a 64-bit monotonic clock, a stable device ID derived
from the ESP32 eFuse MAC, and a new random 128-bit boot ID on each startup.
Once per second it places an immutable sensor snapshot in a fixed-size,
cross-core-safe RAM queue; state transitions are queued immediately and have
reserved capacity so ordinary samples cannot crowd them out. Sequence numbers
are never reused, even when a full queue drops a record.

A low-priority Core 0 worker freezes batches into LittleFS before attempting
network I/O. It retries the exact immutable file, validates every field of the
server acknowledgement, and deletes only the specifically acknowledged file.
The worker uses bounded backoff and keeps old-boot envelopes under their
original boot ID and clock anchor. Sensor sampling and display rendering never
perform Wi-Fi, HTTP, NVS, or flash I/O.

Wi-Fi, backend URL, and bearer token are provisioned over USB and stored in a
two-slot NVS record so a reset during an update leaves the previous valid
configuration intact:

```sh
python3 tools/provision_core2.py --ssid "Wi-Fi network"
```

The password and token are collected through hidden prompts, never command-line
arguments. See `tools/README.md` for non-interactive input and port selection.
The current uploader supports the explicit trusted-LAN HTTP endpoint only;
HTTP does not encrypt the bearer token, so do not expose port 8081 to an
untrusted LAN or the public internet. TLS certificate validation is a future
transport gate, not something this firmware silently bypasses.

USB provisioning likewise assumes the attached computer and serial device are
trusted. The challenge prevents stale or crossed commands, but it is not a
physical-presence authorization step. Do not provision through an unknown USB
adapter or on a shared/untrusted host.

The worker also fetches revisioned presence settings from the backend. A newer
revision is validated against the device's fixed capacities, written to an
atomic two-slot NVS record, and handed to the hardware loop through a bounded
mailbox. The loop applies the complete snapshot at one boundary and stamps all
subsequent telemetry with that revision; the uploader never mixes revisions in
one batch.

The three physical buttons are A=`PRESENT`, B=`WAKE`, and C=`ABSENT`. A and C
snapshot an immediate pre-button sample with the human label; B only wakes the
display. Each sample/label pair enters one indivisible RAM item, is persisted as
one checksummed LittleFS bundle, and survives restart without changing
identity. The worker acknowledges the referenced telemetry sample before it
posts the feedback, preventing dangling labels. Raw microphone audio is never
uploaded.

The v1 wire contract still calls these corrections `source=touch` and calls a
button wake `touch_wake`. Those legacy names intentionally remain stable so the
existing device history, backend, and idempotency keys do not split during the
hardware-model correction.

## Hardware

- M5Stack M5GO IoT Kit (classic M5Stack core, 16 MB flash, no PSRAM)
- M5Stack digital PIR Unit on PORT.B (signal on GPIO36)
- M5GO Base analog microphone on ADC1 GPIO34

This particular sensor is validated as the digital GPIO36 PIR path. Do not
enable an internal pull resistor on GPIO36: it is input-only and the PIR Unit
provides its own conditioned digital output. The unused base speaker output on
GPIO25 is held low while the microphone is sampled.

## Demo behavior

- First 5 seconds: calibrate the microphone noise floor.
- `IDLE`: screen backlight is off. Sound alone does not wake it.
- PIR motion: immediately enter `PRESENT` and turn the screen on.
- While present: PIR or above-baseline sound extends the on-time.
- After both sensors are quiet: dim for 5 seconds, then turn the backlight off.
- Sound may bridge stationary periods for at most 5 minutes after the latest
  PIR evidence, so a television or fan cannot keep the display on forever.
- Press B to wake the screen without adding a label.
- Press A or C to record a pre-button `PRESENT` or `ABSENT` correction.

No audio is stored or transmitted. The microphone samples are reduced to a
short-window RMS value and smoothed envelope in memory. Sound detection uses a
slowly learned room-noise floor plus hysteresis; the default trigger is `1.12x`
the learned floor.

## Hardware validation on this M5GO

Validated over USB serial on 2026-08-23:

- The digital motion input on GPIO36 changed from `0` to `1`, entered
  `PRESENT`, and raised the LCD backlight to brightness `255`.
- A controlled sound played near the M5GO Base raised the microphone envelope over
  its learned threshold and produced `sound=1`.
- During that sound-only test the state remained `IDLE / SCREEN OFF`, as
  intended. PIR is still required to declare an arrival.
- The firmware builds and flashes successfully for the 16 MB classic M5Stack
  target and reports `board=1`, physical buttons, and no PSRAM.
- A, the GPIO39 physical button, produced durable `present` feedback whose
  evidence telemetry and feedback were both acknowledged by the backend.
- An 8-bit full-screen canvas in internal RAM removes visible partial redraws;
  a low-memory partial-update fallback remains available.

M5Unified 0.2.19's I2S ADC compatibility path returned a constant value and
blocked for roughly half a second on this runtime. The firmware therefore polls
the M5GO Base microphone directly through ADC1 GPIO34 at 8 kHz in 128-sample
windows. This path remains live with Wi-Fi enabled and produced repeatable
sound events in the controlled test.

The microphone is deliberately treated as weak supporting evidence rather than
a second occupancy sensor: it can extend a PIR-confirmed presence window but it
cannot wake the display on its own.

## Display data

The production dashboard reads two low-frequency sources in the existing Core
0 network worker; sensor sampling and drawing never perform HTTP or JSON work:

- Room conditions every 30 seconds from
  `http://192.168.0.46:8080/v1/metrics`.
- Current and daily Blacksburg weather about every 15 minutes from Open-Meteo,
  using `37.2296,-80.4139`.

The room API's `OUT_DATED` status is shown as stale while preserving its last
values. Network or parse failures likewise preserve the last successful
snapshot. Rain and snow remain distinct forecast fields. Weather data is
provided by [Open-Meteo](https://open-meteo.com/). All room and forecast
temperatures are displayed in Celsius. The header clock is synchronized by
SNTP and rendered to the second in Eastern time with automatic EST/EDT
daylight transitions. Only the 32-pixel header strip is pushed for each clock
tick. The secondary QMP temperature remains parsed as backup data but is not
shown. The footer ages describe the last successful HTTP fetch for each source,
not the age of the underlying sensor observation or forecast.

## Build, flash, and monitor

All dependencies are kept inside this directory.

```sh
make firmware-unit
make tools-test
PLATFORMIO_CORE_DIR="$PWD/.platformio" ./.venv/bin/pio run
PLATFORMIO_CORE_DIR="$PWD/.platformio" ./.venv/bin/pio run --target upload
PLATFORMIO_CORE_DIR="$PWD/.platformio" ./.venv/bin/pio device monitor
```

Serial output is CSV-like and includes PIR, microphone RMS, learned noise floor,
trigger threshold, sound activity, presence state, and screen brightness.

## devb deployment helpers

`deploy/devb/serve_env_api.py` runs the existing environmental sensor FastAPI
application as one Uvicorn process with two explicitly bound sockets:
`100.117.242.46:8080` and `192.168.0.46:8080`. The accompanying systemd drop-in
switches `env-api.service` to that launcher without adding a reverse proxy or
starting a second sampler process.

## Existing firmware backup

Before this project firmware was flashed, the original 4MB used region was
saved under a legacy filename created before the hardware was identified:

```text
backups/core2-original-20260823.bin
SHA-256: 82cd216d92947f47d4a7766393c60f90ec02b955452cde46bf0ec70496a3e40e
```

The detected partition table ends at `0x400000`; the remaining 12MB of the 16MB
flash was outside the old firmware's partition map. Restoring the backup is a
separate destructive operation and is intentionally not automated.
