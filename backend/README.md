# Presence API

FastAPI + SQLite service for M5GO presence telemetry, state transitions,
human feedback, versioned device configuration, health, remote control,
signed OTA releases, operational logs, and crash diagnostics.

## Local development

From the repository root:

```sh
make backend-venv PYTHON=/opt/homebrew/bin/python3.12  # macOS used here
make backend-test
make backend-run
```

On devb, install only runtime dependencies with
`make backend-install PYTHON=python3`.

The default database is `data/presence.db`. Override it with
`PRESENCE_DB_PATH`. Set `PRESENCE_API_TOKEN` to require
`Authorization: Bearer ...` on the device API under `/v1/devices/...`.
The devb systemd unit loads the same token from
`/etc/m5-presence.token` as a systemd credential, keeping it out of the
repository and process command line. A credential takes precedence over an
environment token so key rotation cannot be masked by stale environment state.
Production sets `PRESENCE_REQUIRE_TOKEN=1`, so a missing or empty credential
causes startup to fail rather than silently exposing write endpoints.

`presence_api.schemas.TelemetryBatch` is the executable telemetry contract and
the checked-in JSON Schema is generated from it. The complete HTTP surface,
including ingest responses, feedback, configuration, pagination, and auth, is
captured in `contracts/api-v1.openapi.json`. `make contract-generate` updates
both artifacts; `make contract-check` fails if either has drifted.

The production service will use port 8081 so the existing environmental sensor
API can remain on port 8080.

## Presence Console

Open `/console` on the same origin as the API, for example
`http://192.168.0.46:8081/console`, or from the configured Mac at
`http://100.117.242.46:8081/console`. It loads directly without a login or
bearer token. The shell, assets, bounded data endpoints, and dedicated
configuration write endpoint accept clients in `192.168.0.0/24`, plus one
exact Tailnet client configured by `PRESENCE_CONSOLE_TAILNET_CLIENT`; all other
sources receive HTTP 403. The server uses the direct socket peer address and
ignores forwarding headers; no reverse proxy is part of this trust boundary.
The LAN and Tailnet sources must respectively use `PRESENCE_CONSOLE_HOST` and
`PRESENCE_CONSOLE_TAILNET_HOST` in the HTTP Host header, and state-changing
browser requests need a matching same-origin Origin header. The ordinary
`/v1/devices/...` endpoints remain bearer-protected.

The console's online indicator means the server received telemetry or a health
snapshot within the last 120 seconds. Configuration reads or writes do not
refresh that signal.
Charts use reconstructed `observed_at_ms` when available and fall back to
immutable server `received_at_ms` for unanchored data. Queries are capped at a
24-hour window and downsampled to a bounded number of chart points.

Configuration changes show a field-by-field review dialog and require a second
confirmation before issuing a revision. The API still enforces optimistic
concurrency through `base_revision`; an intervening update returns HTTP 409 and
the console reloads the current values.

## v0.7 operations

The device posts full health snapshots to `/v1/devices/{device_id}/health`,
structured log batches to `/logs/batches`, and authenticated core dumps to
`/coredumps`. It polls `/control` every five seconds, acknowledges a leased
one-shot command at `/control/acks`, and reports OTA progress at
`/control/release-status`. These routes inherit the device bearer-token
requirement. Health history, commands, logs, release status, and dumps have
per-device retention limits so the SQLite database remains bounded.

Release import and selection are Console-only trusted-network operations.
Configure the backend trust set with `PRESENCE_OTA_TRUST_KEYS_PATH`, pointing
to a root-owned, mode-`0644` JSON object whose one or two entries map a signing
key ID to a mode-`0644` P-256 public PEM file. Both contain public material;
the service's systemd `DynamicUser` needs read access while only root may
replace them. The
bundle verifier accepts only the canonical archive and binary record
produced by `tools/ota_release.py`; it never trusts key material supplied by the
upload. A selected device downloads its bearer-protected manifest as the exact
`manifest.bin` bytes followed by the 64-byte raw signature, and its image as raw
`firmware.bin`; both responses use `application/octet-stream`.

Set `PRESENCE_COREDUMP_DECODER` to the absolute `esp-coredump` executable when
server-side symbolization is available. Verified OTA bundles retain their exact
ELF by build ID. Raw dumps are retained only for authenticated backend
processing: the tokenless Console can retrieve sanitized summaries, never dump
bytes, ELF bytes, or unrestricted decoder output.
