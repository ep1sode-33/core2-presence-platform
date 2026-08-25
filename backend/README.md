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
`Authorization: Bearer ...` on every endpoint except `/v1/healthz`.
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
