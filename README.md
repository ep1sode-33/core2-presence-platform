# Core2 Presence

A presence-aware desk display built around an M5Stack Core2. The current
firmware is a hardware-validation baseline: PIR input, the built-in microphone,
sensor fusion, LCD power control, and flicker-free rendering have all been
validated before networking and dashboard features are added.

## Repository layout

- `src/` and `platformio.ini`: Core2 firmware.
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

## Expected hardware

- M5Stack Core2
- M5Stack digital PIR Unit on PORT.B (signal on GPIO36)
- The Core2's built-in microphone

This particular sensor is validated as the digital GPIO36 PIR path. Do not
remap ESP32 I2C controller 1 to PORT.A while using this Core2 v1.1: its internal
AXP2101 power-management chip already uses that controller on GPIO21/22, and
remapping it breaks LCD backlight control.

## Demo behavior

- First 5 seconds: calibrate the microphone noise floor.
- First 5 minutes: keep the diagnostic screen on for bench testing while all
  sensor values remain live.
- `IDLE`: screen backlight is off. Sound alone does not wake it.
- PIR motion: immediately enter `PRESENT` and turn the screen on.
- While present: PIR or above-baseline sound extends the on-time.
- After both sensors are quiet: dim for 5 seconds, then turn the backlight off.
- Sound may bridge stationary periods for at most 5 minutes after the latest
  PIR evidence, so a television or fan cannot keep the display on forever.
- Touch anywhere to wake the diagnostics screen.
- Touch the lower-left or lower-right areas to tune microphone sensitivity.

No audio is stored or transmitted. The microphone samples are reduced to a
short-window RMS value and smoothed envelope in memory. Sound detection uses a
slowly learned room-noise floor plus hysteresis; the default trigger is `1.12x`
the learned floor.

## Hardware validation on this Core2

Validated over USB serial on 2026-08-23:

- The digital motion input on GPIO36 changed from `0` to `1`, entered
  `PRESENT`, and raised the LCD backlight to brightness `255`.
- A controlled sound played near the Core2 raised the microphone envelope over
  its learned threshold and produced `sound=1`.
- During that sound-only test the state remained `IDLE / SCREEN OFF`, as
  intended. PIR is still required to declare an arrival.
- The final firmware builds and flashes successfully on the attached
  ESP32-D0WDQ6-V3 Core2.
- The Core2 v1.1 AXP2101 display rails stay powered after the startup splash.
- An 8-bit full-screen canvas in PSRAM removes visible full-screen redraws; a
  partial-update fallback remains available if allocation ever fails.

M5Unified 0.2.19's asynchronous microphone path returned a constant value on
this board during the first hardware test. The demo therefore uses the ESP32's
raw PDM I2S receiver directly on Core2 microphone clock GPIO0 and data GPIO34.
This path produced a repeatable sound event in the controlled test.

The microphone is deliberately treated as weak supporting evidence rather than
a second occupancy sensor: it can extend a PIR-confirmed presence window but it
cannot wake the display on its own.

## Build, flash, and monitor

All dependencies are kept inside this directory.

```sh
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

Before this demo was flashed, the original 4MB used region was saved as:

```text
backups/core2-original-20260823.bin
SHA-256: 82cd216d92947f47d4a7766393c60f90ec02b955452cde46bf0ec70496a3e40e
```

The detected partition table ends at `0x400000`; the remaining 12MB of the 16MB
flash was outside the old firmware's partition map. Restoring the backup is a
separate destructive operation and is intentionally not automated.
