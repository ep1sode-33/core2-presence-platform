from __future__ import annotations

from sqlalchemy import (
    Boolean,
    Float,
    ForeignKey,
    ForeignKeyConstraint,
    Index,
    Integer,
    String,
    Text,
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
