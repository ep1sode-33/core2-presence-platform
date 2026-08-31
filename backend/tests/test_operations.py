from __future__ import annotations

import base64
import hashlib
from copy import deepcopy

import pytest
from fastapi.testclient import TestClient
from sqlalchemy import func, select

from presence_api.config import Settings
from presence_api.database import Database
from presence_api.main import create_app
from presence_api.models import CommandAck, DeviceCommand, FirmwareRelease
from presence_api.operations import COMMAND_HISTORY_LIMIT, OperationsService
from presence_api.schemas import ConsoleCommandCreate, CoreDumpIn, DeviceCommandAck

DEVICE = "core2-2cbcbb81eb60"


@pytest.fixture
def health_report() -> dict:
    return {
        "schema_version": 1,
        "device_id": DEVICE,
        "boot_id": "boot000000000001",
        "firmware_version": "0.7.0",
        "build_id": "git.0123456789ab",
        "uptime_ms": 12_345,
        "sequence": 7,
        "level": "healthy",
        "reset_reason": "power_on",
        "boot_count": 4,
        "wifi": {
            "connected": True,
            "ip": "192.168.0.51",
            "rssi_dbm": -47,
            "reconnect_count": 1,
            "clock_synchronized": True,
        },
        "config": {
            "desired_revision": 3,
            "stored_revision": 3,
            "applied_revision": 3,
        },
        "freshness": {
            "last_telemetry_ack_ms": 500,
            "last_config_attempt_ms": 1_000,
            "last_room_fetch_ms": 2_000,
            "last_weather_fetch_ms": 3_000,
            "telemetry_ack_result": "ok",
            "config_result": "ok",
            "room_fetch_result": "ok",
            "weather_fetch_result": "retrying",
        },
        "tasks": {
            "main_heartbeat_ms": 4,
            "uploader_heartbeat_ms": 10,
            "main_stack_hwm": 3_000,
            "uploader_stack_hwm": 2_000,
        },
        "queues": {
            "telemetry_depth": 1,
            "telemetry_capacity": 64,
            "dropped_samples": 0,
            "dropped_critical": 0,
            "feedback_depth": 0,
            "feedback_capacity": 8,
            "feedback_dropped_full": 0,
            "feedback_rejected_invalid": 0,
        },
        "storage": {
            "filesystem_ready": True,
            "spool_files": 0,
            "feedback_wait_files": 0,
            "feedback_ready_files": 0,
            "dead_files": 0,
            "oldest_backlog_age_ms": 0,
            "littlefs_total_bytes": 1_000_000,
            "littlefs_used_bytes": 250_000,
            "littlefs_free_bytes": 750_000,
        },
        "memory": {
            "free_heap_bytes": 120_000,
            "min_free_heap_bytes": 90_000,
            "largest_free_block_bytes": 70_000,
        },
        "sensors": {
            "pir_status": "healthy",
            "mic_status": "healthy",
            "pir_only_mode": False,
        },
        "uploader_status": "ready",
        "ota": {"active": False, "state": "inactive"},
        "debug": {"active": False, "state": "inactive"},
        "safe_mode": False,
    }


def test_health_is_idempotent_and_available_as_bounded_history(
    client: TestClient, health_report: dict
) -> None:
    first = client.post(f"/v1/devices/{DEVICE}/health", json=health_report)
    duplicate = client.post(f"/v1/devices/{DEVICE}/health", json=health_report)

    assert first.status_code == 200, first.text
    assert first.json()["duplicate"] is False
    assert duplicate.status_code == 200
    assert duplicate.json()["duplicate"] is True

    page = client.get(f"/v1/devices/{DEVICE}/health", params={"limit": 1})
    assert page.status_code == 200
    assert page.json()["latest"]["level"] == "healthy"
    assert page.json()["latest"]["sensors"]["mic_status"] == "healthy"
    assert page.json()["server_online"] is True
    assert len(page.json()["history"]) == 1

    console_device = client.get("/v1/console/devices").json()["items"][0]
    assert console_device["online"] is True
    assert console_device["reported_health_level"] == "healthy"
    assert console_device["last_health_at_ms"] == first.json()["server_utc_ms"]

    changed = deepcopy(health_report)
    changed["level"] = "degraded"
    conflict = client.post(f"/v1/devices/{DEVICE}/health", json=changed)
    assert conflict.status_code == 409


