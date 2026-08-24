# Architecture

## Runtime topology

The project is one Git repository with three separately deployed parts:

1. The Core2 firmware owns sensor sampling, presence decisions, display power,
   rendering, a bounded telemetry queue, and eventual offline spooling.
2. The presence API on `devb` owns durable telemetry, feedback, configuration,
   and later the display-data aggregation endpoint.
3. The existing environmental sensor API remains a separate service on `devb`
   while the presence API is introduced.

Initial ports on `devb`:

- Environmental sensor API: `192.168.0.46:8080` and
  `100.117.242.46:8080`.
- Presence API: reserve `192.168.0.46:8081` and
  `100.117.242.46:8081`.

The Core2 uses the LAN address. Tailnet clients use the Tailscale address. Both
services bind only those two explicit addresses, not every interface.

## Telemetry flow

The hardware loop never performs network or flash I/O. Once per second it
publishes an immutable aggregate sample to a bounded queue; state transitions
publish immediately. A lower-priority worker batches records and uploads them.

Every boot gets a random `boot_id`. Every sample and transition within that boot
gets a single increasing `seq`. The backend identity is therefore
`(device_id, boot_id, seq)`, making retries safe even when batch boundaries
change.

The client deletes queued records only after receiving a successful response
that passes every acknowledgement check below. A conflicting payload for an
existing record identity is an HTTP 409 rather than a silent overwrite.

### Upload acknowledgement and retry

- A queued batch is immutable. Its `batch_id`, records, clock anchor, firmware
  version, and applied config revision are persisted as one envelope and must be
  replayed byte-for-byte-equivalently. Any envelope change requires a new
  `batch_id`.
- Records from different `boot_id` values are never mixed in one batch. Every
  flash spool segment stores its original `boot_id` and optional clock anchor;
  after reboot it must not inherit the new boot's identity or clock.
- Records are removed only after HTTP 200, matching response `batch_id`,
  `stored + duplicates == request.records.length`, and matching `max_seq`.
- Network failures, timeouts, HTTP 429, and HTTP 5xx use bounded exponential
  backoff with jitter. HTTP 401 or an endpoint 404 halts uploads globally and
  leaves the exact envelope in the retry spool; USB reprovisioning plus restart
  retries it with corrected connection settings. Payload-specific HTTP 409 and
  422 responses move only that envelope to the diagnostic dead-letter queue.
- Device-originated touch feedback is uploaded only after the telemetry record
  it references has been acknowledged. This preserves its original uptime and
  reconstructed event time without accepting dangling calibration labels.
- A feedback envelope has one durable, immutable `feedback_id` and payload.
  Lost responses replay the same envelope. It is removed only after HTTP 200
  with a matching response `feedback_id`. A 401 keeps the immutable envelope
  and halts all uploads until credentials are corrected. Feedback-specific 404,
  409, and 422 responses move that envelope to the bounded diagnostic/dead-letter
  path. A feedback 404 indicates an uploader ordering or restored-state fault
  because normal flow waits for the referenced telemetry ACK first.

### Firmware integration gates

- Sample, transition, and clock-anchor timestamps share one 64-bit monotonic
  clock (`esp_timer_get_time() / 1000` or one centralized `millis()` wrap
  extender). Raw 32-bit `millis()` is not a valid `uptime_ms` implementation.
- ACK cleanup removes only the exact immutable envelope whose `batch_id` was
  acknowledged. `max_seq` is a response cross-check, never a cumulative delete
  watermark, because envelopes may be retried out of order.
- A touch correction first snapshots the pre-touch state into a priority
  telemetry record and atomically queues feedback referencing that record.
  Touch-wake evidence may then update the live UI immediately, but the network
  worker waits for the referenced record's ACK before posting the feedback.
  Thus the gesture cannot contaminate its own human label or produce a 404.
- Applying a new configuration closes the current batch. One envelope never
  mixes records produced under different `applied_config_revision` values.
- A device never silently rolls back a durably applied configuration. If the
  server reports a lower revision, or different settings under the same
  revision, uploads halt with immutable spools preserved for operator recovery;
  otherwise valid data is not converted into a stream of revision-conflict
  dead letters. Accordingly, the first telemetry HTTP 409 holds the exact
  envelope and forces a configuration revalidation. Only the same batch's
  repeated 409 after a consistent server revision is confirmed is treated as a
  payload conflict.
