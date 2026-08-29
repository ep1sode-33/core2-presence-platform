from __future__ import annotations

from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path

from sqlalchemy import Engine, create_engine, event, text
from sqlalchemy.orm import Session, sessionmaker

from .models import Base


class Database:
    SCHEMA_VERSION = 3

    def __init__(self, path: Path) -> None:
        self.path = path
        self.engine: Engine = create_engine(
            f"sqlite+pysqlite:///{path}",
            connect_args={"check_same_thread": False},
        )
        event.listen(self.engine, "connect", self._configure_connection)
        self._sessions = sessionmaker(
            bind=self.engine,
            class_=Session,
            expire_on_commit=False,
        )

    @staticmethod
    def _configure_connection(dbapi_connection, _connection_record) -> None:
        cursor = dbapi_connection.cursor()
        cursor.execute("PRAGMA foreign_keys=ON")
        cursor.execute("PRAGMA journal_mode=WAL")
        cursor.execute("PRAGMA synchronous=NORMAL")
        cursor.execute("PRAGMA busy_timeout=5000")
        cursor.close()

    def initialize(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with self.engine.begin() as connection:
            schema_version = connection.execute(
                text("PRAGMA user_version")
            ).scalar_one()
            if schema_version not in (0, 1, 2, self.SCHEMA_VERSION):
                raise RuntimeError(
                    f"unsupported database schema version: {schema_version}"
                )
        Base.metadata.create_all(self.engine)
        if schema_version < 2:
            self._migrate_to_v2()
        if schema_version < 3:
            self._migrate_to_v3()
        self._ensure_query_indexes()

    def _migrate_to_v2(self) -> None:
        with self.engine.begin() as connection:
            feedback_columns = {
                row[1]
                for row in connection.execute(text("PRAGMA table_info(feedback)"))
            }
            if "occurred_at_ms" not in feedback_columns:
                connection.execute(
                    text("ALTER TABLE feedback ADD COLUMN occurred_at_ms INTEGER")
                )
            if "occurred_uptime_ms" not in feedback_columns:
                connection.execute(
                    text("ALTER TABLE feedback ADD COLUMN occurred_uptime_ms INTEGER")
                )
            if "time_quality" not in feedback_columns:
                connection.execute(
                    text(
                        "ALTER TABLE feedback ADD COLUMN time_quality "
                        "VARCHAR(32) NOT NULL DEFAULT 'server_receive'"
                    )
                )

            connection.execute(
                text(
                    """
                    UPDATE feedback
                    SET occurred_at_ms = CASE
                            WHEN boot_id IS NULL THEN created_at_ms
                            ELSE (
                                SELECT telemetry_records.observed_at_ms
                                FROM telemetry_records
                                WHERE telemetry_records.device_id = feedback.device_id
                                  AND telemetry_records.boot_id = feedback.boot_id
                                  AND telemetry_records.seq = feedback.seq
                            )
                        END,
                        occurred_uptime_ms = CASE
                            WHEN boot_id IS NULL THEN NULL
                            ELSE (
                                SELECT telemetry_records.uptime_ms
                                FROM telemetry_records
                                WHERE telemetry_records.device_id = feedback.device_id
                                  AND telemetry_records.boot_id = feedback.boot_id
                                  AND telemetry_records.seq = feedback.seq
                            )
                        END,
                        time_quality = CASE
                            WHEN boot_id IS NULL THEN 'server_receive'
                            ELSE COALESCE((
                                SELECT telemetry_records.time_quality
                                FROM telemetry_records
                                WHERE telemetry_records.device_id = feedback.device_id
                                  AND telemetry_records.boot_id = feedback.boot_id
                                  AND telemetry_records.seq = feedback.seq
                            ), 'receive_only')
                        END
                    """
                )
            )
            connection.execute(text("PRAGMA user_version=2"))

    def _migrate_to_v3(self) -> None:
        with self.engine.begin() as connection:
            telemetry_columns = {
                row[1]
                for row in connection.execute(
                    text("PRAGMA table_info(telemetry_records)")
                )
            }
            if "applied_config_revision" not in telemetry_columns:
                # The batch revision was not persisted before v3, and the
                # device-level maximum cannot reconstruct record history.
                # NULL deliberately represents an unknown legacy revision.
                connection.execute(
                    text(
                        "ALTER TABLE telemetry_records "
                        "ADD COLUMN applied_config_revision INTEGER"
                    )
                )
            connection.execute(text("PRAGMA user_version=3"))

    def _ensure_query_indexes(self) -> None:
        with self.engine.begin() as connection:
            connection.execute(
                text(
                    "CREATE INDEX IF NOT EXISTS idx_records_device_event "
                    "ON telemetry_records ("
                    "device_id, "
                    "COALESCE(observed_at_ms, received_at_ms), "
                    "boot_id, seq"
                    ")"
                )
            )

    @contextmanager
    def session(self) -> Iterator[Session]:
        with self._sessions() as session:
            yield session

    def dispose(self) -> None:
        self.engine.dispose()
