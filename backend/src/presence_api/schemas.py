from __future__ import annotations

import json
from enum import StrEnum
from typing import Annotated, Literal

from pydantic import BaseModel, ConfigDict, Field, model_validator

DEVICE_ID_PATTERN = r"^core2-[0-9a-f]{12}$"
MAX_SIGNED_64 = (1 << 63) - 1
STRUCTURED_FIELD_KEY_MAX_LENGTH = 64
STRUCTURED_FIELD_STRING_MAX_LENGTH = 256
STRUCTURED_FIELDS_MAX_SERIALIZED_BYTES = 4_096


def _validate_structured_fields(
    fields: dict[str, str | int | float | bool | None], field_name: str
) -> None:
    for key, value in fields.items():
        if len(key) > STRUCTURED_FIELD_KEY_MAX_LENGTH:
            raise ValueError(
                f"{field_name} keys must be at most "
                f"{STRUCTURED_FIELD_KEY_MAX_LENGTH} characters"
            )
        if isinstance(value, str) and len(value) > STRUCTURED_FIELD_STRING_MAX_LENGTH:
            raise ValueError(
                f"{field_name} string values must be at most "
                f"{STRUCTURED_FIELD_STRING_MAX_LENGTH} characters"
            )
    encoded = json.dumps(
        fields,
        separators=(",", ":"),
        sort_keys=True,
        ensure_ascii=True,
    ).encode("utf-8")
    if len(encoded) > STRUCTURED_FIELDS_MAX_SERIALIZED_BYTES:
        raise ValueError(
            f"{field_name} must serialize to at most "
            f"{STRUCTURED_FIELDS_MAX_SERIALIZED_BYTES} bytes"
        )


class StrictModel(BaseModel):
    model_config = ConfigDict(extra="forbid", allow_inf_nan=False)


class PresenceState(StrEnum):
    calibrating = "calibrating"
    idle = "idle"
    present = "present"
    cooldown = "cooldown"


class TransitionReason(StrEnum):
    boot = "boot"
    calibration_complete = "calibration_complete"
    pir_motion = "pir_motion"
    sound_bridge = "sound_bridge"
    quiet_timeout = "quiet_timeout"
    cooldown_timeout = "cooldown_timeout"
    touch_wake = "touch_wake"
    bench_override = "bench_override"
    config_change = "config_change"
    unknown = "unknown"


class ClockSource(StrEnum):
    sntp = "sntp"
    rtc = "rtc"


class ClockAnchor(StrictModel):
    utc_ms: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)
    uptime_ms: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)
    source: ClockSource


class SampleRecord(StrictModel):
    seq: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)
    kind: Literal["sample"]
    uptime_ms: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)
    pir: bool = Field(strict=True)
    mic_rms: float = Field(strict=True, ge=0)
    mic_envelope: float = Field(strict=True, ge=0)
    mic_min: int = Field(strict=True, ge=-32768, le=32767)
    mic_max: int = Field(strict=True, ge=-32768, le=32767)
    noise_floor: float = Field(strict=True, ge=0)
    sound_threshold: float = Field(strict=True, ge=0)
    sound_active: bool = Field(strict=True)
    state: PresenceState
    brightness: int = Field(strict=True, ge=0, le=255)

    @model_validator(mode="after")
    def validate_sample_range(self) -> SampleRecord:
        if self.mic_min > self.mic_max:
            raise ValueError("mic_min must be <= mic_max")
        return self


class TransitionRecord(StrictModel):
    seq: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)
    kind: Literal["transition"]
    uptime_ms: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)
    from_state: PresenceState | None
    to_state: PresenceState
    reason: TransitionReason
    # Optional only for compatibility with pre-v0.7 batches. v0.7 firmware
    # emits the complete decision-evidence set on every transition.
    pir: bool | None = Field(default=None, strict=True)
    pir_age_ms: int | None = Field(default=None, strict=True, ge=0, le=MAX_SIGNED_64)
    sound_active: bool | None = Field(default=None, strict=True)
    sound_age_ms: int | None = Field(default=None, strict=True, ge=0, le=MAX_SIGNED_64)
    mic_envelope: float | None = Field(default=None, strict=True, ge=0)
    noise_floor: float | None = Field(default=None, strict=True, ge=0)
    sound_threshold: float | None = Field(default=None, strict=True, ge=0)
    brightness_before: int | None = Field(default=None, strict=True, ge=0, le=255)
    brightness_after: int | None = Field(default=None, strict=True, ge=0, le=255)


