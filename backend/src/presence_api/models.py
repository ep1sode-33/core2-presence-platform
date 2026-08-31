from __future__ import annotations

from sqlalchemy import (
    Boolean,
    Float,
    ForeignKey,
    ForeignKeyConstraint,
    Index,
    Integer,
    LargeBinary,
    String,
    Text,
    UniqueConstraint,
)
from sqlalchemy.orm import DeclarativeBase, Mapped, mapped_column


class Base(DeclarativeBase):
    pass


class Device(Base):
    __tablename__ = "devices"

    device_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    name: Mapped[str | None] = mapped_column(String(128), nullable=True)
    created_at_ms: Mapped[int] = mapped_column(Integer, nullable=False)
    last_seen_at_ms: Mapped[int] = mapped_column(Integer, nullable=False)
    firmware_version: Mapped[str | None] = mapped_column(String(64), nullable=True)
    desired_config_revision: Mapped[int] = mapped_column(
        Integer, nullable=False, default=0
    )
    applied_config_revision: Mapped[int] = mapped_column(
        Integer, nullable=False, default=0
    )
    # Highest release counter proved by a valid ``phase=running`` report.
    # This is device state, not merely a property of the latest status row:
    # a later idle/failure report must never make an old wireless image
    # selectable again.
    confirmed_release_counter: Mapped[int] = mapped_column(
        Integer, nullable=False, default=0
    )


class BootSession(Base):
    __tablename__ = "boot_sessions"

    device_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    boot_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    first_received_at_ms: Mapped[int] = mapped_column(Integer, nullable=False)
    last_received_at_ms: Mapped[int] = mapped_column(Integer, nullable=False)
    anchor_utc_ms: Mapped[int | None] = mapped_column(Integer, nullable=True)
    anchor_uptime_ms: Mapped[int | None] = mapped_column(Integer, nullable=True)
    anchor_source: Mapped[str | None] = mapped_column(String(16), nullable=True)

    __table_args__ = (
        ForeignKeyConstraint(["device_id"], ["devices.device_id"], ondelete="CASCADE"),
    )


class IngestBatch(Base):
    __tablename__ = "ingest_batches"

    device_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    batch_id: Mapped[str] = mapped_column(String(96), primary_key=True)
    body_hash: Mapped[str] = mapped_column(String(64), nullable=False)
    received_at_ms: Mapped[int] = mapped_column(Integer, nullable=False)
    record_count: Mapped[int] = mapped_column(Integer, nullable=False)

    __table_args__ = (
        ForeignKeyConstraint(["device_id"], ["devices.device_id"], ondelete="CASCADE"),
    )


class TelemetryRecord(Base):
    __tablename__ = "telemetry_records"

    device_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    boot_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    seq: Mapped[int] = mapped_column(Integer, primary_key=True)
    kind: Mapped[str] = mapped_column(String(16), nullable=False)
    uptime_ms: Mapped[int] = mapped_column(Integer, nullable=False)
    observed_at_ms: Mapped[int | None] = mapped_column(Integer, nullable=True)
    received_at_ms: Mapped[int] = mapped_column(Integer, nullable=False)
    time_quality: Mapped[str] = mapped_column(String(32), nullable=False)
    # Schema-v2 databases did not retain the batch revision per record. Keep
    # migrated rows nullable rather than incorrectly labelling them revision 0;
    # every record ingested by schema v3 receives a concrete revision.
    applied_config_revision: Mapped[int | None] = mapped_column(Integer, nullable=True)
    build_id: Mapped[str | None] = mapped_column(String(128), nullable=True)
    payload_hash: Mapped[str] = mapped_column(String(64), nullable=False)

    __table_args__ = (
        ForeignKeyConstraint(
            ["device_id", "boot_id"],
            ["boot_sessions.device_id", "boot_sessions.boot_id"],
            ondelete="CASCADE",
        ),
        Index("idx_records_device_observed", "device_id", "observed_at_ms"),
        Index("idx_records_device_received", "device_id", "received_at_ms"),
    )


class Sample(Base):
    __tablename__ = "samples"

    device_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    boot_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    seq: Mapped[int] = mapped_column(Integer, primary_key=True)
    pir: Mapped[bool] = mapped_column(Boolean, nullable=False)
    mic_rms: Mapped[float] = mapped_column(Float, nullable=False)
    mic_envelope: Mapped[float] = mapped_column(Float, nullable=False)
    mic_min: Mapped[int] = mapped_column(Integer, nullable=False)
    mic_max: Mapped[int] = mapped_column(Integer, nullable=False)
    noise_floor: Mapped[float] = mapped_column(Float, nullable=False)
    sound_threshold: Mapped[float] = mapped_column(Float, nullable=False)
    sound_active: Mapped[bool] = mapped_column(Boolean, nullable=False)
    state: Mapped[str] = mapped_column(String(16), nullable=False)
    brightness: Mapped[int] = mapped_column(Integer, nullable=False)

    __table_args__ = (
        ForeignKeyConstraint(
            ["device_id", "boot_id", "seq"],
            [
                "telemetry_records.device_id",
                "telemetry_records.boot_id",
                "telemetry_records.seq",
            ],
            ondelete="CASCADE",
        ),
    )


