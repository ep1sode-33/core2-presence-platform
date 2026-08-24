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

    assert version == 2
    assert {"occurred_at_ms", "occurred_uptime_ms", "time_quality"} <= columns
    assert migrated == (123456, None, "server_receive")