TelemetryRecord = Annotated[
    SampleRecord | TransitionRecord,
    Field(discriminator="kind"),
]


class TelemetryBatch(StrictModel):
    schema_version: Literal[1]
    batch_id: str = Field(min_length=1, max_length=96, pattern=r"^[A-Za-z0-9._:-]+$")
    boot_id: str = Field(min_length=16, max_length=64, pattern=r"^[A-Za-z0-9_-]+$")
    firmware_version: str = Field(min_length=1, max_length=64)
    build_id: str | None = Field(
        default=None,
        min_length=1,
        max_length=128,
        pattern=r"^[A-Za-z0-9._:+-]+$",
    )
    applied_config_revision: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)
    clock_anchor: ClockAnchor | None = None
    records: list[TelemetryRecord] = Field(min_length=1, max_length=256)

    @model_validator(mode="after")
    def validate_unique_sequences(self) -> TelemetryBatch:
        sequences = [record.seq for record in self.records]
        if len(sequences) != len(set(sequences)):
            raise ValueError("record seq values must be unique within a batch")
        return self


class IngestResponse(StrictModel):
    batch_id: str
    stored: int
    duplicates: int
    max_seq: int
    server_utc_ms: int
    desired_config_revision: int


class SampleOut(StrictModel):
    device_id: str
    boot_id: str
    seq: int
    uptime_ms: int
    observed_at_ms: int | None
    received_at_ms: int
    time_quality: str
    applied_config_revision: int | None
    pir: bool
    mic_rms: float
    mic_envelope: float
    mic_min: int
    mic_max: int
    noise_floor: float
    sound_threshold: float
    sound_active: bool
    state: PresenceState
    brightness: int


class TransitionOut(StrictModel):
    device_id: str
    boot_id: str
    seq: int
    uptime_ms: int
    observed_at_ms: int | None
    received_at_ms: int
    time_quality: str
    applied_config_revision: int | None
    build_id: str | None
    from_state: PresenceState | None
    to_state: PresenceState
    reason: TransitionReason
    pir: bool | None
    pir_age_ms: int | None
    sound_active: bool | None
    sound_age_ms: int | None
    mic_envelope: float | None
    noise_floor: float | None
    sound_threshold: float | None
    brightness_before: int | None
    brightness_after: int | None


class SamplePage(StrictModel):
    items: list[SampleOut]
    truncated: bool
    next_cursor: str | None


class TransitionPage(StrictModel):
    items: list[TransitionOut]
    truncated: bool
    next_cursor: str | None


class ActualPresence(StrEnum):
    present = "present"
    absent = "absent"


class FeedbackSource(StrEnum):
    touch = "touch"
    web = "web"
    api = "api"


class FeedbackCreate(StrictModel):
    feedback_id: str = Field(min_length=8, max_length=64, pattern=r"^[A-Za-z0-9._:-]+$")
    boot_id: str | None = Field(
        default=None, min_length=16, max_length=64, pattern=r"^[A-Za-z0-9_-]+$"
    )
    seq: int | None = Field(default=None, strict=True, ge=0, le=MAX_SIGNED_64)
    actual_presence: ActualPresence
    observed_state: PresenceState | None = None
    source: FeedbackSource
    note: str | None = Field(default=None, max_length=500)

    @model_validator(mode="after")
    def validate_record_reference(self) -> FeedbackCreate:
        if (self.boot_id is None) != (self.seq is None):
            raise ValueError("boot_id and seq must be supplied together")
        if self.source is FeedbackSource.touch and self.boot_id is None:
            raise ValueError("touch feedback must reference a telemetry record")
        return self


class FeedbackResponse(StrictModel):
    feedback_id: str
    device_id: str
    boot_id: str | None
    seq: int | None
    created_at_ms: int
    occurred_at_ms: int | None
    occurred_uptime_ms: int | None
    time_quality: str
    actual_presence: ActualPresence
    observed_state: PresenceState | None
    source: FeedbackSource
    note: str | None
    duplicate: bool = False


class FeedbackPage(StrictModel):
    items: list[FeedbackResponse]
    truncated: bool
    next_cursor: str | None


