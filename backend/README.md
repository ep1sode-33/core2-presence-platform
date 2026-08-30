# Presence API

FastAPI + SQLite service for M5GO presence telemetry, state transitions,
human feedback, and versioned device configuration.

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
`http://192.168.0.46:8081/console`. It loads directly without a login or bearer
token. The shell, assets, bounded data endpoints, and dedicated configuration
write endpoint all reject clients outside `192.168.0.0/24` with HTTP 403. The
server uses the direct socket peer address and ignores forwarding headers; no
reverse proxy is part of this trust boundary. Consequently, every host on that
LAN can read Console telemetry and issue a confirmed configuration revision.
The ordinary `/v1/devices/...` endpoints remain bearer-protected.

The console's online indicator means the server received telemetry within the
last 120 seconds. Configuration reads or writes do not refresh that signal.
Charts use reconstructed `observed_at_ms` when available and fall back to
immutable server `received_at_ms` for unanchored data. Queries are capped at a
24-hour window and downsampled to a bounded number of chart points.

Configuration changes show a field-by-field review dialog and require a second
confirmation before issuing a revision. The API still enforces optimistic
concurrency through `base_revision`; an intervening update returns HTTP 409 and
the console reloads the current values.