- Payload-specific permanent-error envelopes move to a bounded
  dead-letter/diagnostic queue so one HTTP 409 or 422 cannot block later valid
  telemetry. Credential-wide 401 and endpoint-wide 404 errors halt instead of
  dead-lettering every otherwise valid envelope.

`presence_api.schemas.TelemetryBatch` is the single executable contract used by
the API. `contracts/telemetry-v1.schema.json` is generated from that model and
is its language-neutral structural projection; CI-style checks fail if the
artifact drifts. JSON Schema cannot express comparison between `mic_min` and
`mic_max`, or uniqueness of a nested `seq` property across array items. Those
two normative rules are listed in the artifact's `x-semantic-constraints`; a
shared validation corpus proves that the executable contract and API reject
the same semantic violations.

`contracts/api-v1.openapi.json` is generated from the FastAPI application and
versions the rest of the HTTP contract, including acknowledgement responses,
feedback, configuration, pagination envelopes, errors, and bearer auth.

## Time model

`uptime_ms` is always required and must be represented as an extended 64-bit
monotonic value in the firmware. When SNTP or the RTC provides trusted UTC, the
device includes a clock anchor pairing UTC milliseconds with uptime
milliseconds. The backend reconstructs record time from that anchor.

Without an anchor, the backend preserves ingestion order and server receive
time. It does not invent a device timestamp. A later anchor for the same boot
can backfill previously received records.

The first accepted clock anchor for a boot is immutable. Later batches may
repeat exactly that anchor, but a different anchor receives HTTP 409. This keeps
offline replay from silently splitting one boot into incompatible timelines.

## Query model

Sample, transition, and feedback queries return
`{items, truncated, next_cursor}` envelopes. When `truncated` is true, clients
repeat the same time window and limit with the opaque `next_cursor`. Telemetry
windows and cursor order use immutable server `received_at_ms` plus
`(boot_id, seq)`, so a late clock-anchor backfill cannot reorder an in-progress
scan and records sharing one millisecond are neither repeated forever nor
skipped. Returned `observed_at_ms` remains the reconstructed event time for
analysis. Feedback uses immutable `(created_at_ms, feedback_id)` ordering. A
bare list is never presented as a complete history when the requested limit was
reached.

## Device identity

`device_id` is derived once from the ESP32 eFuse base MAC, formatted as
`core2-` followed by twelve lowercase hexadecimal digits. It is independent of
Wi-Fi credentials, IP address, USB serial enumeration, firmware version, and
display settings. Firmware updates and factory Wi-Fi reprovisioning must not
change it, because it is part of every idempotency and history key.

## Data ownership

- Raw microphone audio never leaves the device and is never stored.
- NVS is reserved for low-frequency settings such as Wi-Fi credentials, API
  token, and the latest applied configuration revision. Provisioning writes an
  inactive settings slot completely before atomically switching the active-slot
  marker, preserving the previous valid configuration across interrupted
  updates.
- SQLite on `devb` is the authoritative history.
- The Core2's internal flash is only an outage spool; it is not the primary
  database.
- Sensor samples and transitions are append-only. Configuration and human
  feedback are the only user-editable domains.

## Hardware invariants

The following validated behavior must survive every firmware refactor:

- Explicit AXP2101 display rail enablement (`ALDO4`, `ALDO2`, `BLDO1`).
- No remapping of I2C controller 1 away from internal GPIO21/22.
- Raw I2S0 PDM microphone on clock GPIO0 and data GPIO34 at 44.1 kHz.
- Stop the shared I2S speaker before starting the microphone.
- PSRAM-backed 8-bit display canvas with the partial-update fallback.
- Digital PIR on GPIO36.

## Transport security boundary

The first deployed device transport is plain HTTP to the explicitly bound
`192.168.0.46:8081` trusted-LAN endpoint. Bearer authentication prevents
unauthenticated API use but does not encrypt credentials or telemetry. The
service must not be exposed to an untrusted LAN or public interface. HTTPS is
not accepted by provisioning until the firmware has a real certificate trust
configuration; it never falls back to an insecure TLS client.

USB provisioning also treats the attached host and serial endpoint as trusted.
Its short-lived challenge supplies freshness and transaction correlation, not
proof of physical presence. A future hardened flow must gate configured-device
replacement on a Core2 touch/long-press and confirm the displayed identity
before the host sends credentials.