class StateTransition(Base):
    __tablename__ = "state_transitions"

    device_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    boot_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    seq: Mapped[int] = mapped_column(Integer, primary_key=True)
    from_state: Mapped[str | None] = mapped_column(String(16), nullable=True)
    to_state: Mapped[str] = mapped_column(String(16), nullable=False)
    reason: Mapped[str] = mapped_column(String(32), nullable=False)
    pir: Mapped[bool | None] = mapped_column(Boolean, nullable=True)
    pir_age_ms: Mapped[int | None] = mapped_column(Integer, nullable=True)
    sound_active: Mapped[bool | None] = mapped_column(Boolean, nullable=True)
    sound_age_ms: Mapped[int | None] = mapped_column(Integer, nullable=True)
    mic_envelope: Mapped[float | None] = mapped_column(Float, nullable=True)
    noise_floor: Mapped[float | None] = mapped_column(Float, nullable=True)
    sound_threshold: Mapped[float | None] = mapped_column(Float, nullable=True)
    brightness_before: Mapped[int | None] = mapped_column(Integer, nullable=True)
    brightness_after: Mapped[int | None] = mapped_column(Integer, nullable=True)

    __table_args__ = (
        ForeignKeyConstraint(
            ["device_id", "boot_id", "seq"],
            [
                "telemetry_records.device_id",
                "telemetry_records.boot_id",
                "telemetry_records.seq",
            ],
            ondelete="CASCADE",
        ),
    )


class Feedback(Base):
    __tablename__ = "feedback"

    feedback_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    device_id: Mapped[str] = mapped_column(
        String(64), ForeignKey("devices.device_id", ondelete="CASCADE")
    )
    boot_id: Mapped[str | None] = mapped_column(String(64), nullable=True)
    seq: Mapped[int | None] = mapped_column(Integer, nullable=True)
    created_at_ms: Mapped[int] = mapped_column(Integer, nullable=False)
    occurred_at_ms: Mapped[int | None] = mapped_column(Integer, nullable=True)
    occurred_uptime_ms: Mapped[int | None] = mapped_column(Integer, nullable=True)
    time_quality: Mapped[str] = mapped_column(String(32), nullable=False)
    actual_presence: Mapped[str] = mapped_column(String(16), nullable=False)
    observed_state: Mapped[str | None] = mapped_column(String(16), nullable=True)
    source: Mapped[str] = mapped_column(String(16), nullable=False)
    note: Mapped[str | None] = mapped_column(Text, nullable=True)
    payload_hash: Mapped[str] = mapped_column(String(64), nullable=False)

    __table_args__ = (
        Index("idx_feedback_device_created", "device_id", "created_at_ms"),
    )


class ConfigRevision(Base):
    __tablename__ = "config_revisions"

    device_id: Mapped[str] = mapped_column(
        String(64),
        ForeignKey("devices.device_id", ondelete="CASCADE"),
        primary_key=True,
    )
    revision: Mapped[int] = mapped_column(Integer, primary_key=True)
    created_at_ms: Mapped[int] = mapped_column(Integer, nullable=False)
    created_by: Mapped[str] = mapped_column(String(64), nullable=False)
    config_json: Mapped[str] = mapped_column(Text, nullable=False)


class DeviceHealthReport(Base):
    __tablename__ = "device_health_reports"

    device_id: Mapped[str] = mapped_column(
        String(64),
        ForeignKey("devices.device_id", ondelete="CASCADE"),
        primary_key=True,
    )
    boot_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    sequence: Mapped[int] = mapped_column(Integer, primary_key=True)
    received_at_ms: Mapped[int] = mapped_column(Integer, nullable=False)
    uptime_ms: Mapped[int] = mapped_column(Integer, nullable=False)
    local_level: Mapped[str] = mapped_column(String(24), nullable=False)
    firmware_version: Mapped[str] = mapped_column(String(64), nullable=False)
    build_id: Mapped[str] = mapped_column(String(128), nullable=False)
    body_hash: Mapped[str] = mapped_column(String(64), nullable=False)
    snapshot_json: Mapped[str] = mapped_column(Text, nullable=False)

    __table_args__ = (
        Index(
            "idx_health_device_received",
            "device_id",
            "received_at_ms",
            "boot_id",
            "sequence",
        ),
    )


