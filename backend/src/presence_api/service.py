from __future__ import annotations

import base64
import binascii
import hashlib
import json
import time
from dataclasses import dataclass

from pydantic import BaseModel
from sqlalchemy import and_, func, or_, select, text
from sqlalchemy.orm import Session

from .database import Database
from .models import (
    BootSession,
    ConfigRevision,
    Device,
    Feedback,
    IngestBatch,
    Sample,
    StateTransition,
    TelemetryRecord,
)
from .schemas import (
    MAX_SIGNED_64,
    ConfigPut,
    ConfigResponse,
    FeedbackCreate,
    FeedbackPage,
    FeedbackResponse,
    IngestResponse,
    PresenceConfig,
    SampleOut,
    SamplePage,
    SampleRecord,
    TelemetryBatch,
    TransitionOut,
    TransitionPage,
    TransitionRecord,
)


class ConflictError(RuntimeError):
    pass


class NotFoundError(RuntimeError):
    pass


@dataclass(frozen=True)
class QueryWindow:
    start_ms: int | None = None
    end_ms: int | None = None
    limit: int = 500
    cursor: TelemetryPageCursor | None = None


@dataclass(frozen=True)
class TelemetryPageCursor:
    received_at_ms: int
    boot_id: str
    seq: int


@dataclass(frozen=True)
class FeedbackPageCursor:
    created_at_ms: int
    feedback_id: str


def _encode_cursor(payload: dict) -> str:
    encoded = json.dumps(payload, separators=(",", ":"), sort_keys=True).encode()
    return base64.urlsafe_b64encode(encoded).decode().rstrip("=")