def test_health_rejects_path_identity_mismatch(
    client: TestClient, health_report: dict
) -> None:
    health_report["device_id"] = "core2-000000000000"
    response = client.post(f"/v1/devices/{DEVICE}/health", json=health_report)
    assert response.status_code == 409


def test_one_shot_command_uses_stable_lease_and_idempotent_ack(
    client: TestClient,
) -> None:
    created = client.post(
        f"/v1/console/devices/{DEVICE}/commands",
        json={
            "command": {
                "action": "set_log_level",
                "level": "debug_sensor",
                "duration_seconds": 300,
            },
            "expires_in_seconds": 600,
            "created_by": "test-console",
        },
    )
    assert created.status_code == 200, created.text
    command_id = created.json()["command_id"]

    first_poll = client.get(f"/v1/devices/{DEVICE}/control")
    retry_poll = client.get(f"/v1/devices/{DEVICE}/control")
    assert first_poll.status_code == 200
    assert first_poll.json()["poll_after_ms"] == 5_000
    assert first_poll.json()["command"]["command_id"] == command_id
    assert (
        retry_poll.json()["command"]["lease_id"]
        == first_poll.json()["command"]["lease_id"]
    )
    lease_id = first_poll.json()["command"]["lease_id"]

    accepted = {
        "schema_version": 1,
        "ack_id": "ack-command-0001",
        "command_id": command_id,
        "lease_id": lease_id,
        "status": "accepted",
        "result": {"durably_recorded": True},
    }
    first_ack = client.post(f"/v1/devices/{DEVICE}/control/acks", json=accepted)
    duplicate_ack = client.post(f"/v1/devices/{DEVICE}/control/acks", json=accepted)
    assert first_ack.status_code == 200, first_ack.text
    assert first_ack.json()["duplicate"] is False
    assert duplicate_ack.json()["duplicate"] is True
    assert client.get(f"/v1/devices/{DEVICE}/control").json()["command"] is None

    succeeded = {
        **accepted,
        "ack_id": "ack-command-0002",
        "status": "succeeded",
        "result": {"level": "debug_sensor"},
    }
    final_ack = client.post(f"/v1/devices/{DEVICE}/control/acks", json=succeeded)
    assert final_ack.status_code == 200, final_ack.text
    assert final_ack.json()["status"] == "succeeded"

    commands = client.get(f"/v1/console/devices/{DEVICE}/commands").json()
    assert commands[0]["status"] == "succeeded"
    assert commands[0]["latest_result"] == {"level": "debug_sensor"}


def test_command_rejects_a_second_ack_id_for_the_same_status(
    client: TestClient,
) -> None:
    created = client.post(
        f"/v1/console/devices/{DEVICE}/commands",
        json={"command": {"action": "reboot"}},
    ).json()
    leased = client.get(f"/v1/devices/{DEVICE}/control").json()["command"]
    first = {
        "schema_version": 1,
        "ack_id": "ack-one-status-0001",
        "command_id": created["command_id"],
        "lease_id": leased["lease_id"],
        "status": "accepted",
        "result": {"durably_recorded": True},
    }
    assert (
        client.post(f"/v1/devices/{DEVICE}/control/acks", json=first).status_code == 200
    )
    second = {**first, "ack_id": "ack-one-status-0002"}
    rejected = client.post(f"/v1/devices/{DEVICE}/control/acks", json=second)
    assert rejected.status_code == 409


@pytest.mark.parametrize(
    "result",
    [
        {"k" * 65: "value"},
        {"key": "v" * 257},
        {f"key-{index}": "v" * 256 for index in range(24)},
    ],
)
def test_command_ack_result_has_per_field_and_serialized_bounds(
    client: TestClient, result: dict
) -> None:
    created = client.post(
        f"/v1/console/devices/{DEVICE}/commands",
        json={"command": {"action": "reboot"}},
    ).json()
    leased = client.get(f"/v1/devices/{DEVICE}/control").json()["command"]
    response = client.post(
        f"/v1/devices/{DEVICE}/control/acks",
        json={
            "schema_version": 1,
            "ack_id": "ack-oversized-result-0001",
            "command_id": created["command_id"],
            "lease_id": leased["lease_id"],
            "status": "accepted",
            "result": result,
        },
    )
    assert response.status_code == 422


