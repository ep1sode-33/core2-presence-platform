from __future__ import annotations

from enum import StrEnum
from typing import Annotated, Literal

from pydantic import BaseModel, ConfigDict, Field, model_validator

DEVICE_ID_PATTERN = r"^core2-[0-9a-f]{12}$"
MAX_SIGNED_64 = (1 << 63) - 1


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


TelemetryRecord = Annotated[
    SampleRecord | TransitionRecord,
    Field(discriminator="kind"),
]


class TelemetryBatch(StrictModel):
    schema_version: Literal[1]
    batch_id: str = Field(min_length=1, max_length=96, pattern=r"^[A-Za-z0-9._:-]+$")
    boot_id: str = Field(min_length=16, max_length=64, pattern=r"^[A-Za-z0-9_-]+$")
    firmware_version: str = Field(min_length=1, max_length=64)
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
    from_state: PresenceState | None
    to_state: PresenceState
    reason: TransitionReason


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
    minimum_on_ms: int = Field(default=10000, strict=True, ge=0, le=600000)
    pir_hold_ms: int = Field(default=30000, strict=True, ge=1000, le=3600000)
    sound_hold_ms: int = Field(default=12000, strict=True, ge=0, le=600000)
    max_sound_bridge_ms: int = Field(default=300000, strict=True, ge=0, le=3600000)
    cooldown_ms: int = Field(default=5000, strict=True, ge=0, le=600000)
    sound_factor: float = Field(default=1.12, strict=True, ge=1.0, le=4.0)
    telemetry_interval_ms: int = Field(default=1000, strict=True, ge=250, le=60000)
    upload_batch_size: int = Field(default=30, strict=True, ge=1, le=256)


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