class PresenceConfig(StrictModel):
    minimum_on_ms: int = Field(strict=True, ge=0, le=600000)
    pir_hold_ms: int = Field(strict=True, ge=1000, le=3600000)
    sound_hold_ms: int = Field(strict=True, ge=0, le=600000)
    max_sound_bridge_ms: int = Field(strict=True, ge=0, le=3600000)
    cooldown_ms: int = Field(strict=True, ge=0, le=600000)
    sound_factor: float = Field(strict=True, ge=1.0, le=4.0)
    telemetry_interval_ms: int = Field(strict=True, ge=250, le=60000)
    # The current device uploader deliberately serializes at most 30 records
    # from a fixed-size worker buffer. Keep the server contract at the same
    # capability ceiling so a device never has to silently clamp a revision.
    upload_batch_size: int = Field(strict=True, ge=1, le=30)


class ConfigPut(StrictModel):
    base_revision: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)
    created_by: str = Field(default="api", min_length=1, max_length=64)
    config: PresenceConfig


class ConfigResponse(StrictModel):
    device_id: str
    revision: int
    created_at_ms: int | None
    created_by: str | None
    config: PresenceConfig


class HealthResponse(StrictModel):
    ok: bool
    database: str
    server_utc_ms: int


class DeviceHealthLevel(StrEnum):
    healthy = "healthy"
    degraded = "degraded"
    action_required = "action_required"
    unknown = "unknown"


class SensorHealthStatus(StrEnum):
    healthy = "healthy"
    degraded = "degraded"
    fault = "fault"
    unknown = "unknown"


class HealthOperationResult(StrEnum):
    unknown = "unknown"
    ok = "ok"
    retrying = "retrying"
    rejected = "rejected"
    error = "error"


class UploaderHealthStatus(StrEnum):
    starting = "starting"
    ready = "ready"
    retrying = "retrying"
    filesystem_unavailable = "filesystem_unavailable"
    operator_halted = "operator_halted"
    task_unavailable = "task_unavailable"


class HealthWifi(StrictModel):
    connected: bool = Field(strict=True)
    ip: str | None = Field(default=None, max_length=45)
    rssi_dbm: int | None = Field(default=None, strict=True, ge=-127, le=0)
    reconnect_count: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)
    clock_synchronized: bool = Field(strict=True)


class HealthConfig(StrictModel):
    desired_revision: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)
    stored_revision: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)
    applied_revision: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)


class HealthFreshness(StrictModel):
    # Every value is an age measured by the device's monotonic clock. ``None``
    # means that operation has not completed during the current boot.
    last_telemetry_ack_ms: int | None = Field(
        default=None, strict=True, ge=0, le=MAX_SIGNED_64
    )
    last_config_attempt_ms: int | None = Field(
        default=None, strict=True, ge=0, le=MAX_SIGNED_64
    )
    last_room_fetch_ms: int | None = Field(
        default=None, strict=True, ge=0, le=MAX_SIGNED_64
    )
    last_weather_fetch_ms: int | None = Field(
        default=None, strict=True, ge=0, le=MAX_SIGNED_64
    )
    telemetry_ack_result: HealthOperationResult
    config_result: HealthOperationResult
    room_fetch_result: HealthOperationResult
    weather_fetch_result: HealthOperationResult


class HealthTasks(StrictModel):
    main_heartbeat_ms: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)
    uploader_heartbeat_ms: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)
    main_stack_hwm: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)
    uploader_stack_hwm: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)


class HealthQueues(StrictModel):
    telemetry_depth: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)
    telemetry_capacity: int = Field(strict=True, ge=1, le=MAX_SIGNED_64)
    dropped_samples: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)
    dropped_critical: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)
    feedback_depth: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)
    feedback_capacity: int = Field(strict=True, ge=1, le=MAX_SIGNED_64)
    feedback_dropped_full: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)
    feedback_rejected_invalid: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)

    @model_validator(mode="after")
    def validate_depths(self) -> HealthQueues:
        if self.telemetry_depth > self.telemetry_capacity:
            raise ValueError("telemetry_depth must not exceed telemetry_capacity")
        if self.feedback_depth > self.feedback_capacity:
            raise ValueError("feedback_depth must not exceed feedback_capacity")
        return self


class HealthStorage(StrictModel):
    filesystem_ready: bool = Field(strict=True)
    spool_files: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)
    feedback_wait_files: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)
    feedback_ready_files: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)
    dead_files: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)
    oldest_backlog_age_ms: int | None = Field(
        default=None, strict=True, ge=0, le=MAX_SIGNED_64
    )
    littlefs_total_bytes: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)
    littlefs_used_bytes: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)
    littlefs_free_bytes: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)

    @model_validator(mode="after")
    def validate_storage_usage(self) -> HealthStorage:
        if self.littlefs_used_bytes > self.littlefs_total_bytes:
            raise ValueError("littlefs_used_bytes must not exceed total bytes")
        if (
            self.littlefs_used_bytes + self.littlefs_free_bytes
            != self.littlefs_total_bytes
        ):
            raise ValueError("LittleFS used and free bytes must sum to total bytes")
        return self


