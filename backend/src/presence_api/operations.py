from __future__ import annotations

import base64
import binascii
import hashlib
import json
import uuid
from collections.abc import Sequence
from pathlib import Path

from pydantic import TypeAdapter
from sqlalchemy import func, select, text

from .coredump import symbolize_coredump
from .database import Database
from .models import (
    CommandAck,
    CoreDump,
    Device,
    DeviceCommand,
    DeviceReleaseTarget,
    FirmwareRelease,
    OperationalLog,
    OperationalLogBatch,
    TelemetryRecord,
)
from .models import (
    DeviceHealthReport as DeviceHealthRow,
)
from .models import (
    ReleaseStatusReport as ReleaseStatusRow,
)
from .ota_bundle import ReleaseBundleError, verify_release_bundle
from .schemas import (
    CommandAckResponse,
    CommandOut,
    CommandStatus,
    ConsoleCommandCreate,
    ControlPollResponse,
    CoreDumpIn,
    CoreDumpIngestResponse,
    DesiredRelease,
    DeviceCommandAck,
    DeviceHealthOut,
    DeviceHealthPage,
    DeviceHealthReport,
    DeviceReleaseOverview,
    DeviceReleaseSelection,
    DeviceReleaseTargetOut,
    HealthIngestResponse,
    LeasedCommand,
    OperationalLogBatchIn,
    OperationalLogIngestResponse,
    OperationalLogOut,
    OperationalLogPage,
    ReleaseBundleImport,
    ReleaseStatusIn,
    ReleaseStatusOut,
    ReleaseStatusResponse,
    ReleaseSummary,
    RemoteCommand,
    SanitizedCoreDumpPage,
    SanitizedCoreDumpSummary,
)
from .service import ConflictError, NotFoundError, canonical_hash, utc_now_ms

ONLINE_THRESHOLD_MS = 120_000
HEALTH_HISTORY_LIMIT = 1_440
RELEASE_STATUS_HISTORY_LIMIT = 256
COMMAND_HISTORY_LIMIT = 256
OUTSTANDING_COMMAND_LIMIT = 64
COMMAND_LEASE_MS = 15_000
OPERATIONAL_LOG_LIMIT = 10_000
OPERATIONAL_LOG_BATCH_LIMIT = 4_096
COREDUMP_LIMIT = 10
_REMOTE_COMMAND_ADAPTER = TypeAdapter(RemoteCommand)
_TERMINAL_COMMAND_STATUSES = {
    CommandStatus.succeeded.value,
    CommandStatus.failed.value,
    CommandStatus.expired.value,
    CommandStatus.rejected.value,
}
_FAILED_TERMINAL_RELEASE_PHASES = {"failed", "rejected", "rolled_back"}
_COMMAND_STATUS_RANK = {
    CommandStatus.queued.value: 0,
    CommandStatus.leased.value: 0,
    CommandStatus.accepted.value: 1,
    CommandStatus.running.value: 2,
    CommandStatus.succeeded.value: 3,
    CommandStatus.failed.value: 3,
    CommandStatus.expired.value: 3,
    CommandStatus.rejected.value: 3,
}