def test_outstanding_command_queue_is_bounded(client: TestClient, monkeypatch) -> None:
    monkeypatch.setattr("presence_api.operations.OUTSTANDING_COMMAND_LIMIT", 2)
    request = {"command": {"action": "reboot"}}
    assert (
        client.post(f"/v1/console/devices/{DEVICE}/commands", json=request).status_code
        == 200
    )
    assert (
        client.post(f"/v1/console/devices/{DEVICE}/commands", json=request).status_code
        == 200
    )
    refused = client.post(f"/v1/console/devices/{DEVICE}/commands", json=request)
    assert refused.status_code == 409


def test_nonterminal_command_survives_terminal_history_pruning(
    tmp_path,
) -> None:
    database = Database(tmp_path / "commands.db")
    database.initialize()
    operations = OperationsService(database)
    try:
        original = operations.create_command(
            DEVICE,
            ConsoleCommandCreate.model_validate(
                {"command": {"action": "reboot"}, "expires_in_seconds": 600}
            ),
        )
        lease = operations.poll_control(DEVICE).command
        assert lease is not None
        operations.acknowledge_command(
            DEVICE,
            DeviceCommandAck.model_validate(
                {
                    "schema_version": 1,
                    "ack_id": "ack-old-accepted-0001",
                    "command_id": original.command_id,
                    "lease_id": lease.lease_id,
                    "status": "accepted",
                }
            ),
        )

        with database.session() as session:
            for index in range(COMMAND_HISTORY_LIMIT + 5):
                session.add(
                    DeviceCommand(
                        command_id=f"cmd-terminal-history-{index:04d}",
                        device_id=DEVICE,
                        created_at_ms=original.created_at_ms + index + 1,
                        expires_at_ms=original.expires_at_ms,
                        created_by="history-test",
                        action="reboot",
                        payload_json='{"action":"reboot"}',
                        status="succeeded",
                        delivery_attempts=1,
                    )
                )
            session.commit()

        # Creating another command invokes the retention policy. The accepted
        # command is nonterminal, so it must remain present and ACKable.
        operations.create_command(
            DEVICE,
            ConsoleCommandCreate.model_validate(
                {"command": {"action": "reboot"}, "expires_in_seconds": 600}
            ),
        )
        with database.session() as session:
            assert session.get(DeviceCommand, original.command_id) is not None
            terminal_count = session.scalar(
                select(func.count())
                .select_from(DeviceCommand)
                .where(
                    DeviceCommand.device_id == DEVICE,
                    DeviceCommand.status.in_(
                        ("succeeded", "failed", "expired", "rejected")
                    ),
                )
            )
        assert terminal_count == COMMAND_HISTORY_LIMIT

        completed = operations.acknowledge_command(
            DEVICE,
            DeviceCommandAck.model_validate(
                {
                    "schema_version": 1,
                    "ack_id": "ack-old-succeeded-0001",
                    "command_id": original.command_id,
                    "lease_id": lease.lease_id,
                    "status": "succeeded",
                }
            ),
        )
        assert completed.status.value == "succeeded"

        # Retention is based on recent terminal activity, not original command
        # creation time. The ACK and its command must survive so a lost HTTP
        # response can be retried idempotently.
        with database.session() as session:
            assert session.get(DeviceCommand, original.command_id) is not None
            assert session.get(CommandAck, "ack-old-succeeded-0001") is not None
        duplicate = operations.acknowledge_command(
            DEVICE,
            DeviceCommandAck.model_validate(
                {
                    "schema_version": 1,
                    "ack_id": "ack-old-succeeded-0001",
                    "command_id": original.command_id,
                    "lease_id": lease.lease_id,
                    "status": "succeeded",
                }
            ),
        )
        assert duplicate.duplicate is True
        assert duplicate.status.value == "succeeded"
    finally:
        database.dispose()


def test_short_command_lease_never_outlives_command(
    client: TestClient, monkeypatch
) -> None:
    now_ms = 1_800_000_000_000
    monkeypatch.setattr("presence_api.operations.utc_now_ms", lambda: now_ms)
    created = client.post(
        f"/v1/console/devices/{DEVICE}/commands",
        json={"command": {"action": "reboot"}, "expires_in_seconds": 5},
    )
    assert created.status_code == 200, created.text
    leased = client.get(f"/v1/devices/{DEVICE}/control").json()["command"]
    assert leased["lease_expires_at_ms"] == leased["expires_at_ms"]