def _decode_cursor(value: str) -> dict:
    try:
        padding = "=" * (-len(value) % 4)
        decoded = base64.urlsafe_b64decode(value + padding)
        payload = json.loads(decoded)
    except (binascii.Error, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError("invalid pagination cursor") from exc
    if not isinstance(payload, dict):
        raise ValueError("invalid pagination cursor")
    return payload


def encode_telemetry_cursor(record: TelemetryRecord) -> str:
    return _encode_cursor(
        {
            "v": 1,
            "k": "telemetry",
            "t": record.received_at_ms,
            "b": record.boot_id,
            "s": record.seq,
        }
    )


def decode_telemetry_cursor(value: str) -> TelemetryPageCursor:
    payload = _decode_cursor(value)
    if (
        set(payload) != {"v", "k", "t", "b", "s"}
        or payload["v"] != 1
        or payload["k"] != "telemetry"
        or type(payload["t"]) is not int
        or not 0 <= payload["t"] <= MAX_SIGNED_64
        or not isinstance(payload["b"], str)
        or type(payload["s"]) is not int
        or not 0 <= payload["s"] <= MAX_SIGNED_64
    ):
        raise ValueError("invalid telemetry pagination cursor")
    return TelemetryPageCursor(
        received_at_ms=payload["t"], boot_id=payload["b"], seq=payload["s"]
    )


def encode_feedback_cursor(row: Feedback) -> str:
    return _encode_cursor(
        {
            "v": 1,
            "k": "feedback",
            "t": row.created_at_ms,
            "i": row.feedback_id,
        }
    )


def decode_feedback_cursor(value: str) -> FeedbackPageCursor:
    payload = _decode_cursor(value)
    if (
        set(payload) != {"v", "k", "t", "i"}
        or payload["v"] != 1
        or payload["k"] != "feedback"
        or type(payload["t"]) is not int
        or not 0 <= payload["t"] <= MAX_SIGNED_64
        or not isinstance(payload["i"], str)
    ):
        raise ValueError("invalid feedback pagination cursor")
    return FeedbackPageCursor(created_at_ms=payload["t"], feedback_id=payload["i"])


def utc_now_ms() -> int:
    return time.time_ns() // 1_000_000


def canonical_hash(value: BaseModel | dict) -> str:
    if isinstance(value, BaseModel):
        payload = value.model_dump(mode="json", exclude_none=False)
    else:
        payload = value
    encoded = json.dumps(
        payload,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=True,
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


_V07_TRANSITION_FIELDS = (
    "pir",
    "pir_age_ms",
    "sound_active",
    "sound_age_ms",
    "mic_envelope",
    "noise_floor",
    "sound_threshold",
    "brightness_before",
    "brightness_after",
)


def telemetry_hash(value: TelemetryBatch | SampleRecord | TransitionRecord) -> str:
    """Hash telemetry without changing pre-v0.7 idempotency identities.

    The v0.7 fields are optional only so an already stored v1 batch can still
    be retried after the schema upgrade. Omitting absent additions recreates
    the exact canonical JSON used by schema v3.
    """
    payload = value.model_dump(mode="json", exclude_none=False)
    if isinstance(value, TelemetryBatch):
        if value.build_id is None:
            payload.pop("build_id", None)
        for record in payload["records"]:
            if record.get("kind") == "transition":
                for field in _V07_TRANSITION_FIELDS:
                    if record.get(field) is None:
                        record.pop(field, None)
    elif isinstance(value, TransitionRecord):
        for field in _V07_TRANSITION_FIELDS:
            if payload.get(field) is None:
                payload.pop(field, None)
    return canonical_hash(payload)


class PresenceService:
    def __init__(self, database: Database) -> None:
        self.database = database

    @staticmethod
    def _ensure_device(
        session: Session,
        device_id: str,
        now_ms: int,
        firmware_version: str | None = None,
        applied_config_revision: int | None = None,
    ) -> Device:
        device = session.get(Device, device_id)
        if device is None:
            device = Device(
                device_id=device_id,
                created_at_ms=now_ms,
                last_seen_at_ms=now_ms,
                firmware_version=firmware_version,
                desired_config_revision=0,
                applied_config_revision=applied_config_revision or 0,
            )
            session.add(device)
        else:
            device.last_seen_at_ms = now_ms
            if firmware_version is not None:
                device.firmware_version = firmware_version
            if applied_config_revision is not None:
                device.applied_config_revision = max(
                    device.applied_config_revision, applied_config_revision
                )
        return device

    @staticmethod
    def _observed_time(boot: BootSession, uptime_ms: int) -> tuple[int | None, str]:
        if boot.anchor_utc_ms is None or boot.anchor_uptime_ms is None:
            return None, "receive_only"
        return (
            boot.anchor_utc_ms + uptime_ms - boot.anchor_uptime_ms,
            f"anchor_{boot.anchor_source or 'unknown'}",
        )

    @staticmethod
    def _backfill_anchor(session: Session, boot: BootSession) -> None:
        if boot.anchor_utc_ms is None or boot.anchor_uptime_ms is None:
            return
        records = session.scalars(
            select(TelemetryRecord).where(
                TelemetryRecord.device_id == boot.device_id,
                TelemetryRecord.boot_id == boot.boot_id,
                TelemetryRecord.observed_at_ms.is_(None),
            )
        ).all()
        for record in records:
            record.observed_at_ms = (
                boot.anchor_utc_ms + record.uptime_ms - boot.anchor_uptime_ms
            )
            record.time_quality = f"anchor_{boot.anchor_source or 'unknown'}"
        feedback_rows = session.scalars(
            select(Feedback).where(
                Feedback.device_id == boot.device_id,
                Feedback.boot_id == boot.boot_id,
                Feedback.occurred_uptime_ms.is_not(None),
            )
        ).all()
        for feedback in feedback_rows:
            feedback.occurred_at_ms = (
                boot.anchor_utc_ms + feedback.occurred_uptime_ms - boot.anchor_uptime_ms
            )
            feedback.time_quality = f"anchor_{boot.anchor_source or 'unknown'}"

    def ingest(self, device_id: str, batch: TelemetryBatch) -> IngestResponse:
        received_at_ms = utc_now_ms()
        batch_hash = telemetry_hash(batch)
        max_seq = max(record.seq for record in batch.records)

        with self.database.session() as session:
            # Serialize writers before checking idempotency keys. This makes
            # concurrent retries observe the first committed batch instead of
            # racing into a UNIQUE constraint at commit time.
            session.execute(text("BEGIN IMMEDIATE"))
            existing_batch = session.get(IngestBatch, (device_id, batch.batch_id))
            if existing_batch is not None:
                if existing_batch.body_hash != batch_hash:
                    raise ConflictError(
                        "batch_id already exists with a different payload"
                    )
                device = session.get(Device, device_id)
                desired_revision = (
                    device.desired_config_revision if device is not None else 0
                )
                return IngestResponse(
                    batch_id=batch.batch_id,
                    stored=0,
                    duplicates=len(batch.records),
                    max_seq=max_seq,
                    server_utc_ms=received_at_ms,
                    desired_config_revision=desired_revision,
                )

            try:
                if batch.applied_config_revision > 0:
                    applied_revision = session.get(
                        ConfigRevision,
                        (device_id, batch.applied_config_revision),
                    )
                    if applied_revision is None:
                        raise ConflictError(
                            "applied_config_revision does not exist for device"
                        )
                device = self._ensure_device(
                    session,
                    device_id,
                    received_at_ms,
                    batch.firmware_version,
                    batch.applied_config_revision,
                )

                boot = session.get(BootSession, (device_id, batch.boot_id))
                if boot is None:
                    boot = BootSession(
                        device_id=device_id,
                        boot_id=batch.boot_id,
                        first_received_at_ms=received_at_ms,
                        last_received_at_ms=received_at_ms,
                    )
                    session.add(boot)
                    session.flush()
                else:
                    boot.last_received_at_ms = received_at_ms

                if batch.clock_anchor is not None:
                    incoming_anchor = (
                        batch.clock_anchor.utc_ms,
                        batch.clock_anchor.uptime_ms,
                        batch.clock_anchor.source.value,
                    )
                    existing_anchor = (
                        boot.anchor_utc_ms,
                        boot.anchor_uptime_ms,
                        boot.anchor_source,
                    )
                    if boot.anchor_utc_ms is None:
                        (
                            boot.anchor_utc_ms,
                            boot.anchor_uptime_ms,
                            boot.anchor_source,
                        ) = incoming_anchor
                        self._backfill_anchor(session, boot)
                    elif existing_anchor != incoming_anchor:
                        raise ConflictError(
                            "clock anchor is immutable within one boot session"
                        )

                stored = 0
                duplicates = 0
                for item in batch.records:
                    record_hash = telemetry_hash(item)
                    key = (device_id, batch.boot_id, item.seq)
                    existing_record = session.get(TelemetryRecord, key)
                    if existing_record is not None:
                        if existing_record.payload_hash != record_hash:
                            raise ConflictError(
                                "record identity already exists with a "
                                "different payload: "
                                f"{device_id}/{batch.boot_id}/{item.seq}"
                            )
                        if (
                            existing_record.applied_config_revision is not None
                            and existing_record.applied_config_revision
                            != batch.applied_config_revision
                        ):
                            raise ConflictError(
                                "record identity already exists with a "
                                "different applied_config_revision: "
                                f"{device_id}/{batch.boot_id}/{item.seq}"
                            )
                        duplicates += 1
                        continue

                    observed_at_ms, time_quality = self._observed_time(
                        boot, item.uptime_ms
                    )
                    telemetry_record = TelemetryRecord(
                        device_id=device_id,
                        boot_id=batch.boot_id,
                        seq=item.seq,
                        kind=item.kind,
                        uptime_ms=item.uptime_ms,
                        observed_at_ms=observed_at_ms,
                        received_at_ms=received_at_ms,
                        time_quality=time_quality,
                        applied_config_revision=batch.applied_config_revision,
                        build_id=batch.build_id,
                        payload_hash=record_hash,
                    )
                    session.add(telemetry_record)
                    # The child tables use a composite FK without ORM
                    # relationships. Flush the parent explicitly so a query-
                    # triggered autoflush cannot insert the child first.
                    session.flush()

                    if isinstance(item, SampleRecord):
                        session.add(
                            Sample(
                                device_id=device_id,
                                boot_id=batch.boot_id,
                                seq=item.seq,
                                pir=item.pir,
                                mic_rms=item.mic_rms,
                                mic_envelope=item.mic_envelope,
                                mic_min=item.mic_min,
                                mic_max=item.mic_max,
                                noise_floor=item.noise_floor,
                                sound_threshold=item.sound_threshold,
                                sound_active=item.sound_active,
                                state=item.state.value,
                                brightness=item.brightness,
                            )
                        )
                    elif isinstance(item, TransitionRecord):
                        session.add(
                            StateTransition(
                                device_id=device_id,
                                boot_id=batch.boot_id,
                                seq=item.seq,
                                from_state=(
                                    item.from_state.value
                                    if item.from_state is not None
                                    else None
                                ),
                                to_state=item.to_state.value,
                                reason=item.reason.value,
                                pir=item.pir,
                                pir_age_ms=item.pir_age_ms,
                                sound_active=item.sound_active,
                                sound_age_ms=item.sound_age_ms,
                                mic_envelope=item.mic_envelope,
                                noise_floor=item.noise_floor,
                                sound_threshold=item.sound_threshold,
                                brightness_before=item.brightness_before,
                                brightness_after=item.brightness_after,
                            )
                        )
                    stored += 1

                session.add(
                    IngestBatch(
                        device_id=device_id,
                        batch_id=batch.batch_id,
                        body_hash=batch_hash,
                        received_at_ms=received_at_ms,
                        record_count=len(batch.records),
                    )
                )
                session.commit()
            except Exception:
                session.rollback()
                raise

            return IngestResponse(
                batch_id=batch.batch_id,
                stored=stored,
                duplicates=duplicates,
                max_seq=max_seq,
                server_utc_ms=received_at_ms,
                desired_config_revision=device.desired_config_revision,
            )

    @staticmethod
    def _sample_out(record: TelemetryRecord, sample: Sample) -> SampleOut:
        return SampleOut(
            device_id=record.device_id,
            boot_id=record.boot_id,
            seq=record.seq,
            uptime_ms=record.uptime_ms,
            observed_at_ms=record.observed_at_ms,
            received_at_ms=record.received_at_ms,
            time_quality=record.time_quality,
            applied_config_revision=record.applied_config_revision,
            pir=sample.pir,
            mic_rms=sample.mic_rms,
            mic_envelope=sample.mic_envelope,
            mic_min=sample.mic_min,
            mic_max=sample.mic_max,
            noise_floor=sample.noise_floor,
            sound_threshold=sample.sound_threshold,
            sound_active=sample.sound_active,
            state=sample.state,
            brightness=sample.brightness,
        )

    @staticmethod
    def _transition_out(
        record: TelemetryRecord, transition: StateTransition
    ) -> TransitionOut:
        return TransitionOut(
            device_id=record.device_id,
            boot_id=record.boot_id,
            seq=record.seq,
            uptime_ms=record.uptime_ms,
            observed_at_ms=record.observed_at_ms,
            received_at_ms=record.received_at_ms,
            time_quality=record.time_quality,
            applied_config_revision=record.applied_config_revision,
            build_id=record.build_id,
            from_state=transition.from_state,
            to_state=transition.to_state,
            reason=transition.reason,
            pir=transition.pir,
            pir_age_ms=transition.pir_age_ms,
            sound_active=transition.sound_active,
            sound_age_ms=transition.sound_age_ms,
            mic_envelope=transition.mic_envelope,
            noise_floor=transition.noise_floor,
            sound_threshold=transition.sound_threshold,
            brightness_before=transition.brightness_before,
            brightness_after=transition.brightness_after,
        )

    def latest_sample(self, device_id: str) -> SampleOut:
        event_time = func.coalesce(
            TelemetryRecord.observed_at_ms, TelemetryRecord.received_at_ms
        )
        with self.database.session() as session:
            row = session.execute(
                select(TelemetryRecord, Sample)
                .join(
                    Sample,
                    (Sample.device_id == TelemetryRecord.device_id)
                    & (Sample.boot_id == TelemetryRecord.boot_id)
                    & (Sample.seq == TelemetryRecord.seq),
                )
                .where(TelemetryRecord.device_id == device_id)
                .order_by(
                    event_time.desc(),
                    TelemetryRecord.boot_id.desc(),
                    TelemetryRecord.seq.desc(),
                )
                .limit(1)
            ).first()
            if row is None:
                raise NotFoundError("no samples for device")
            return self._sample_out(row[0], row[1])

    def list_samples(self, device_id: str, window: QueryWindow) -> SamplePage:
        statement = (
            select(TelemetryRecord, Sample)
            .join(
                Sample,
                (Sample.device_id == TelemetryRecord.device_id)
                & (Sample.boot_id == TelemetryRecord.boot_id)
                & (Sample.seq == TelemetryRecord.seq),
            )
            .where(TelemetryRecord.device_id == device_id)
        )
        if window.start_ms is not None:
            statement = statement.where(
                TelemetryRecord.received_at_ms >= window.start_ms
            )
        if window.end_ms is not None:
            statement = statement.where(TelemetryRecord.received_at_ms <= window.end_ms)
        if window.cursor is not None:
            cursor = window.cursor
            statement = statement.where(
                or_(
                    TelemetryRecord.received_at_ms < cursor.received_at_ms,
                    and_(
                        TelemetryRecord.received_at_ms == cursor.received_at_ms,
                        TelemetryRecord.boot_id < cursor.boot_id,
                    ),
                    and_(
                        TelemetryRecord.received_at_ms == cursor.received_at_ms,
                        TelemetryRecord.boot_id == cursor.boot_id,
                        TelemetryRecord.seq < cursor.seq,
                    ),
                )
            )
        statement = statement.order_by(
            TelemetryRecord.received_at_ms.desc(),
            TelemetryRecord.boot_id.desc(),
            TelemetryRecord.seq.desc(),
        ).limit(window.limit + 1)

        with self.database.session() as session:
            rows = session.execute(statement).all()
            truncated = len(rows) > window.limit
            items = [
                self._sample_out(record, sample)
                for record, sample in rows[: window.limit]
            ]
            next_cursor = None
            if truncated:
                last_record = rows[window.limit - 1][0]
                next_cursor = encode_telemetry_cursor(last_record)
            return SamplePage(items=items, truncated=truncated, next_cursor=next_cursor)

    def list_transitions(self, device_id: str, window: QueryWindow) -> TransitionPage:
        statement = (
            select(TelemetryRecord, StateTransition)
            .join(
                StateTransition,
                (StateTransition.device_id == TelemetryRecord.device_id)
                & (StateTransition.boot_id == TelemetryRecord.boot_id)
                & (StateTransition.seq == TelemetryRecord.seq),
            )
            .where(TelemetryRecord.device_id == device_id)
        )
        if window.start_ms is not None:
            statement = statement.where(
                TelemetryRecord.received_at_ms >= window.start_ms
            )
        if window.end_ms is not None:
            statement = statement.where(TelemetryRecord.received_at_ms <= window.end_ms)
        if window.cursor is not None:
            cursor = window.cursor
            statement = statement.where(
                or_(
                    TelemetryRecord.received_at_ms < cursor.received_at_ms,
                    and_(
                        TelemetryRecord.received_at_ms == cursor.received_at_ms,
                        TelemetryRecord.boot_id < cursor.boot_id,
                    ),
                    and_(
                        TelemetryRecord.received_at_ms == cursor.received_at_ms,
                        TelemetryRecord.boot_id == cursor.boot_id,
                        TelemetryRecord.seq < cursor.seq,
                    ),
                )
            )
        statement = statement.order_by(
            TelemetryRecord.received_at_ms.desc(),
            TelemetryRecord.boot_id.desc(),
            TelemetryRecord.seq.desc(),
        ).limit(window.limit + 1)

        with self.database.session() as session:
            rows = session.execute(statement).all()
            truncated = len(rows) > window.limit
            items = [
                self._transition_out(record, transition)
                for record, transition in rows[: window.limit]
            ]
            next_cursor = None
            if truncated:
                last_record = rows[window.limit - 1][0]
                next_cursor = encode_telemetry_cursor(last_record)
            return TransitionPage(
                items=items, truncated=truncated, next_cursor=next_cursor
            )

    def create_feedback(
        self, device_id: str, feedback: FeedbackCreate
    ) -> FeedbackResponse:
        now_ms = utc_now_ms()
        payload_hash = canonical_hash(feedback)
        with self.database.session() as session:
            session.execute(text("BEGIN IMMEDIATE"))
            existing = session.get(Feedback, feedback.feedback_id)
            if existing is not None:
                identity_changed = existing.device_id != device_id
                payload_changed = existing.payload_hash != payload_hash
                if identity_changed or payload_changed:
                    raise ConflictError(
                        "feedback_id already exists with a different payload"
                    )
                return self._feedback_out(existing, duplicate=True)

            try:
                self._ensure_device(session, device_id, now_ms)
                session.flush()
                occurred_at_ms = now_ms
                occurred_uptime_ms = None
                time_quality = "server_receive"
                if feedback.boot_id is not None and feedback.seq is not None:
                    referenced_record = session.get(
                        TelemetryRecord,
                        (device_id, feedback.boot_id, feedback.seq),
                    )
                    if referenced_record is None:
                        raise NotFoundError(
                            "referenced telemetry record was not found for device"
                        )
                    if referenced_record.kind != "sample":
                        raise NotFoundError(
                            "referenced telemetry sample was not found for device"
                        )
                    occurred_at_ms = referenced_record.observed_at_ms
                    occurred_uptime_ms = referenced_record.uptime_ms
                    time_quality = referenced_record.time_quality
                row = Feedback(
                    feedback_id=feedback.feedback_id,
                    device_id=device_id,
                    boot_id=feedback.boot_id,
                    seq=feedback.seq,
                    created_at_ms=now_ms,
                    occurred_at_ms=occurred_at_ms,
                    occurred_uptime_ms=occurred_uptime_ms,
                    time_quality=time_quality,
                    actual_presence=feedback.actual_presence.value,
                    observed_state=(
                        feedback.observed_state.value
                        if feedback.observed_state is not None
                        else None
                    ),
                    source=feedback.source.value,
                    note=feedback.note,
                    payload_hash=payload_hash,
                )
                session.add(row)
                session.commit()
            except Exception:
                session.rollback()
                raise
            return self._feedback_out(row, duplicate=False)

    @staticmethod
    def _feedback_out(row: Feedback, duplicate: bool = False) -> FeedbackResponse:
        return FeedbackResponse(
            feedback_id=row.feedback_id,
            device_id=row.device_id,
            boot_id=row.boot_id,
            seq=row.seq,
            created_at_ms=row.created_at_ms,
            occurred_at_ms=row.occurred_at_ms,
            occurred_uptime_ms=row.occurred_uptime_ms,
            time_quality=row.time_quality,
            actual_presence=row.actual_presence,
            observed_state=row.observed_state,
            source=row.source,
            note=row.note,
            duplicate=duplicate,
        )

    def list_feedback(
        self,
        device_id: str,
        limit: int,
        cursor: FeedbackPageCursor | None,
    ) -> FeedbackPage:
        statement = select(Feedback).where(Feedback.device_id == device_id)
        if cursor is not None:
            statement = statement.where(
                or_(
                    Feedback.created_at_ms < cursor.created_at_ms,
                    and_(
                        Feedback.created_at_ms == cursor.created_at_ms,
                        Feedback.feedback_id < cursor.feedback_id,
                    ),
                )
            )
        statement = statement.order_by(
            Feedback.created_at_ms.desc(), Feedback.feedback_id.desc()
        ).limit(limit + 1)
        with self.database.session() as session:
            rows = session.scalars(statement).all()
            truncated = len(rows) > limit
            page_rows = rows[:limit]
            next_cursor = encode_feedback_cursor(page_rows[-1]) if truncated else None
            return FeedbackPage(
                items=[self._feedback_out(row) for row in page_rows],
                truncated=truncated,
                next_cursor=next_cursor,
            )

    def get_config(self, device_id: str) -> ConfigResponse:
        with self.database.session() as session:
            row = session.scalars(
                select(ConfigRevision)
                .where(ConfigRevision.device_id == device_id)
                .order_by(ConfigRevision.revision.desc())
                .limit(1)
            ).first()
            if row is None:
                return ConfigResponse(
                    device_id=device_id,
                    revision=0,
                    created_at_ms=None,
                    created_by=None,
                    config=PresenceConfig(
                        minimum_on_ms=10000,
                        pir_hold_ms=30000,
                        sound_hold_ms=12000,
                        max_sound_bridge_ms=300000,
                        cooldown_ms=5000,
                        sound_factor=1.12,
                        telemetry_interval_ms=1000,
                        upload_batch_size=30,
                    ),
                )
            return ConfigResponse(
                device_id=device_id,
                revision=row.revision,
                created_at_ms=row.created_at_ms,
                created_by=row.created_by,
                config=PresenceConfig.model_validate_json(row.config_json),
            )

    def put_config(self, device_id: str, update: ConfigPut) -> ConfigResponse:
        now_ms = utc_now_ms()
        with self.database.session() as session:
            session.execute(text("BEGIN IMMEDIATE"))
            try:
                device = self._ensure_device(session, device_id, now_ms)
                session.flush()
                current_revision = device.desired_config_revision
                if update.base_revision != current_revision:
                    raise ConflictError(
                        "config base_revision does not match current revision "
                        f"{current_revision}"
                    )

                revision = current_revision + 1
                row = ConfigRevision(
                    device_id=device_id,
                    revision=revision,
                    created_at_ms=now_ms,
                    created_by=update.created_by,
                    config_json=update.config.model_dump_json(),
                )
                session.add(row)
                device.desired_config_revision = revision
                session.commit()
            except Exception:
                session.rollback()
                raise

            return ConfigResponse(
                device_id=device_id,
                revision=revision,
                created_at_ms=now_ms,
                created_by=update.created_by,
                config=update.config,
            )
