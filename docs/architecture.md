# Architecture

## Runtime topology

The project is one Git repository with three separately deployed parts:

1. The M5GO firmware owns sensor sampling, presence decisions, display power,
   rendering, a bounded telemetry queue, and eventual offline spooling.
2. The presence API on `devb` owns durable telemetry, feedback, configuration,
   health, control commands, signed releases, operational logs, and sanitized
   crash diagnostics. Its same-origin Web Console is an operator view over
   those APIs; it does not create a second backend or database.
3. The existing environmental sensor API remains a separate service on `devb`
   while the presence API is introduced.

Initial ports on `devb`:

- Environmental sensor API: `192.168.0.46:8080` and
  `100.117.242.46:8080`.
- Presence API: reserve `192.168.0.46:8081` and
  `100.117.242.46:8081`.

The M5GO uses the LAN address. Tailnet clients use the Tailscale address. Both
services bind only those two explicit addresses, not every interface.

The display reads the environmental API directly over the LAN and reads the
public Open-Meteo forecast directly. Both requests run serially in the same
Core 0 worker that owns Wi-Fi. A POD mailbox publishes complete display-data
snapshots to the hardware loop; HTTP, JSON parsing, and retry logic never run in
the sampling/rendering loop. Weather and room temperatures use Celsius end to
end. The same mailbox latches successful SNTP synchronization before the main
loop renders the clock with the POSIX `America/New_York` EST/EDT rules. The
clock uses an epoch-second equality token and a dedicated 32-pixel sprite, so
each second updates only the header while full dashboard redraws remain tied to
state/data changes and a recovery heartbeat. Footer ages are fetch freshness,
not upstream observation timestamps.

## Telemetry flow

The hardware loop never performs network or flash I/O. Once per second it
publishes an immutable aggregate sample to a bounded queue; state transitions
publish immediately. A lower-priority worker batches records and uploads them.
All authenticated port-8081 operations share that worker's single serialized
HTTP/1.1 keep-alive session. Successful responses are completely consumed
before reuse; any non-success or partial response closes the connection, and
every later request explicitly restores keep-alive mode. Telemetry request
bodies are copied from LittleFS into bounded RAM chunks with the file closed
before each socket write, so flash and TCP progress do not hold one another's
resources. This keeps the five-second control poll from exhausting the ESP32's
bounded TCP PCB pool with HTTP/1.0 `TIME_WAIT` connections.

Microphone electrical/timing health is also hysteretic: three consecutive bad
sample windows are required to enter `degraded`; eight consecutive bad windows
ending in a hard-fault classification enter `fault`; and 24 healthy windows
recover. One Wi-Fi interrupt or scheduler outlier therefore cannot generate a
health/log feedback loop, while persistent sensor faults remain visible
quickly.

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
- Records are removed only after the local request body is sent completely,
  HTTP 200, matching response `batch_id`,
  `stored + duplicates == request.records.length`, and matching `max_seq`.
- Network failures, timeouts, HTTP 429, and HTTP 5xx use bounded exponential
  backoff with jitter. HTTP 401 or an endpoint 404 halts uploads globally and
  leaves the exact envelope in the retry spool; USB reprovisioning plus restart
  retries it with corrected connection settings. Payload-specific HTTP 409 and
  422 responses move only that envelope to the diagnostic dead-letter queue.
- Device-originated button feedback is uploaded only after the telemetry record
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
- A button correction first snapshots the pre-button state into a priority
  telemetry record and atomically queues feedback referencing that record.
  Button-wake evidence may then update the live UI immediately, but the network
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
feedback, configuration, bounded console snapshots, pagination envelopes,
errors, bearer auth, and the Console's LAN-only routes.

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

The console has a deliberately different, bounded analytics query: 15 minutes
through 24 hours, downsampled to at most 2,000 chart points. Its event timeline
uses `COALESCE(observed_at_ms, received_at_ms)`. The equivalent window predicate
and latest-event ordering use a matching composite SQLite expression index.
Transition and feedback markers use the same event-time rule as chart buckets.

Connectivity is server-observed. A device is online only when its newest
telemetry record or health report was received within 120 seconds;
`devices.last_seen_at_ms` is not used because configuration operations also
update that administrative field. The separately reported local health level
cannot claim backend connectivity. Calibration aggregates and feedback totals
cover the complete selected window even when the visible marker list is capped.

## Device identity

`device_id` is derived once from the ESP32 eFuse base MAC, formatted as
`core2-` followed by twelve lowercase hexadecimal digits. It is independent of
Wi-Fi credentials, IP address, USB serial enumeration, firmware version, and
display settings. Firmware updates and factory Wi-Fi reprovisioning must not
change it, because it is part of every idempotency and history key.