class HealthMemory(StrictModel):
    free_heap_bytes: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)
    min_free_heap_bytes: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)
    largest_free_block_bytes: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)


class HealthSensors(StrictModel):
    pir_status: SensorHealthStatus
    mic_status: SensorHealthStatus
    pir_only_mode: bool = Field(strict=True)


class HealthOta(StrictModel):
    active: bool = Field(strict=True)
    state: str = Field(min_length=1, max_length=32, pattern=r"^[a-z0-9_]+$")


class HealthDebug(StrictModel):
    active: bool = Field(strict=True)
    state: str = Field(min_length=1, max_length=32, pattern=r"^[a-z0-9_]+$")


class DeviceHealthReport(StrictModel):
    schema_version: Literal[1]
    device_id: str = Field(pattern=DEVICE_ID_PATTERN)
    boot_id: str = Field(min_length=16, max_length=64, pattern=r"^[A-Za-z0-9_-]+$")
    firmware_version: str = Field(min_length=1, max_length=64)
    build_id: str = Field(min_length=1, max_length=128, pattern=r"^[A-Za-z0-9._:+-]+$")
    uptime_ms: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)
    sequence: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)
    level: DeviceHealthLevel
    reset_reason: str = Field(min_length=1, max_length=64, pattern=r"^[A-Za-z0-9._-]+$")
    boot_count: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)
    wifi: HealthWifi
    config: HealthConfig
    freshness: HealthFreshness
    tasks: HealthTasks
    queues: HealthQueues
    storage: HealthStorage
    memory: HealthMemory
    sensors: HealthSensors
    uploader_status: UploaderHealthStatus
    ota: HealthOta
    debug: HealthDebug
    safe_mode: bool = Field(strict=True)


class HealthIngestResponse(StrictModel):
    device_id: str
    boot_id: str
    sequence: int
    duplicate: bool
    server_utc_ms: int
    retained_reports: int


class DeviceHealthOut(DeviceHealthReport):
    received_at_ms: int


class DeviceHealthPage(StrictModel):
    latest: DeviceHealthOut | None
    history: list[DeviceHealthOut]
    server_online: bool
    online_threshold_ms: int
    last_activity_at_ms: int | None


class ReleaseBundleImport(StrictModel):
    # Base64 of the canonical ZIP emitted by tools/ota_release.py. The request
    # contains no public key; verification uses only the backend trust set.
    bundle_base64: str = Field(min_length=1, max_length=100 * 1024 * 1024)
    imported_by: str = Field(
        default="presence-console",
        min_length=1,
        max_length=64,
        pattern=r"^[A-Za-z0-9._:+-]+$",
    )


class ReleaseSummary(StrictModel):
    release_id: str
    hardware_model: str
    firmware_version: str
    release_counter: int
    build_id: str
    image_size: int
    image_sha256: str
    elf_size: int
    elf_sha256: str
    key_id: str
    signature_format: Literal["ecdsa-p256-sha256-raw"]
    imported_at_ms: int
    imported_by: str = Field(
        min_length=1, max_length=64, pattern=r"^[A-Za-z0-9._:+-]+$"
    )
    verified: bool


class DesiredRelease(StrictModel):
    # The device never downloads the server-side ELF. Its digest is useful for
    # crash matching, while the ELF byte count stays a Console-only detail.
    release_id: str
    hardware_model: str
    firmware_version: str
    release_counter: int
    build_id: str
    image_size: int
    image_sha256: str
    elf_sha256: str
    key_id: str
    signature_format: Literal["ecdsa-p256-sha256-raw"]
    imported_at_ms: int
    imported_by: str
    verified: bool
    manifest_url: str
    image_url: str


class DeviceReleaseSelection(StrictModel):
    release_id: str = Field(min_length=1, max_length=48, pattern=r"^rel-[0-9a-f]{32}$")
    selected_by: str = Field(default="presence-console", min_length=1, max_length=64)


class DeviceReleaseTargetOut(StrictModel):
    device_id: str
    release: ReleaseSummary
    selected_at_ms: int
    selected_by: str
    completed_at_ms: int | None