class FirmwareRelease(Base):
    __tablename__ = "firmware_releases"

    release_id: Mapped[str] = mapped_column(String(48), primary_key=True)
    hardware_model: Mapped[str] = mapped_column(String(64), nullable=False)
    firmware_version: Mapped[str] = mapped_column(String(64), nullable=False)
    release_counter: Mapped[int] = mapped_column(Integer, nullable=False)
    build_id: Mapped[str] = mapped_column(String(128), nullable=False)
    image_size: Mapped[int] = mapped_column(Integer, nullable=False)
    image_sha256: Mapped[str] = mapped_column(String(64), nullable=False)
    elf_size: Mapped[int] = mapped_column(Integer, nullable=False)
    elf_sha256: Mapped[str] = mapped_column(String(64), nullable=False)
    key_id: Mapped[str] = mapped_column(String(64), nullable=False)
    signature_format: Mapped[str] = mapped_column(String(48), nullable=False)
    signed_record: Mapped[bytes] = mapped_column(LargeBinary, nullable=False)
    signature: Mapped[bytes] = mapped_column(LargeBinary, nullable=False)
    manifest_json: Mapped[str] = mapped_column(Text, nullable=False)
    image_blob: Mapped[bytes] = mapped_column(LargeBinary, nullable=False)
    elf_blob: Mapped[bytes] = mapped_column(LargeBinary, nullable=False)
    imported_at_ms: Mapped[int] = mapped_column(Integer, nullable=False)
    imported_by: Mapped[str] = mapped_column(String(64), nullable=False)
    verified: Mapped[bool] = mapped_column(Boolean, nullable=False)

    __table_args__ = (
        UniqueConstraint(
            "hardware_model", "release_counter", name="uq_release_model_counter"
        ),
        UniqueConstraint("build_id", name="uq_release_build_id"),
    )


class DeviceReleaseTarget(Base):
    __tablename__ = "device_release_targets"

    device_id: Mapped[str] = mapped_column(
        String(64),
        ForeignKey("devices.device_id", ondelete="CASCADE"),
        primary_key=True,
    )
    release_id: Mapped[str] = mapped_column(
        String(48), ForeignKey("firmware_releases.release_id"), nullable=False
    )
    selected_at_ms: Mapped[int] = mapped_column(Integer, nullable=False)
    selected_by: Mapped[str] = mapped_column(String(64), nullable=False)
    completed_at_ms: Mapped[int | None] = mapped_column(Integer, nullable=True)


class ReleaseStatusReport(Base):
    __tablename__ = "release_status_reports"

    status_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    device_id: Mapped[str] = mapped_column(
        String(64), ForeignKey("devices.device_id", ondelete="CASCADE")
    )
    received_at_ms: Mapped[int] = mapped_column(Integer, nullable=False)
    desired_release_id: Mapped[str | None] = mapped_column(String(48), nullable=True)
    running_release_id: Mapped[str | None] = mapped_column(String(48), nullable=True)
    previous_release_id: Mapped[str | None] = mapped_column(String(48), nullable=True)
    last_known_good_release_id: Mapped[str | None] = mapped_column(
        String(48), nullable=True
    )
    phase: Mapped[str] = mapped_column(String(32), nullable=False)
    progress_percent: Mapped[int | None] = mapped_column(Integer, nullable=True)
    last_error: Mapped[str | None] = mapped_column(Text, nullable=True)
    rollback_outcome: Mapped[str] = mapped_column(String(24), nullable=False)
    firmware_version: Mapped[str] = mapped_column(String(64), nullable=False)
    build_id: Mapped[str] = mapped_column(String(128), nullable=False)
    body_hash: Mapped[str] = mapped_column(String(64), nullable=False)
    # Idempotent retries must return the outcome produced by this exact report,
    # not derive an answer from a release target that may since have changed.
    desired_release_completed: Mapped[bool] = mapped_column(
        Boolean, nullable=False, default=False
    )

    __table_args__ = (
        Index("idx_release_status_device_received", "device_id", "received_at_ms"),
    )