The `core2-` prefix is a legacy v1 wire identifier assigned before the physical
unit was correctly identified as an M5GO. It is intentionally preserved; a
cosmetic rename would create a different logical device and split history.

## Data ownership

- Raw microphone audio never leaves the device and is never stored.
- NVS is reserved for low-frequency settings such as Wi-Fi credentials, API
  token, per-device development-OTA secret, release counter, and the latest
  applied configuration revision. Provisioning writes an inactive settings
  slot completely before atomically switching the active-slot marker,
  preserving the previous valid configuration across interrupted updates.
- SQLite on `devb` is the authoritative history.
- A daily systemd timer creates consistent SQLite online-backup snapshots,
  validates them before atomic publication, and rotates daily and weekly
  generations independently in a root-managed state directory that the API's
  dynamic user cannot modify. These local snapshots protect against migration
  and operator mistakes; they do not protect against loss of the devb disk.
- The M5GO's internal flash is only an outage spool; it is not the primary
  database.
- Sensor samples and transitions are append-only. Configuration and human
  feedback are the only user-editable domains.

## Database migration and recovery

Database schema v3 added nullable `applied_config_revision` to every telemetry
record. New records store the batch revision; existing schema-v2 rows migrate
to `NULL` because their historical value cannot be reconstructed.

v0.7 migrates additively through schema v4 to schema v5. Schema v4 adds health,
control, release, operational-log, and core-dump storage plus transition
evidence. Schema v5 persists each device's monotonic confirmed-release floor
and the immutable completion outcome of every release-status report. SQLite
uses WAL with `synchronous=FULL`, so a successful core-dump response is not sent
until the transaction whose durability lets the device erase its copy has
committed.

Deployment takes and validates an online `pre-schema-v5` backup before the
service restart. Rolling code back across this boundary also restores that
snapshot because older application versions reject a newer
`PRAGMA user_version`.

## OTA and control flow

The device initiates every connection. It polls the bearer-authenticated
control endpoint for desired state and at most one leased command; devb never
opens a connection to the M5GO. Command acceptance is persisted before a side
effect, retries reuse the same acknowledgement identity, and nonterminal
commands are never removed by history retention.

Production OTA streams only the inactive application slot. Before writing, the
firmware validates the fixed hardware identifier, monotonic counter, canonical
manifest, and ECDSA P-256 signature against its compiled current/next public
keys. It verifies the final byte count and SHA-256, then leaves the image in
pending-verification state until 30 seconds of healthy local runtime. Wi-Fi or
devb loss is a soft condition; failed local boot gates request bootloader
rollback. Development OTA is separately protected by a per-device secret and
exists only in a physically opened 120-second LAN window.

## Hardware invariants

The following validated behavior must survive every firmware refactor:

- Classic M5Stack/M5GO board target with 16 MB flash and no PSRAM.
- Digital PIR on input-only GPIO36 at PORT.B.
- Analog microphone on ADC1 GPIO34, sampled directly at 8 kHz in 128-sample
  windows so it remains compatible with Wi-Fi.
- Unused speaker/DAC output GPIO25 held low while sampling the microphone.
- Physical buttons A/B/C on GPIO39/38/37.
- Internal-RAM 8-bit display canvas with the partial-update fallback.

## Transport security boundary

The first deployed device transport is plain HTTP to the explicitly bound
`192.168.0.46:8081` trusted-LAN endpoint. The ordinary device API requires a
bearer token, but HTTP does not encrypt credentials or telemetry. The Web
Console deliberately uses a different boundary: its shell, data reads, and
confirmed configuration writes require the direct socket peer to be in
`192.168.0.0/24`, never trusts forwarding headers, and accepts only the
configured literal LAN Host. Browser mutations additionally require an exact
same-origin Origin header, closing the DNS-rebinding path. Every host on that
LAN is therefore a Console administrator. The service must not be exposed to
an untrusted LAN or public interface. HTTPS is not accepted by provisioning
until the firmware has a real certificate trust configuration; it never falls
back to an insecure TLS client.

The two display feeds are credential-free GETs and never receive the presence
Bearer token. The LAN environment feed and first Open-Meteo integration also
use plain HTTP, so their values have no transport-integrity guarantee; a later
TLS trust-store change can harden weather transport without changing telemetry
or provisioning semantics.

USB provisioning also treats the attached host and serial endpoint as trusted.
Its short-lived challenge supplies freshness and transaction correlation, not
proof of physical presence. A future hardened flow must gate configured-device
replacement on an M5GO physical-button hold and confirm the displayed identity
before the host sends credentials.