def test_first_ack_after_lease_expiry_is_rejected_and_durably_expires_command(
    client: TestClient, monkeypatch
) -> None:
    clock = {"now_ms": 1_800_000_000_000}
    monkeypatch.setattr("presence_api.operations.utc_now_ms", lambda: clock["now_ms"])
    created = client.post(
        f"/v1/console/devices/{DEVICE}/commands",
        json={
            "command": {"action": "reboot"},
            "expires_in_seconds": 600,
        },
    )
    assert created.status_code == 200, created.text
    leased = client.get(f"/v1/devices/{DEVICE}/control").json()["command"]
    clock["now_ms"] = leased["lease_expires_at_ms"]
    late = client.post(
        f"/v1/devices/{DEVICE}/control/acks",
        json={
            "schema_version": 1,
            "ack_id": "ack-command-expired-0001",
            "command_id": leased["command_id"],
            "lease_id": leased["lease_id"],
            "status": "accepted",
            "result": {"durably_recorded": True},
        },
    )
    assert late.status_code == 409
    commands = client.get(f"/v1/console/devices/{DEVICE}/commands").json()
    assert commands[0]["status"] == "expired"


def test_operations_device_routes_keep_bearer_while_console_remains_lan_only(
    tmp_path, health_report: dict
) -> None:
    app = create_app(Settings(database_path=tmp_path / "secure.db", api_token="secret"))
    with TestClient(app, client=("192.168.0.42", 50_000)) as client:
        health = client.post(f"/v1/devices/{DEVICE}/health", json=health_report)
        assert health.status_code == 401
        assert client.get(f"/v1/devices/{DEVICE}/control").status_code == 401
        assert (
            client.get(
                f"/v1/console/devices/{DEVICE}/commands",
                headers={"Host": "192.168.0.46"},
            ).status_code
            == 200
        )


def test_structured_log_batches_are_idempotent_and_console_bounded(
    client: TestClient,
) -> None:
    batch = {
        "schema_version": 1,
        "batch_id": "log-boot000000000001-1",
        "boot_id": "boot000000000001",
        "build_id": "git.0123456789ab",
        "records": [
            {
                "sequence": 10,
                "uptime_ms": 5_000,
                "level": "warning",
                "event_type": "sensor_changed",
                "fields": {"value0": 1, "value1": 0},
            },
            {
                "sequence": 11,
                "uptime_ms": 5_100,
                "level": "info",
                "event_type": "recovery_action",
                "fields": {"value0": 2, "value1": 3},
            },
        ],
    }
    first = client.post(f"/v1/devices/{DEVICE}/logs/batches", json=batch)
    duplicate = client.post(f"/v1/devices/{DEVICE}/logs/batches", json=batch)
    assert first.status_code == 200, first.text
    assert first.json()["stored"] == 2
    assert duplicate.json()["duplicates"] == 2

    page = client.get(f"/v1/console/devices/{DEVICE}/logs", params={"limit": 1})
    assert page.status_code == 200
    assert page.json()["retained_records"] == 2
    assert page.json()["truncated"] is True
    assert page.json()["items"][0]["build_id"] == "git.0123456789ab"

    changed = deepcopy(batch)
    changed["records"][0]["fields"]["value0"] = 99
    conflict = client.post(f"/v1/devices/{DEVICE}/logs/batches", json=changed)
    assert conflict.status_code == 409


def test_operational_log_batch_id_matches_firmware_plus_boundary(
    client: TestClient,
) -> None:
    batch = {
        "schema_version": 1,
        "batch_id": "+" * 96,
        "boot_id": "boot000000000001",
        "build_id": "git.0123456789ab",
        "records": [
            {
                "sequence": 1,
                "uptime_ms": 5,
                "level": "info",
                "event_type": "boot",
                "fields": {},
            }
        ],
    }
    accepted = client.post(f"/v1/devices/{DEVICE}/logs/batches", json=batch)
    assert accepted.status_code == 200, accepted.text
    too_long = {**batch, "batch_id": "+" * 97}
    assert (
        client.post(f"/v1/devices/{DEVICE}/logs/batches", json=too_long).status_code
        == 422
    )