class DeviceCommand(Base):
    __tablename__ = "device_commands"

    command_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    device_id: Mapped[str] = mapped_column(
        String(64), ForeignKey("devices.device_id", ondelete="CASCADE")
    )
    created_at_ms: Mapped[int] = mapped_column(Integer, nullable=False)
    expires_at_ms: Mapped[int] = mapped_column(Integer, nullable=False)
    created_by: Mapped[str] = mapped_column(String(64), nullable=False)
    action: Mapped[str] = mapped_column(String(40), nullable=False)
    payload_json: Mapped[str] = mapped_column(Text, nullable=False)
    status: Mapped[str] = mapped_column(String(24), nullable=False)
    lease_id: Mapped[str | None] = mapped_column(String(64), nullable=True)
    lease_expires_at_ms: Mapped[int | None] = mapped_column(Integer, nullable=True)
    delivery_attempts: Mapped[int] = mapped_column(Integer, nullable=False)
    latest_ack_at_ms: Mapped[int | None] = mapped_column(Integer, nullable=True)
    latest_result_json: Mapped[str | None] = mapped_column(Text, nullable=True)

    __table_args__ = (
        Index(
            "idx_commands_device_delivery",
            "device_id",
            "status",
            "expires_at_ms",
            "created_at_ms",
        ),
    )


class CommandAck(Base):
    __tablename__ = "command_acks"

    ack_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    command_id: Mapped[str] = mapped_column(
        String(64), ForeignKey("device_commands.command_id", ondelete="CASCADE")
    )
    device_id: Mapped[str] = mapped_column(String(64), nullable=False)
    received_at_ms: Mapped[int] = mapped_column(Integer, nullable=False)
    status: Mapped[str] = mapped_column(String(24), nullable=False)
    body_hash: Mapped[str] = mapped_column(String(64), nullable=False)
    result_json: Mapped[str | None] = mapped_column(Text, nullable=True)

    __table_args__ = (
        Index("idx_command_acks_command_received", "command_id", "received_at_ms"),
    )


class OperationalLogBatch(Base):
    __tablename__ = "operational_log_batches"

    device_id: Mapped[str] = mapped_column(
        String(64),
        ForeignKey("devices.device_id", ondelete="CASCADE"),
        primary_key=True,
    )
    batch_id: Mapped[str] = mapped_column(String(96), primary_key=True)
    boot_id: Mapped[str] = mapped_column(String(64), nullable=False)
    build_id: Mapped[str] = mapped_column(String(128), nullable=False)
    received_at_ms: Mapped[int] = mapped_column(Integer, nullable=False)
    body_hash: Mapped[str] = mapped_column(String(64), nullable=False)
    record_count: Mapped[int] = mapped_column(Integer, nullable=False)


class OperationalLog(Base):
    __tablename__ = "operational_logs"

    device_id: Mapped[str] = mapped_column(
        String(64),
        ForeignKey("devices.device_id", ondelete="CASCADE"),
        primary_key=True,
    )
    boot_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    sequence: Mapped[int] = mapped_column(Integer, primary_key=True)
    build_id: Mapped[str] = mapped_column(String(128), nullable=False)
    uptime_ms: Mapped[int] = mapped_column(Integer, nullable=False)
    received_at_ms: Mapped[int] = mapped_column(Integer, nullable=False)
    level: Mapped[str] = mapped_column(String(16), nullable=False)
    event_type: Mapped[str] = mapped_column(String(64), nullable=False)
    message: Mapped[str | None] = mapped_column(Text, nullable=True)
    fields_json: Mapped[str] = mapped_column(Text, nullable=False)
    payload_hash: Mapped[str] = mapped_column(String(64), nullable=False)

    __table_args__ = (Index("idx_logs_device_received", "device_id", "received_at_ms"),)


class CoreDump(Base):
    __tablename__ = "core_dumps"

    crash_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    device_id: Mapped[str] = mapped_column(
        String(64), ForeignKey("devices.device_id", ondelete="CASCADE")
    )
    boot_id: Mapped[str] = mapped_column(String(64), nullable=False)
    build_id: Mapped[str] = mapped_column(String(128), nullable=False)
    reset_reason: Mapped[str] = mapped_column(String(64), nullable=False)
    dump_sha256: Mapped[str] = mapped_column(String(64), nullable=False)
    dump_size: Mapped[int] = mapped_column(Integer, nullable=False)
    received_at_ms: Mapped[int] = mapped_column(Integer, nullable=False)
    body_hash: Mapped[str] = mapped_column(String(64), nullable=False)
    raw_dump: Mapped[bytes] = mapped_column(LargeBinary, nullable=False)
    release_id: Mapped[str | None] = mapped_column(String(48), nullable=True)
    symbolication_status: Mapped[str] = mapped_column(String(24), nullable=False)
    sanitized_summary_json: Mapped[str] = mapped_column(Text, nullable=False)

    __table_args__ = (
        Index("idx_coredumps_device_received", "device_id", "received_at_ms"),
    )