class ReleasePhase(StrEnum):
    idle = "idle"
    downloading = "downloading"
    verifying = "verifying"
    reboot_pending = "reboot_pending"
    validating = "validating"
    running = "running"
    failed = "failed"
    rejected = "rejected"
    rolled_back = "rolled_back"


class RollbackOutcome(StrEnum):
    none = "none"
    not_needed = "not_needed"
    succeeded = "succeeded"
    failed = "failed"
    unknown = "unknown"


class ReleaseStatusIn(StrictModel):
    schema_version: Literal[1]
    status_id: str = Field(min_length=8, max_length=64, pattern=r"^[A-Za-z0-9._:-]+$")
    desired_release_id: str | None = Field(
        default=None, max_length=48, pattern=r"^rel-[0-9a-f]{32}$"
    )
    running_release_id: str | None = Field(
        default=None, max_length=48, pattern=r"^rel-[0-9a-f]{32}$"
    )
    previous_release_id: str | None = Field(
        default=None, max_length=48, pattern=r"^rel-[0-9a-f]{32}$"
    )
    last_known_good_release_id: str | None = Field(
        default=None, max_length=48, pattern=r"^rel-[0-9a-f]{32}$"
    )
    phase: ReleasePhase
    progress_percent: int | None = Field(default=None, strict=True, ge=0, le=100)
    last_error: str | None = Field(default=None, max_length=500)
    rollback_outcome: RollbackOutcome = RollbackOutcome.none
    firmware_version: str = Field(min_length=1, max_length=64)
    build_id: str = Field(min_length=1, max_length=128, pattern=r"^[A-Za-z0-9._:+-]+$")


class ReleaseStatusResponse(StrictModel):
    status_id: str
    duplicate: bool
    server_utc_ms: int
    desired_release_completed: bool


class ReleaseStatusOut(StrictModel):
    status_id: str
    device_id: str
    received_at_ms: int
    desired_release_id: str | None
    running_release_id: str | None
    previous_release_id: str | None
    last_known_good_release_id: str | None
    phase: ReleasePhase
    progress_percent: int | None
    last_error: str | None
    rollback_outcome: RollbackOutcome
    firmware_version: str
    build_id: str


class DeviceReleaseOverview(StrictModel):
    target: DeviceReleaseTargetOut | None
    latest_status: ReleaseStatusOut | None


class DiagnosticSnapshotCommand(StrictModel):
    action: Literal["diagnostic_snapshot"]


class SetLogLevelCommand(StrictModel):
    action: Literal["set_log_level"]
    level: Literal["event", "debug_sensor"]
    duration_seconds: int = Field(strict=True, ge=1, le=600)


class RecalibrateMicrophoneCommand(StrictModel):
    action: Literal["recalibrate_microphone"]


class RetryUploadCommand(StrictModel):
    action: Literal["retry_upload"]


class RebootCommand(StrictModel):
    action: Literal["reboot"]


class OpenDevOtaCommand(StrictModel):
    action: Literal["open_dev_ota"]
    requires_local_confirmation: Literal[True] = True


RemoteCommand = Annotated[
    DiagnosticSnapshotCommand
    | SetLogLevelCommand
    | RecalibrateMicrophoneCommand
    | RetryUploadCommand
    | RebootCommand
    | OpenDevOtaCommand,
    Field(discriminator="action"),
]


class ConsoleCommandCreate(StrictModel):
    command: RemoteCommand
    expires_in_seconds: int = Field(default=300, strict=True, ge=5, le=86400)
    created_by: str = Field(default="presence-console", min_length=1, max_length=64)


class CommandStatus(StrEnum):
    queued = "queued"
    leased = "leased"
    accepted = "accepted"
    running = "running"
    succeeded = "succeeded"
    failed = "failed"
    expired = "expired"
    rejected = "rejected"


class LeasedCommand(StrictModel):
    command_id: str
    created_at_ms: int
    expires_at_ms: int
    lease_id: str
    lease_expires_at_ms: int
    delivery_attempt: int
    command: RemoteCommand


class CommandOut(StrictModel):
    command_id: str
    device_id: str
    created_at_ms: int
    expires_at_ms: int
    created_by: str
    status: CommandStatus
    delivery_attempts: int
    latest_ack_at_ms: int | None
    latest_result: dict[str, str | int | float | bool | None] | None
    command: RemoteCommand