@pytest.mark.parametrize(
    "fields",
    [
        {"k" * 65: "value"},
        {"key": "v" * 257},
        {f"key-{index}": "v" * 256 for index in range(24)},
    ],
)
def test_operational_log_fields_have_per_field_and_serialized_bounds(
    client: TestClient, fields: dict
) -> None:
    response = client.post(
        f"/v1/devices/{DEVICE}/logs/batches",
        json={
            "schema_version": 1,
            "batch_id": "log-oversized-fields-0001",
            "boot_id": "boot000000000001",
            "build_id": "git.0123456789ab",
            "records": [
                {
                    "sequence": 1,
                    "uptime_ms": 5,
                    "level": "info",
                    "event_type": "boot",
                    "fields": fields,
                }
            ],
        },
    )
    assert response.status_code == 422


def test_coredump_is_durable_but_console_exposes_only_sanitized_summary(
    client: TestClient,
) -> None:
    raw_dump = b"binary-core-dump\x00contains-secret-material"
    report = {
        "schema_version": 1,
        "crash_id": "crash-boot000000000001",
        "boot_id": "boot000000000001",
        "build_id": "git.unknown-build",
        "reset_reason": "task_wdt",
        "dump_size": len(raw_dump),
        "dump_sha256": hashlib.sha256(raw_dump).hexdigest(),
        "dump_base64": base64.b64encode(raw_dump).decode(),
    }
    first = client.post(f"/v1/devices/{DEVICE}/coredumps", json=report)
    duplicate = client.post(f"/v1/devices/{DEVICE}/coredumps", json=report)
    assert first.status_code == 200, first.text
    assert first.json()["durable"] is True
    assert first.json()["symbolication_status"] == "missing_elf"
    assert duplicate.json()["duplicate"] is True

    response = client.get(f"/v1/console/devices/{DEVICE}/coredumps")
    assert response.status_code == 200
    serialized = response.text
    assert "dump_base64" not in serialized
    assert base64.b64encode(raw_dump).decode() not in serialized
    assert "contains-secret-material" not in serialized
    assert response.json()["items"][0]["dump_sha256"] == report["dump_sha256"]
    assert (
        client.get(
            f"/v1/console/devices/{DEVICE}/coredumps/{report['crash_id']}/raw"
        ).status_code
        == 404
    )


def test_coredump_duplicate_bypasses_slow_symbolizer(tmp_path, monkeypatch) -> None:
    database = Database(tmp_path / "coredump-duplicate.db")
    database.initialize()
    operations = OperationsService(database)
    raw_dump = b"retryable-core-dump"
    report = CoreDumpIn.model_validate(
        {
            "schema_version": 1,
            "crash_id": "crash-slow-decoder-0001",
            "boot_id": "boot000000000001",
            "build_id": "git.slow-decoder",
            "reset_reason": "task_wdt",
            "dump_size": len(raw_dump),
            "dump_sha256": hashlib.sha256(raw_dump).hexdigest(),
            "dump_base64": base64.b64encode(raw_dump).decode(),
        }
    )
    try:
        with database.session() as session:
            session.add(
                FirmwareRelease(
                    release_id="rel-slow-decoder",
                    hardware_model="m5go-classic-esp32-16m",
                    firmware_version="0.7.0",
                    release_counter=1,
                    build_id=report.build_id,
                    image_size=1,
                    image_sha256="0" * 64,
                    elf_size=3,
                    elf_sha256="1" * 64,
                    key_id="release-test",
                    signature_format="ecdsa-p256-sha256-raw",
                    signed_record=b"manifest",
                    signature=b"s" * 64,
                    manifest_json="{}",
                    image_blob=b"i",
                    elf_blob=b"elf",
                    imported_at_ms=1,
                    imported_by="test",
                    verified=True,
                )
            )
            session.commit()

        calls = 0

        def symbolize_once(_decoder, _dump, _elf):
            nonlocal calls
            calls += 1
            return "succeeded", ["decoded"]

        monkeypatch.setattr(
            "presence_api.operations.symbolize_coredump", symbolize_once
        )
        first = operations.ingest_coredump(DEVICE, report)
        assert first.duplicate is False
        assert calls == 1

        def symbolizer_must_not_run(_decoder, _dump, _elf):
            raise AssertionError("durable duplicate called the symbolizer")

        monkeypatch.setattr(
            "presence_api.operations.symbolize_coredump",
            symbolizer_must_not_run,
        )
        duplicate = operations.ingest_coredump(DEVICE, report)
        assert duplicate.duplicate is True
        assert duplicate.durable is True
        assert duplicate.symbolication_status == "succeeded"
    finally:
        database.dispose()
