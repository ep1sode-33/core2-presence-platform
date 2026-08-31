from __future__ import annotations

from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path

from sqlalchemy import Engine, create_engine, event, text
from sqlalchemy.orm import Session, sessionmaker

from .models import Base


class Database:
    SCHEMA_VERSION = 5

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
        # Coredumps are acknowledged with durable=true only after commit. FULL
        # makes a WAL commit survive power loss before the device erases its
        # only local copy; the same conservative guarantee also benefits the
        # rest of this low-volume home service.
        cursor.execute("PRAGMA synchronous=FULL")
        cursor.execute("PRAGMA busy_timeout=5000")
        cursor.close()

    def initialize(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with self.engine.begin() as connection:
            schema_version = connection.execute(
                text("PRAGMA user_version")
            ).scalar_one()
            if schema_version not in (0, 1, 2, 3, 4, self.SCHEMA_VERSION):
                raise RuntimeError(
                    f"unsupported database schema version: {schema_version}"
                )
        Base.metadata.create_all(self.engine)
        if schema_version < 2:
            self._migrate_to_v2()
        if schema_version < 3:
            self._migrate_to_v3()
        if schema_version < 4:
            self._migrate_to_v4()
        if schema_version < 5:
            self._migrate_to_v5()
        self._ensure_v4_columns()
        self._ensure_v5_columns()
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

    def _migrate_to_v4(self) -> None:
        """Record creation of the v0.7 operations tables.

        ``Base.metadata.create_all`` creates the new, additive tables before
        this migration runs. Keeping v4 free of destructive table rewrites
        preserves every v1-v3 telemetry row and makes the migration safe to
        retry after an interrupted service start.
        """
        with self.engine.begin() as connection:
            telemetry_columns = {
                row[1]
                for row in connection.execute(
                    text("PRAGMA table_info(telemetry_records)")
                )
            }
            if "build_id" not in telemetry_columns:
                connection.execute(
                    text(
                        "ALTER TABLE telemetry_records ADD COLUMN build_id VARCHAR(128)"
                    )
                )

            transition_columns = {
                row[1]
                for row in connection.execute(
                    text("PRAGMA table_info(state_transitions)")
                )
            }
            transition_additions = {
                "pir": "BOOLEAN",
                "pir_age_ms": "INTEGER",
                "sound_active": "BOOLEAN",
                "sound_age_ms": "INTEGER",
                "mic_envelope": "FLOAT",
                "noise_floor": "FLOAT",
                "sound_threshold": "FLOAT",
                "brightness_before": "INTEGER",
                "brightness_after": "INTEGER",
            }
            for column, sql_type in transition_additions.items():
                if column not in transition_columns:
                    connection.execute(
                        text(
                            f"ALTER TABLE state_transitions "
                            f"ADD COLUMN {column} {sql_type}"
                        )
                    )

            required_tables = (
                "device_health_reports",
                "firmware_releases",
                "device_release_targets",
                "release_status_reports",
                "device_commands",
                "command_acks",
                "operational_log_batches",
                "operational_logs",
                "core_dumps",
            )
            present_tables = {
                row[0]
                for row in connection.execute(
                    text("SELECT name FROM sqlite_master WHERE type = 'table'")
                )
            }
            missing = sorted(set(required_tables) - present_tables)
            if missing:
                raise RuntimeError(
                    "schema v4 tables were not created: " + ", ".join(missing)
                )
            connection.execute(text("PRAGMA user_version=4"))

    def _migrate_to_v5(self) -> None:
        """Persist OTA confirmation and per-report completion receipts.

        Both additions are retry-safe columns. Existing confirmed counters are
        conservatively reconstructed only from running reports whose release,
        firmware version, and build ID all agree.
        """
        with self.engine.begin() as connection:
            device_columns = {
                row[1] for row in connection.execute(text("PRAGMA table_info(devices)"))
            }
            if "confirmed_release_counter" not in device_columns:
                connection.execute(
                    text(
                        "ALTER TABLE devices ADD COLUMN "
                        "confirmed_release_counter INTEGER NOT NULL DEFAULT 0"
                    )
                )

            status_columns = {
                row[1]
                for row in connection.execute(
                    text("PRAGMA table_info(release_status_reports)")
                )
            }
            if status_columns and "desired_release_completed" not in status_columns:
                connection.execute(
                    text(
                        "ALTER TABLE release_status_reports ADD COLUMN "
                        "desired_release_completed BOOLEAN NOT NULL DEFAULT 0"
                    )
                )

            # A v4 database can already contain successfully confirmed OTA
            # installs. Preserve their monotonic floor during migration, while
            # deliberately ignoring incomplete or identity-mismatched reports.
            connection.execute(
                text(
                    """
                    UPDATE devices
                    SET confirmed_release_counter = MAX(
                        confirmed_release_counter,
                        COALESCE((
                            SELECT MAX(firmware_releases.release_counter)
                            FROM release_status_reports
                            JOIN firmware_releases
                              ON firmware_releases.release_id =
                                 release_status_reports.running_release_id
                            WHERE release_status_reports.device_id =
                                  devices.device_id
                              AND release_status_reports.phase = 'running'
                              AND release_status_reports.firmware_version =
                                  firmware_releases.firmware_version
                              AND release_status_reports.build_id =
                                  firmware_releases.build_id
                        ), 0)
                    )
                    """
                )
            )
            connection.execute(text("PRAGMA user_version=5"))

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

    def _ensure_v4_columns(self) -> None:
        """Repair additive v4 columns after an interrupted early deployment."""
        with self.engine.begin() as connection:
            release_columns = {
                row[1]
                for row in connection.execute(
                    text("PRAGMA table_info(firmware_releases)")
                )
            }
            if release_columns and "elf_size" not in release_columns:
                connection.execute(
                    text(
                        "ALTER TABLE firmware_releases "
                        "ADD COLUMN elf_size INTEGER NOT NULL DEFAULT 0"
                    )
                )
                connection.execute(
                    text("UPDATE firmware_releases SET elf_size = LENGTH(elf_blob)")
                )

            batch_columns = {
                row[1]
                for row in connection.execute(
                    text("PRAGMA table_info(operational_log_batches)")
                )
            }
            for column in ("boot_id", "build_id"):
                if batch_columns and column not in batch_columns:
                    connection.execute(
                        text(
                            "ALTER TABLE operational_log_batches "
                            f"ADD COLUMN {column} VARCHAR(128) "
                            "NOT NULL DEFAULT 'unknown'"
                        )
                    )

            log_columns = {
                row[1]
                for row in connection.execute(
                    text("PRAGMA table_info(operational_logs)")
                )
            }
            if log_columns and "build_id" not in log_columns:
                connection.execute(
                    text(
                        "ALTER TABLE operational_logs "
                        "ADD COLUMN build_id VARCHAR(128) "
                        "NOT NULL DEFAULT 'unknown'"
                    )
                )

    def _ensure_v5_columns(self) -> None:
        """Repair additive v5 columns after an interrupted service start."""
        with self.engine.begin() as connection:
            device_columns = {
                row[1] for row in connection.execute(text("PRAGMA table_info(devices)"))
            }
            if device_columns and "confirmed_release_counter" not in device_columns:
                connection.execute(
                    text(
                        "ALTER TABLE devices ADD COLUMN "
                        "confirmed_release_counter INTEGER NOT NULL DEFAULT 0"
                    )
                )

            status_columns = {
                row[1]
                for row in connection.execute(
                    text("PRAGMA table_info(release_status_reports)")
                )
            }
            if status_columns and "desired_release_completed" not in status_columns:
                connection.execute(
                    text(
                        "ALTER TABLE release_status_reports ADD COLUMN "
                        "desired_release_completed BOOLEAN NOT NULL DEFAULT 0"
                    )
                )

    @contextmanager
    def session(self) -> Iterator[Session]:
        with self._sessions() as session:
            yield session

    def dispose(self) -> None:
        self.engine.dispose()
