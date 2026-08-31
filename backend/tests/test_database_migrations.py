from __future__ import annotations

import sqlite3
from pathlib import Path

from sqlalchemy import text

from presence_api.database import Database


def create_v1_database(path: Path) -> None:
    with sqlite3.connect(path) as connection:
        connection.executescript(
            """
            CREATE TABLE feedback (
                feedback_id VARCHAR(64) PRIMARY KEY,
                device_id VARCHAR(64) NOT NULL,
                boot_id VARCHAR(64),
                seq INTEGER,
                created_at_ms INTEGER NOT NULL,
                actual_presence VARCHAR(16) NOT NULL,
                observed_state VARCHAR(16),
                source VARCHAR(16) NOT NULL,
                note TEXT,
                payload_hash VARCHAR(64) NOT NULL
            );
            INSERT INTO feedback VALUES (
                'feedback-old-0001', 'legacy-device', NULL, NULL, 123456,
                'present', NULL, 'web', NULL, 'hash'
            );
            PRAGMA user_version=1;
            """
        )


def create_v2_database(path: Path) -> None:
    with sqlite3.connect(path) as connection:
        connection.executescript(
            """
            CREATE TABLE telemetry_records (
                device_id VARCHAR(64) NOT NULL,
                boot_id VARCHAR(64) NOT NULL,
                seq INTEGER NOT NULL,
                kind VARCHAR(16) NOT NULL,
                uptime_ms INTEGER NOT NULL,
                observed_at_ms INTEGER,
                received_at_ms INTEGER NOT NULL,
                time_quality VARCHAR(32) NOT NULL,
                payload_hash VARCHAR(64) NOT NULL,
                PRIMARY KEY (device_id, boot_id, seq)
            );
            INSERT INTO telemetry_records VALUES (
                'core2-2cbcbb81eb60', 'boot000000000001', 7, 'sample',
                1234, NULL, 5678, 'receive_only', 'legacy-hash'
            );
            PRAGMA user_version=2;
            """
        )


def test_v1_database_migrates_feedback_columns(tmp_path: Path) -> None:
    path = tmp_path / "legacy.db"
    create_v1_database(path)
    database = Database(path)

    try:
        database.initialize()
        database.initialize()
        with database.engine.connect() as connection:
            version = connection.execute(text("PRAGMA user_version")).scalar_one()
            columns = {
                row[1]
                for row in connection.execute(text("PRAGMA table_info(feedback)"))
            }
            migrated = connection.execute(
                text(
                    "SELECT occurred_at_ms, occurred_uptime_ms, time_quality "
                    "FROM feedback WHERE feedback_id = 'feedback-old-0001'"
                )
            ).one()
    finally:
        database.dispose()

    assert version == Database.SCHEMA_VERSION
    assert {"occurred_at_ms", "occurred_uptime_ms", "time_quality"} <= columns
    assert migrated == (123456, None, "server_receive")


def test_v2_database_adds_unknown_record_revision_idempotently(
    tmp_path: Path,
) -> None:
    path = tmp_path / "legacy-v2.db"
    create_v2_database(path)
    database = Database(path)

    try:
        database.initialize()
        database.initialize()
        with database.engine.connect() as connection:
            version = connection.execute(text("PRAGMA user_version")).scalar_one()
            columns = {
                row[1]
                for row in connection.execute(
                    text("PRAGMA table_info(telemetry_records)")
                )
            }
            migrated_revision = connection.execute(
                text(
                    "SELECT applied_config_revision FROM telemetry_records "
                    "WHERE device_id = 'core2-2cbcbb81eb60' "
                    "AND boot_id = 'boot000000000001' AND seq = 7"
                )
            ).scalar_one()
            event_index = connection.execute(
                text(
                    "SELECT sql FROM sqlite_master "
                    "WHERE type = 'index' "
                    "AND name = 'idx_records_device_event'"
                )
            ).scalar_one()
    finally:
        database.dispose()

    assert version == Database.SCHEMA_VERSION
    assert "applied_config_revision" in columns
    assert migrated_revision is None
    assert "COALESCE(observed_at_ms, received_at_ms)" in event_index


def test_schema_v3_initialization_repairs_missing_event_index(tmp_path: Path) -> None:
    path = tmp_path / "missing-index.db"
    database = Database(path)
    try:
        database.initialize()
        with database.engine.begin() as connection:
            connection.execute(text("DROP INDEX idx_records_device_event"))
        database.initialize()
        with database.engine.connect() as connection:
            repaired_index = connection.execute(
                text(
                    "SELECT sql FROM sqlite_master "
                    "WHERE type = 'index' "
                    "AND name = 'idx_records_device_event'"
                )
            ).scalar_one()
    finally:
        database.dispose()

    assert "boot_id, seq" in repaired_index


