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

    assert version == 3
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

    assert version == 3
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