class DeviceCommandAck(StrictModel):
    schema_version: Literal[1]
    ack_id: str = Field(min_length=8, max_length=64, pattern=r"^[A-Za-z0-9._:-]+$")
    command_id: str = Field(min_length=8, max_length=64, pattern=r"^[A-Za-z0-9._:-]+$")
    lease_id: str = Field(min_length=8, max_length=64, pattern=r"^[A-Za-z0-9._:-]+$")
    status: Literal["accepted", "running", "succeeded", "failed", "expired", "rejected"]
    result: dict[str, str | int | float | bool | None] | None = Field(
        default=None, max_length=24
    )

    @model_validator(mode="after")
    def validate_result_size(self) -> DeviceCommandAck:
        if self.result is not None:
            _validate_structured_fields(self.result, "result")
        return self


class CommandAckResponse(StrictModel):
    ack_id: str
    command_id: str
    status: CommandStatus
    duplicate: bool
    server_utc_ms: int


class ControlPollResponse(StrictModel):
    server_utc_ms: int
    poll_after_ms: Literal[5000] = 5000
    desired_release: DesiredRelease | None
    command: LeasedCommand | None


class OperationalLogLevel(StrEnum):
    debug = "debug"
    info = "info"
    warning = "warning"
    error = "error"


class OperationalLogRecord(StrictModel):
    sequence: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)
    uptime_ms: int = Field(strict=True, ge=0, le=MAX_SIGNED_64)
    level: OperationalLogLevel
    event_type: str = Field(
        min_length=1, max_length=64, pattern=r"^[a-z0-9][a-z0-9_.-]*$"
    )
    message: str | None = Field(default=None, max_length=256)
    fields: dict[str, str | int | float | bool | None] = Field(
        default_factory=dict, max_length=24
    )

    @model_validator(mode="after")
    def validate_fields_size(self) -> OperationalLogRecord:
        _validate_structured_fields(self.fields, "fields")
        return self


class OperationalLogBatchIn(StrictModel):
    schema_version: Literal[1]
    batch_id: str = Field(min_length=1, max_length=96, pattern=r"^[A-Za-z0-9._:+-]+$")
    boot_id: str = Field(min_length=16, max_length=64, pattern=r"^[A-Za-z0-9_-]+$")
    build_id: str = Field(min_length=1, max_length=128, pattern=r"^[A-Za-z0-9._:+-]+$")
    records: list[OperationalLogRecord] = Field(min_length=1, max_length=256)

    @model_validator(mode="after")
    def validate_unique_sequences(self) -> OperationalLogBatchIn:
        sequences = [record.sequence for record in self.records]
        if len(sequences) != len(set(sequences)):
            raise ValueError("log sequence values must be unique within a batch")
        return self


class OperationalLogIngestResponse(StrictModel):
    batch_id: str
    stored: int
    duplicates: int
    server_utc_ms: int
    retained_records: int


class OperationalLogOut(OperationalLogRecord):
    device_id: str
    boot_id: str
    build_id: str
    received_at_ms: int


class OperationalLogPage(StrictModel):
    items: list[OperationalLogOut]
    retained_records: int
    truncated: bool


class CoreDumpIn(StrictModel):
    schema_version: Literal[1]
    crash_id: str = Field(min_length=8, max_length=64, pattern=r"^[A-Za-z0-9._:-]+$")
    boot_id: str = Field(min_length=16, max_length=64, pattern=r"^[A-Za-z0-9_-]+$")
    build_id: str = Field(min_length=1, max_length=128, pattern=r"^[A-Za-z0-9._:+-]+$")
    reset_reason: str = Field(min_length=1, max_length=64, pattern=r"^[A-Za-z0-9._-]+$")
    dump_size: int = Field(strict=True, ge=1, le=65_536)
    dump_sha256: str = Field(pattern=r"^[0-9a-f]{64}$")
    dump_base64: str = Field(min_length=1, max_length=90_000)


class CoreDumpIngestResponse(StrictModel):
    crash_id: str
    duplicate: bool
    durable: bool
    server_utc_ms: int
    symbolication_status: str


class SanitizedCoreDumpSummary(StrictModel):
    crash_id: str
    device_id: str
    boot_id: str
    build_id: str
    reset_reason: str
    dump_sha256: str
    dump_size: int
    received_at_ms: int
    release_id: str | None
    symbolication_status: Literal["matched_elf", "missing_elf", "succeeded", "failed"]
    summary: list[str]


class SanitizedCoreDumpPage(StrictModel):
    items: list[SanitizedCoreDumpSummary]
    retained_reports: int