def test_v3_database_adds_v07_operations_and_transition_evidence(
    tmp_path: Path,
) -> None:
    path = tmp_path / "schema-v3.db"
    database = Database(path)
    try:
        database.initialize()
        with database.engine.begin() as connection:
            connection.execute(text("PRAGMA user_version=3"))
            for table in (
                "device_health_reports",
                "firmware_releases",
                "device_release_targets",
                "release_status_reports",
                "device_commands",
                "command_acks",
                "operational_log_batches",
                "operational_logs",
                "core_dumps",
            ):
                connection.execute(text(f"DROP TABLE {table}"))
        database.dispose()

        database = Database(path)
        database.initialize()
        database.initialize()
        with database.engine.connect() as connection:
            version = connection.execute(text("PRAGMA user_version")).scalar_one()
            tables = {
                row[0]
                for row in connection.execute(
                    text("SELECT name FROM sqlite_master WHERE type = 'table'")
                )
            }
            record_columns = {
                row[1]
                for row in connection.execute(
                    text("PRAGMA table_info(telemetry_records)")
                )
            }
            transition_columns = {
                row[1]
                for row in connection.execute(
                    text("PRAGMA table_info(state_transitions)")
                )
            }
    finally:
        database.dispose()

    assert version == Database.SCHEMA_VERSION
    assert {
        "device_health_reports",
        "firmware_releases",
        "device_commands",
        "operational_logs",
        "core_dumps",
    } <= tables
    assert "build_id" in record_columns
    assert {
        "pir",
        "pir_age_ms",
        "sound_active",
        "sound_age_ms",
        "mic_envelope",
        "noise_floor",
        "sound_threshold",
        "brightness_before",
        "brightness_after",
    } <= transition_columns


def test_v4_database_backfills_only_identity_verified_running_counter(
    tmp_path: Path,
) -> None:
    path = tmp_path / "schema-v4.db"
    with sqlite3.connect(path) as connection:
        connection.executescript(
            """
            CREATE TABLE devices (
                device_id VARCHAR(64) PRIMARY KEY,
                name VARCHAR(128),
                created_at_ms INTEGER NOT NULL,
                last_seen_at_ms INTEGER NOT NULL,
                firmware_version VARCHAR(64),
                desired_config_revision INTEGER NOT NULL,
                applied_config_revision INTEGER NOT NULL
            );
            CREATE TABLE firmware_releases (
                release_id VARCHAR(48) PRIMARY KEY,
                firmware_version VARCHAR(64) NOT NULL,
                release_counter INTEGER NOT NULL,
                build_id VARCHAR(128) NOT NULL,
                elf_size INTEGER NOT NULL,
                elf_blob BLOB NOT NULL
            );
            CREATE TABLE release_status_reports (
                status_id VARCHAR(64) PRIMARY KEY,
                device_id VARCHAR(64) NOT NULL,
                running_release_id VARCHAR(48),
                phase VARCHAR(32) NOT NULL,
                firmware_version VARCHAR(64) NOT NULL,
                build_id VARCHAR(128) NOT NULL
            );
            INSERT INTO devices VALUES (
                'core2-2cbcbb81eb60', NULL, 1, 2, '0.7.0', 0, 0
            );
            INSERT INTO firmware_releases VALUES (
                'rel-00000000000000000000000000000007',
                '0.7.0', 7, 'build-seven', 1, X'00'
            );
            INSERT INTO firmware_releases VALUES (
                'rel-00000000000000000000000000000099',
                '9.9.0', 99, 'build-ninety-nine', 1, X'00'
            );
            INSERT INTO release_status_reports VALUES (
                'status-valid-running', 'core2-2cbcbb81eb60',
                'rel-00000000000000000000000000000007',
                'running', '0.7.0', 'build-seven'
            );
            INSERT INTO release_status_reports VALUES (
                'status-invalid-running', 'core2-2cbcbb81eb60',
                'rel-00000000000000000000000000000099',
                'running', '0.7.0', 'wrong-build'
            );
            PRAGMA user_version=4;
            """
        )

    database = Database(path)
    try:
        database.initialize()
        database.initialize()
        with database.engine.connect() as connection:
            version = connection.execute(text("PRAGMA user_version")).scalar_one()
            confirmed = connection.execute(
                text(
                    "SELECT confirmed_release_counter FROM devices "
                    "WHERE device_id = 'core2-2cbcbb81eb60'"
                )
            ).scalar_one()
            status_columns = {
                row[1]
                for row in connection.execute(
                    text("PRAGMA table_info(release_status_reports)")
                )
            }
            completion_values = (
                connection.execute(
                    text(
                        "SELECT desired_release_completed "
                        "FROM release_status_reports ORDER BY status_id"
                    )
                )
                .scalars()
                .all()
            )
    finally:
        database.dispose()

    assert version == Database.SCHEMA_VERSION
    assert confirmed == 7
    assert "desired_release_completed" in status_columns
    assert completion_values == [0, 0]


def test_database_uses_full_wal_synchronization_for_durable_receipts(
    tmp_path: Path,
) -> None:
    database = Database(tmp_path / "durable.db")
    database.initialize()
    with database.engine.connect() as connection:
        assert connection.exec_driver_sql("PRAGMA journal_mode").scalar_one() == "wal"
        assert connection.exec_driver_sql("PRAGMA synchronous").scalar_one() == 2