class OperationsService:
    def __init__(
        self,
        database: Database,
        trusted_ota_keys: tuple[tuple[str, Path], ...] = (),
        coredump_decoder: Path | None = None,
    ) -> None:
        self.database = database
        self.trusted_ota_keys = dict(trusted_ota_keys)
        self.coredump_decoder = coredump_decoder

    @staticmethod
    def _ensure_device(
        session, device_id: str, now_ms: int, firmware_version: str | None = None
    ) -> Device:
        device = session.get(Device, device_id)
        if device is None:
            device = Device(
                device_id=device_id,
                created_at_ms=now_ms,
                last_seen_at_ms=now_ms,
                firmware_version=firmware_version,
                desired_config_revision=0,
                applied_config_revision=0,
                confirmed_release_counter=0,
            )
            session.add(device)
        else:
            device.last_seen_at_ms = now_ms
            if firmware_version is not None:
                device.firmware_version = firmware_version
        return device

    @staticmethod
    def _prune_rows(session, rows: Sequence[object], retain: int) -> None:
        for row in rows[retain:]:
            session.delete(row)

    @classmethod
    def _prune_terminal_commands(cls, session, device_id: str) -> None:
        terminal_rows = session.scalars(
            select(DeviceCommand)
            .where(
                DeviceCommand.device_id == device_id,
                DeviceCommand.status.in_(tuple(_TERMINAL_COMMAND_STATUSES)),
            )
            .order_by(
                func.coalesce(
                    DeviceCommand.latest_ack_at_ms,
                    DeviceCommand.created_at_ms,
                ).desc(),
                DeviceCommand.command_id.desc(),
            )
        ).all()
        cls._prune_rows(session, terminal_rows, COMMAND_HISTORY_LIMIT)

    @staticmethod
    def _health_out(row: DeviceHealthRow) -> DeviceHealthOut:
        payload = json.loads(row.snapshot_json)
        payload["received_at_ms"] = row.received_at_ms
        return DeviceHealthOut.model_validate(payload)

    def ingest_health(
        self, device_id: str, report: DeviceHealthReport
    ) -> HealthIngestResponse:
        if report.device_id != device_id:
            raise ConflictError("health device_id does not match request path")
        now_ms = utc_now_ms()
        body_hash = canonical_hash(report)
        key = (device_id, report.boot_id, report.sequence)
        with self.database.session() as session:
            session.execute(text("BEGIN IMMEDIATE"))
            existing = session.get(DeviceHealthRow, key)
            if existing is not None:
                if existing.body_hash != body_hash:
                    raise ConflictError(
                        "health sequence already exists with a different payload"
                    )
                retained = session.scalar(
                    select(func.count())
                    .select_from(DeviceHealthRow)
                    .where(DeviceHealthRow.device_id == device_id)
                )
                return HealthIngestResponse(
                    device_id=device_id,
                    boot_id=report.boot_id,
                    sequence=report.sequence,
                    duplicate=True,
                    server_utc_ms=now_ms,
                    retained_reports=retained or 0,
                )
            try:
                self._ensure_device(session, device_id, now_ms, report.firmware_version)
                session.flush()
                session.add(
                    DeviceHealthRow(
                        device_id=device_id,
                        boot_id=report.boot_id,
                        sequence=report.sequence,
                        received_at_ms=now_ms,
                        uptime_ms=report.uptime_ms,
                        local_level=report.level.value,
                        firmware_version=report.firmware_version,
                        build_id=report.build_id,
                        body_hash=body_hash,
                        snapshot_json=report.model_dump_json(),
                    )
                )
                session.flush()
                rows = session.scalars(
                    select(DeviceHealthRow)
                    .where(DeviceHealthRow.device_id == device_id)
                    .order_by(
                        DeviceHealthRow.received_at_ms.desc(),
                        DeviceHealthRow.boot_id.desc(),
                        DeviceHealthRow.sequence.desc(),
                    )
                ).all()
                self._prune_rows(session, rows, HEALTH_HISTORY_LIMIT)
                retained = min(len(rows), HEALTH_HISTORY_LIMIT)
                session.commit()
            except Exception:
                session.rollback()
                raise
        return HealthIngestResponse(
            device_id=device_id,
            boot_id=report.boot_id,
            sequence=report.sequence,
            duplicate=False,
            server_utc_ms=now_ms,
            retained_reports=retained,
        )

    def health_page(self, device_id: str, limit: int = 120) -> DeviceHealthPage:
        now_ms = utc_now_ms()
        with self.database.session() as session:
            history_rows = session.scalars(
                select(DeviceHealthRow)
                .where(DeviceHealthRow.device_id == device_id)
                .order_by(
                    DeviceHealthRow.received_at_ms.desc(),
                    DeviceHealthRow.boot_id.desc(),
                    DeviceHealthRow.sequence.desc(),
                )
                .limit(limit)
            ).all()
            telemetry_at = session.scalar(
                select(func.max(TelemetryRecord.received_at_ms)).where(
                    TelemetryRecord.device_id == device_id
                )
            )
        health_at = history_rows[0].received_at_ms if history_rows else None
        activity_values = [
            value for value in (health_at, telemetry_at) if value is not None
        ]
        last_activity = max(activity_values) if activity_values else None
        return DeviceHealthPage(
            latest=self._health_out(history_rows[0]) if history_rows else None,
            history=[self._health_out(row) for row in history_rows],
            server_online=(
                last_activity is not None
                and max(0, now_ms - last_activity) <= ONLINE_THRESHOLD_MS
            ),
            online_threshold_ms=ONLINE_THRESHOLD_MS,
            last_activity_at_ms=last_activity,
        )

    @staticmethod
    def _release_summary(row: FirmwareRelease) -> ReleaseSummary:
        return ReleaseSummary(
            release_id=row.release_id,
            hardware_model=row.hardware_model,
            firmware_version=row.firmware_version,
            release_counter=row.release_counter,
            build_id=row.build_id,
            image_size=row.image_size,
            image_sha256=row.image_sha256,
            elf_size=row.elf_size,
            elf_sha256=row.elf_sha256,
            key_id=row.key_id,
            signature_format=row.signature_format,
            imported_at_ms=row.imported_at_ms,
            imported_by=row.imported_by,
            verified=row.verified,
        )

    @classmethod
    def _desired_release(cls, device_id: str, row: FirmwareRelease) -> DesiredRelease:
        summary = cls._release_summary(row).model_dump(exclude={"elf_size"})
        base = f"/v1/devices/{device_id}/releases/{row.release_id}"
        return DesiredRelease(
            **summary,
            manifest_url=f"{base}/manifest",
            image_url=f"{base}/image",
        )

    def import_release(self, request: ReleaseBundleImport) -> ReleaseSummary:
        if not self.trusted_ota_keys:
            raise ReleaseBundleError("backend OTA trust set is not configured")
        verified = verify_release_bundle(
            request.bundle_base64,
            self.trusted_ota_keys,
        )
        now_ms = utc_now_ms()
        manifest_json = json.dumps(
            {
                "hardware": verified.hardware,
                "firmware_version": verified.firmware_version,
                "release_counter": verified.release_counter,
                "build_id": verified.build_id,
                "signing_key_id": verified.signing_key_id,
                "firmware_size": verified.firmware_size,
                "firmware_sha256": verified.firmware_sha256,
                "elf_size": verified.elf_size,
                "elf_sha256": verified.elf_sha256,
            },
            separators=(",", ":"),
            sort_keys=True,
        )
        with self.database.session() as session:
            session.execute(text("BEGIN IMMEDIATE"))
            existing = session.get(FirmwareRelease, verified.release_id)
            if existing is not None:
                if (
                    existing.signed_record != verified.manifest
                    or existing.signature != verified.signature
                    or existing.image_sha256 != verified.firmware_sha256
                    or existing.elf_sha256 != verified.elf_sha256
                ):
                    raise ConflictError(
                        "release_id already exists with different artifacts"
                    )
                return self._release_summary(existing)
            counter_conflict = session.scalars(
                select(FirmwareRelease).where(
                    FirmwareRelease.hardware_model == verified.hardware,
                    FirmwareRelease.release_counter == verified.release_counter,
                )
            ).first()
            if counter_conflict is not None:
                raise ConflictError(
                    "release counter already belongs to a different bundle"
                )
            build_conflict = session.scalars(
                select(FirmwareRelease).where(
                    FirmwareRelease.build_id == verified.build_id
                )
            ).first()
            if build_conflict is not None:
                raise ConflictError("build ID already belongs to a different bundle")
            row = FirmwareRelease(
                release_id=verified.release_id,
                hardware_model=verified.hardware,
                firmware_version=verified.firmware_version,
                release_counter=verified.release_counter,
                build_id=verified.build_id,
                image_size=verified.firmware_size,
                image_sha256=verified.firmware_sha256,
                elf_size=verified.elf_size,
                elf_sha256=verified.elf_sha256,
                key_id=verified.signing_key_id,
                signature_format="ecdsa-p256-sha256-raw",
                signed_record=verified.manifest,
                signature=verified.signature,
                manifest_json=manifest_json,
                image_blob=verified.firmware,
                elf_blob=verified.elf,
                imported_at_ms=now_ms,
                imported_by=request.imported_by,
                verified=True,
            )
            try:
                session.add(row)
                session.commit()
            except Exception:
                session.rollback()
                raise
        return self._release_summary(row)

    def list_releases(self) -> list[ReleaseSummary]:
        with self.database.session() as session:
            rows = session.scalars(
                select(FirmwareRelease).order_by(
                    FirmwareRelease.release_counter.desc(),
                    FirmwareRelease.imported_at_ms.desc(),
                )
            ).all()
        return [self._release_summary(row) for row in rows]

    @classmethod
    def _release_target_out(
        cls, target: DeviceReleaseTarget, release: FirmwareRelease
    ) -> DeviceReleaseTargetOut:
        return DeviceReleaseTargetOut(
            device_id=target.device_id,
            release=cls._release_summary(release),
            selected_at_ms=target.selected_at_ms,
            selected_by=target.selected_by,
            completed_at_ms=target.completed_at_ms,
        )

    def select_release(
        self, device_id: str, selection: DeviceReleaseSelection
    ) -> DeviceReleaseTargetOut:
        now_ms = utc_now_ms()
        with self.database.session() as session:
            session.execute(text("BEGIN IMMEDIATE"))
            release = session.get(FirmwareRelease, selection.release_id)
            if release is None or not release.verified:
                raise NotFoundError("verified firmware release was not found")
            device = self._ensure_device(session, device_id, now_ms)

            latest_status = session.scalars(
                select(ReleaseStatusRow)
                .where(ReleaseStatusRow.device_id == device_id)
                .order_by(ReleaseStatusRow.received_at_ms.desc())
                .limit(1)
            ).first()
            installed_ids: set[str] = set()
            if latest_status is not None:
                installed_ids = {
                    value
                    for value in (
                        latest_status.running_release_id,
                        latest_status.last_known_good_release_id,
                    )
                    if value is not None
                }
            installed_counters = [
                installed.release_counter
                for release_id in installed_ids
                if (installed := session.get(FirmwareRelease, release_id)) is not None
            ]
            if (device.confirmed_release_counter or 0) > 0:
                installed_counters.append(device.confirmed_release_counter)
            if installed_counters and release.release_counter <= max(
                installed_counters
            ):
                raise ConflictError(
                    "wireless release counter must be newer than the installed release"
                )

            target = session.get(DeviceReleaseTarget, device_id)
            if (
                target is not None
                and target.release_id == release.release_id
                and target.completed_at_ms is None
            ):
                return self._release_target_out(target, release)
            try:
                if target is None:
                    target = DeviceReleaseTarget(
                        device_id=device_id,
                        release_id=release.release_id,
                        selected_at_ms=now_ms,
                        selected_by=selection.selected_by,
                    )
                    session.add(target)
                else:
                    target.release_id = release.release_id
                    target.selected_at_ms = now_ms
                    target.selected_by = selection.selected_by
                    target.completed_at_ms = None
                session.commit()
            except Exception:
                session.rollback()
                raise
        return self._release_target_out(target, release)

    def get_release_target(self, device_id: str) -> DeviceReleaseTargetOut | None:
        with self.database.session() as session:
            target = session.get(DeviceReleaseTarget, device_id)
            if target is None:
                return None
            release = session.get(FirmwareRelease, target.release_id)
            if release is None:
                return None
            return self._release_target_out(target, release)

    def release_overview(self, device_id: str) -> DeviceReleaseOverview:
        target = self.get_release_target(device_id)
        with self.database.session() as session:
            row = session.scalars(
                select(ReleaseStatusRow)
                .where(ReleaseStatusRow.device_id == device_id)
                .order_by(ReleaseStatusRow.received_at_ms.desc())
                .limit(1)
            ).first()
        status = None
        if row is not None:
            status = ReleaseStatusOut(
                status_id=row.status_id,
                device_id=row.device_id,
                received_at_ms=row.received_at_ms,
                desired_release_id=row.desired_release_id,
                running_release_id=row.running_release_id,
                previous_release_id=row.previous_release_id,
                last_known_good_release_id=row.last_known_good_release_id,
                phase=row.phase,
                progress_percent=row.progress_percent,
                last_error=row.last_error,
                rollback_outcome=row.rollback_outcome,
                firmware_version=row.firmware_version,
                build_id=row.build_id,
            )
        return DeviceReleaseOverview(target=target, latest_status=status)

    def release_manifest_for_device(self, device_id: str, release_id: str) -> bytes:
        with self.database.session() as session:
            target = session.get(DeviceReleaseTarget, device_id)
            if target is None or target.release_id != release_id:
                raise NotFoundError("release is not selected for device")
            release = session.get(FirmwareRelease, release_id)
            if release is None or not release.verified:
                raise NotFoundError("verified firmware release was not found")
            return release.signed_record + release.signature

    def release_image_for_device(self, device_id: str, release_id: str) -> bytes:
        with self.database.session() as session:
            target = session.get(DeviceReleaseTarget, device_id)
            if target is None or target.release_id != release_id:
                raise NotFoundError("release is not selected for device")
            release = session.get(FirmwareRelease, release_id)
            if release is None or not release.verified:
                raise NotFoundError("verified firmware release was not found")
            return release.image_blob

    @staticmethod
    def _command_out(row: DeviceCommand) -> CommandOut:
        result = (
            json.loads(row.latest_result_json)
            if row.latest_result_json is not None
            else None
        )
        return CommandOut(
            command_id=row.command_id,
            device_id=row.device_id,
            created_at_ms=row.created_at_ms,
            expires_at_ms=row.expires_at_ms,
            created_by=row.created_by,
            status=row.status,
            delivery_attempts=row.delivery_attempts,
            latest_ack_at_ms=row.latest_ack_at_ms,
            latest_result=result,
            command=_REMOTE_COMMAND_ADAPTER.validate_json(row.payload_json),
        )

    def create_command(
        self, device_id: str, request: ConsoleCommandCreate
    ) -> CommandOut:
        now_ms = utc_now_ms()
        command_id = "cmd-" + uuid.uuid4().hex
        payload = request.command.model_dump(mode="json")
        row = DeviceCommand(
            command_id=command_id,
            device_id=device_id,
            created_at_ms=now_ms,
            expires_at_ms=now_ms + request.expires_in_seconds * 1_000,
            created_by=request.created_by,
            action=payload["action"],
            payload_json=json.dumps(payload, separators=(",", ":"), sort_keys=True),
            status=CommandStatus.queued.value,
            delivery_attempts=0,
        )
        with self.database.session() as session:
            session.execute(text("BEGIN IMMEDIATE"))
            try:
                self._ensure_device(session, device_id, now_ms)
                stale_pending = session.scalars(
                    select(DeviceCommand).where(
                        DeviceCommand.device_id == device_id,
                        DeviceCommand.status.in_(("queued", "leased")),
                        DeviceCommand.expires_at_ms <= now_ms,
                    )
                ).all()
                for stale in stale_pending:
                    stale.status = CommandStatus.expired.value
                    stale.latest_ack_at_ms = now_ms

                outstanding = session.scalar(
                    select(func.count())
                    .select_from(DeviceCommand)
                    .where(
                        DeviceCommand.device_id == device_id,
                        ~DeviceCommand.status.in_(tuple(_TERMINAL_COMMAND_STATUSES)),
                    )
                )
                if (outstanding or 0) >= OUTSTANDING_COMMAND_LIMIT:
                    raise ConflictError("device has too many outstanding commands")
                session.add(row)
                session.flush()
                self._prune_terminal_commands(session, device_id)
                session.commit()
            except Exception:
                session.rollback()
                raise
        return self._command_out(row)

    def list_commands(self, device_id: str, limit: int = 50) -> list[CommandOut]:
        with self.database.session() as session:
            rows = session.scalars(
                select(DeviceCommand)
                .where(DeviceCommand.device_id == device_id)
                .order_by(DeviceCommand.created_at_ms.desc())
                .limit(limit)
            ).all()
        return [self._command_out(row) for row in rows]

    def poll_control(self, device_id: str) -> ControlPollResponse:
        now_ms = utc_now_ms()
        desired = None
        leased = None
        with self.database.session() as session:
            session.execute(text("BEGIN IMMEDIATE"))
            try:
                self._ensure_device(session, device_id, now_ms)
                target = session.get(DeviceReleaseTarget, device_id)
                if target is not None and target.completed_at_ms is None:
                    release = session.get(FirmwareRelease, target.release_id)
                    if release is not None and release.verified:
                        desired = self._desired_release(device_id, release)

                candidates = session.scalars(
                    select(DeviceCommand)
                    .where(
                        DeviceCommand.device_id == device_id,
                        DeviceCommand.status.in_(("queued", "leased")),
                    )
                    .order_by(DeviceCommand.created_at_ms, DeviceCommand.command_id)
                ).all()
                command_row = None
                for candidate in candidates:
                    if candidate.expires_at_ms <= now_ms:
                        candidate.status = CommandStatus.expired.value
                        candidate.latest_ack_at_ms = now_ms
                        continue
                    command_row = candidate
                    break

                if command_row is not None:
                    if (
                        command_row.status == CommandStatus.queued.value
                        or command_row.lease_expires_at_ms is None
                        or command_row.lease_expires_at_ms <= now_ms
                    ):
                        command_row.status = CommandStatus.leased.value
                        command_row.lease_id = "lease-" + uuid.uuid4().hex
                        command_row.lease_expires_at_ms = min(
                            now_ms + COMMAND_LEASE_MS,
                            command_row.expires_at_ms,
                        )
                        command_row.delivery_attempts += 1
                    leased = LeasedCommand(
                        command_id=command_row.command_id,
                        created_at_ms=command_row.created_at_ms,
                        expires_at_ms=command_row.expires_at_ms,
                        lease_id=command_row.lease_id,
                        lease_expires_at_ms=command_row.lease_expires_at_ms,
                        delivery_attempt=command_row.delivery_attempts,
                        command=_REMOTE_COMMAND_ADAPTER.validate_json(
                            command_row.payload_json
                        ),
                    )
                self._prune_terminal_commands(session, device_id)
                session.commit()
            except Exception:
                session.rollback()
                raise
        return ControlPollResponse(
            server_utc_ms=now_ms,
            desired_release=desired,
            command=leased,
        )

    def acknowledge_command(
        self, device_id: str, acknowledgement: DeviceCommandAck
    ) -> CommandAckResponse:
        now_ms = utc_now_ms()
        body_hash = canonical_hash(acknowledgement)
        with self.database.session() as session:
            session.execute(text("BEGIN IMMEDIATE"))
            existing = session.get(CommandAck, acknowledgement.ack_id)
            if existing is not None:
                if (
                    existing.device_id != device_id
                    or existing.command_id != acknowledgement.command_id
                    or existing.body_hash != body_hash
                ):
                    raise ConflictError(
                        "ack_id already exists with a different payload"
                    )
                command = session.get(DeviceCommand, acknowledgement.command_id)
                return CommandAckResponse(
                    ack_id=existing.ack_id,
                    command_id=existing.command_id,
                    status=command.status,
                    duplicate=True,
                    server_utc_ms=now_ms,
                )

            command = session.get(DeviceCommand, acknowledgement.command_id)
            if command is None or command.device_id != device_id:
                raise NotFoundError("command was not found for device")
            if command.lease_id != acknowledgement.lease_id:
                raise ConflictError("command lease is not current")
            if command.status in {
                CommandStatus.queued.value,
                CommandStatus.leased.value,
            } and (
                command.expires_at_ms <= now_ms
                or command.lease_expires_at_ms is None
                or command.lease_expires_at_ms <= now_ms
            ):
                command.status = CommandStatus.expired.value
                command.latest_ack_at_ms = now_ms
                session.commit()
                raise ConflictError("command lease has expired")

            existing_status_ack = session.scalars(
                select(CommandAck).where(
                    CommandAck.command_id == command.command_id,
                    CommandAck.status == acknowledgement.status,
                )
            ).first()
            if existing_status_ack is not None:
                raise ConflictError(
                    "command status already has a different acknowledgement"
                )

            incoming_status = acknowledgement.status
            current_rank = _COMMAND_STATUS_RANK[command.status]
            incoming_rank = _COMMAND_STATUS_RANK[incoming_status]
            if incoming_rank < current_rank:
                raise ConflictError("command status cannot move backwards")
            if (
                command.status in _TERMINAL_COMMAND_STATUSES
                and incoming_status != command.status
            ):
                raise ConflictError("command already has a different terminal status")

            result_json = (
                json.dumps(
                    acknowledgement.result,
                    separators=(",", ":"),
                    sort_keys=True,
                )
                if acknowledgement.result is not None
                else None
            )
            try:
                session.add(
                    CommandAck(
                        ack_id=acknowledgement.ack_id,
                        command_id=command.command_id,
                        device_id=device_id,
                        received_at_ms=now_ms,
                        status=incoming_status,
                        body_hash=body_hash,
                        result_json=result_json,
                    )
                )
                command.status = incoming_status
                command.latest_ack_at_ms = now_ms
                command.latest_result_json = result_json
                if incoming_status in _TERMINAL_COMMAND_STATUSES:
                    self._prune_terminal_commands(session, device_id)
                session.commit()
            except Exception:
                session.rollback()
                raise
        return CommandAckResponse(
            ack_id=acknowledgement.ack_id,
            command_id=command.command_id,
            status=command.status,
            duplicate=False,
            server_utc_ms=now_ms,
        )

    def report_release_status(
        self, device_id: str, report: ReleaseStatusIn
    ) -> ReleaseStatusResponse:
        now_ms = utc_now_ms()
        body_hash = canonical_hash(report)
        with self.database.session() as session:
            session.execute(text("BEGIN IMMEDIATE"))
            existing = session.get(ReleaseStatusRow, report.status_id)
            if existing is not None:
                if existing.device_id != device_id or existing.body_hash != body_hash:
                    raise ConflictError(
                        "status_id already exists with a different payload"
                    )
                return ReleaseStatusResponse(
                    status_id=report.status_id,
                    duplicate=True,
                    server_utc_ms=now_ms,
                    desired_release_completed=existing.desired_release_completed,
                )

            referenced = {
                release_id
                for release_id in (
                    report.desired_release_id,
                    report.running_release_id,
                    report.previous_release_id,
                    report.last_known_good_release_id,
                )
                if release_id is not None
            }
            for release_id in referenced:
                if session.get(FirmwareRelease, release_id) is None:
                    raise ConflictError(f"unknown firmware release: {release_id}")

            running_release = None
            if report.phase.value == "running":
                if report.running_release_id is None:
                    raise ConflictError("running phase requires running_release_id")
                running_release = session.get(
                    FirmwareRelease, report.running_release_id
                )
                if (
                    running_release is None
                    or report.firmware_version != running_release.firmware_version
                    or report.build_id != running_release.build_id
                ):
                    raise ConflictError(
                        "running release identity does not match build and version"
                    )

            try:
                device = self._ensure_device(
                    session, device_id, now_ms, report.firmware_version
                )
                row = ReleaseStatusRow(
                    status_id=report.status_id,
                    device_id=device_id,
                    received_at_ms=now_ms,
                    desired_release_id=report.desired_release_id,
                    running_release_id=report.running_release_id,
                    previous_release_id=report.previous_release_id,
                    last_known_good_release_id=report.last_known_good_release_id,
                    phase=report.phase.value,
                    progress_percent=report.progress_percent,
                    last_error=report.last_error,
                    rollback_outcome=report.rollback_outcome.value,
                    firmware_version=report.firmware_version,
                    build_id=report.build_id,
                    body_hash=body_hash,
                    desired_release_completed=False,
                )
                session.add(row)
                target = session.get(DeviceReleaseTarget, device_id)
                completed = False
                target_release = (
                    session.get(FirmwareRelease, target.release_id)
                    if target is not None
                    else None
                )
                failed_target = (
                    target is not None
                    and report.desired_release_id == target.release_id
                    and report.phase.value in _FAILED_TERMINAL_RELEASE_PHASES
                )
                running_target = (
                    target is not None
                    and target_release is not None
                    and report.desired_release_id == target.release_id
                    and report.running_release_id == target.release_id
                    and report.phase.value == "running"
                    and report.firmware_version == target_release.firmware_version
                    and report.build_id == target_release.build_id
                )
                if failed_target or running_target:
                    target.completed_at_ms = now_ms
                    completed = True
                if running_release is not None:
                    device.confirmed_release_counter = max(
                        device.confirmed_release_counter or 0,
                        running_release.release_counter,
                    )
                row.desired_release_completed = completed
                session.flush()
                old_rows = session.scalars(
                    select(ReleaseStatusRow)
                    .where(ReleaseStatusRow.device_id == device_id)
                    .order_by(ReleaseStatusRow.received_at_ms.desc())
                ).all()
                self._prune_rows(session, old_rows, RELEASE_STATUS_HISTORY_LIMIT)
                session.commit()
            except Exception:
                session.rollback()
                raise
        return ReleaseStatusResponse(
            status_id=report.status_id,
            duplicate=False,
            server_utc_ms=now_ms,
            desired_release_completed=completed,
        )

    def ingest_operational_logs(
        self, device_id: str, batch: OperationalLogBatchIn
    ) -> OperationalLogIngestResponse:
        now_ms = utc_now_ms()
        body_hash = canonical_hash(batch)
        with self.database.session() as session:
            session.execute(text("BEGIN IMMEDIATE"))
            existing_batch = session.get(
                OperationalLogBatch, (device_id, batch.batch_id)
            )
            if existing_batch is not None:
                if existing_batch.body_hash != body_hash:
                    raise ConflictError(
                        "log batch_id already exists with a different payload"
                    )
                retained = session.scalar(
                    select(func.count())
                    .select_from(OperationalLog)
                    .where(OperationalLog.device_id == device_id)
                )
                return OperationalLogIngestResponse(
                    batch_id=batch.batch_id,
                    stored=0,
                    duplicates=len(batch.records),
                    server_utc_ms=now_ms,
                    retained_records=retained or 0,
                )

            stored = 0
            duplicates = 0
            try:
                self._ensure_device(session, device_id, now_ms)
                session.flush()
                for record in batch.records:
                    payload_hash = canonical_hash(record)
                    key = (device_id, batch.boot_id, record.sequence)
                    existing = session.get(OperationalLog, key)
                    if existing is not None:
                        if existing.payload_hash != payload_hash:
                            raise ConflictError(
                                "log identity already exists with a different payload"
                            )
                        duplicates += 1
                        continue
                    session.add(
                        OperationalLog(
                            device_id=device_id,
                            boot_id=batch.boot_id,
                            sequence=record.sequence,
                            build_id=batch.build_id,
                            uptime_ms=record.uptime_ms,
                            received_at_ms=now_ms,
                            level=record.level.value,
                            event_type=record.event_type,
                            message=record.message,
                            fields_json=json.dumps(
                                record.fields,
                                separators=(",", ":"),
                                sort_keys=True,
                            ),
                            payload_hash=payload_hash,
                        )
                    )
                    stored += 1
                session.add(
                    OperationalLogBatch(
                        device_id=device_id,
                        batch_id=batch.batch_id,
                        boot_id=batch.boot_id,
                        build_id=batch.build_id,
                        received_at_ms=now_ms,
                        body_hash=body_hash,
                        record_count=len(batch.records),
                    )
                )
                session.flush()
                log_rows = session.scalars(
                    select(OperationalLog)
                    .where(OperationalLog.device_id == device_id)
                    .order_by(
                        OperationalLog.received_at_ms.desc(),
                        OperationalLog.boot_id.desc(),
                        OperationalLog.sequence.desc(),
                    )
                ).all()
                self._prune_rows(session, log_rows, OPERATIONAL_LOG_LIMIT)
                batch_rows = session.scalars(
                    select(OperationalLogBatch)
                    .where(OperationalLogBatch.device_id == device_id)
                    .order_by(OperationalLogBatch.received_at_ms.desc())
                ).all()
                self._prune_rows(session, batch_rows, OPERATIONAL_LOG_BATCH_LIMIT)
                retained = min(len(log_rows), OPERATIONAL_LOG_LIMIT)
                session.commit()
            except Exception:
                session.rollback()
                raise
        return OperationalLogIngestResponse(
            batch_id=batch.batch_id,
            stored=stored,
            duplicates=duplicates,
            server_utc_ms=now_ms,
            retained_records=retained,
        )

    def list_operational_logs(
        self,
        device_id: str,
        limit: int = 200,
        since_ms: int | None = None,
    ) -> OperationalLogPage:
        statement = select(OperationalLog).where(OperationalLog.device_id == device_id)
        if since_ms is not None:
            statement = statement.where(OperationalLog.received_at_ms >= since_ms)
        statement = statement.order_by(
            OperationalLog.received_at_ms.desc(),
            OperationalLog.boot_id.desc(),
            OperationalLog.sequence.desc(),
        ).limit(limit + 1)
        with self.database.session() as session:
            rows = session.scalars(statement).all()
            retained = session.scalar(
                select(func.count())
                .select_from(OperationalLog)
                .where(OperationalLog.device_id == device_id)
            )
        return OperationalLogPage(
            items=[
                OperationalLogOut(
                    device_id=row.device_id,
                    boot_id=row.boot_id,
                    build_id=row.build_id,
                    sequence=row.sequence,
                    uptime_ms=row.uptime_ms,
                    received_at_ms=row.received_at_ms,
                    level=row.level,
                    event_type=row.event_type,
                    message=row.message,
                    fields=json.loads(row.fields_json),
                )
                for row in rows[:limit]
            ],
            retained_records=retained or 0,
            truncated=len(rows) > limit,
        )

    def ingest_coredump(
        self, device_id: str, report: CoreDumpIn
    ) -> CoreDumpIngestResponse:
        try:
            raw_dump = base64.b64decode(report.dump_base64, validate=True)
        except (binascii.Error, ValueError) as error:
            raise ConflictError("dump_base64 is invalid") from error
        if len(raw_dump) != report.dump_size:
            raise ConflictError("core dump byte count does not match dump_size")
        if hashlib.sha256(raw_dump).hexdigest() != report.dump_sha256:
            raise ConflictError("core dump SHA-256 does not match")
        now_ms = utc_now_ms()
        body_hash = canonical_hash(report)

        with self.database.session() as session:
            # A device retries after any lost/late HTTP response. Return a
            # previously committed receipt before invoking the optional
            # symbolizer, whose bounded runtime can exceed the firmware's HTTP
            # timeout. The write transaction below repeats this check to close
            # the race between concurrent first deliveries.
            existing = session.get(CoreDump, report.crash_id)
            if existing is not None:
                if existing.device_id != device_id or existing.body_hash != body_hash:
                    raise ConflictError(
                        "crash_id already exists with a different payload"
                    )
                return CoreDumpIngestResponse(
                    crash_id=report.crash_id,
                    duplicate=True,
                    durable=True,
                    server_utc_ms=now_ms,
                    symbolication_status=existing.symbolication_status,
                )
            release = session.scalars(
                select(FirmwareRelease).where(
                    FirmwareRelease.build_id == report.build_id
                )
            ).first()
            release_id = release.release_id if release is not None else None
            elf = release.elf_blob if release is not None else None
        if elf is None:
            symbolication_status = "missing_elf"
            summary = ["No verified release contains the exact build ID."]
        else:
            symbolication_status, summary = symbolize_coredump(
                self.coredump_decoder, raw_dump, elf
            )

        with self.database.session() as session:
            session.execute(text("BEGIN IMMEDIATE"))
            existing = session.get(CoreDump, report.crash_id)
            if existing is not None:
                if existing.device_id != device_id or existing.body_hash != body_hash:
                    raise ConflictError(
                        "crash_id already exists with a different payload"
                    )
                return CoreDumpIngestResponse(
                    crash_id=report.crash_id,
                    duplicate=True,
                    durable=True,
                    server_utc_ms=now_ms,
                    symbolication_status=existing.symbolication_status,
                )
            try:
                self._ensure_device(session, device_id, now_ms)
                session.flush()
                session.add(
                    CoreDump(
                        crash_id=report.crash_id,
                        device_id=device_id,
                        boot_id=report.boot_id,
                        build_id=report.build_id,
                        reset_reason=report.reset_reason,
                        dump_sha256=report.dump_sha256,
                        dump_size=report.dump_size,
                        received_at_ms=now_ms,
                        body_hash=body_hash,
                        raw_dump=raw_dump,
                        release_id=release_id,
                        symbolication_status=symbolication_status,
                        sanitized_summary_json=json.dumps(summary),
                    )
                )
                session.flush()
                rows = session.scalars(
                    select(CoreDump)
                    .where(CoreDump.device_id == device_id)
                    .order_by(CoreDump.received_at_ms.desc(), CoreDump.crash_id.desc())
                ).all()
                self._prune_rows(session, rows, COREDUMP_LIMIT)
                session.commit()
            except Exception:
                session.rollback()
                raise
        return CoreDumpIngestResponse(
            crash_id=report.crash_id,
            duplicate=False,
            durable=True,
            server_utc_ms=now_ms,
            symbolication_status=symbolication_status,
        )

    def list_coredump_summaries(
        self, device_id: str, limit: int = 10
    ) -> SanitizedCoreDumpPage:
        with self.database.session() as session:
            rows = session.scalars(
                select(CoreDump)
                .where(CoreDump.device_id == device_id)
                .order_by(CoreDump.received_at_ms.desc(), CoreDump.crash_id.desc())
                .limit(limit)
            ).all()
            retained = session.scalar(
                select(func.count())
                .select_from(CoreDump)
                .where(CoreDump.device_id == device_id)
            )
        return SanitizedCoreDumpPage(
            items=[
                SanitizedCoreDumpSummary(
                    crash_id=row.crash_id,
                    device_id=row.device_id,
                    boot_id=row.boot_id,
                    build_id=row.build_id,
                    reset_reason=row.reset_reason,
                    dump_sha256=row.dump_sha256,
                    dump_size=row.dump_size,
                    received_at_ms=row.received_at_ms,
                    release_id=row.release_id,
                    symbolication_status=row.symbolication_status,
                    summary=json.loads(row.sanitized_summary_json),
                )
                for row in rows
            ],
            retained_reports=retained or 0,
        )
