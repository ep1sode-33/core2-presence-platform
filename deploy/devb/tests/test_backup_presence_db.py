from __future__ import annotations

import importlib.util
import os
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from datetime import UTC, datetime
from pathlib import Path
from unittest import mock

SCRIPT_PATH = Path(__file__).resolve().parents[1] / "backup_presence_db.py"
SPEC = importlib.util.spec_from_file_location("backup_presence_db", SCRIPT_PATH)
if SPEC is None or SPEC.loader is None:  # pragma: no cover - import guard
    raise RuntimeError(f"cannot load {SCRIPT_PATH}")
backup = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = backup
SPEC.loader.exec_module(backup)


class BackupPresenceDatabaseTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name)
        self.database_path = self.root / "presence.db"
        self.backup_directory = self.root / "backups"

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def settings(
        self,
        *,
        daily_retention: int = 14,
        weekly_retention: int = 8,
        weekly_weekday: str = "sun",
        min_free_bytes: int = 0,
    ) -> backup.BackupSettings:
        return backup.BackupSettings(
            database_path=self.database_path,
            backup_directory=self.backup_directory,
            daily_retention=daily_retention,
            weekly_retention=weekly_retention,
            weekly_weekday=weekly_weekday,
            min_free_bytes=min_free_bytes,
        )

    def create_live_wal_database(self) -> sqlite3.Connection:
        connection = sqlite3.connect(self.database_path)
        self.assertEqual(
            connection.execute("PRAGMA journal_mode=WAL").fetchone()[0], "wal"
        )
        connection.execute(
            "CREATE TABLE telemetry (sequence INTEGER PRIMARY KEY, state TEXT NOT NULL)"
        )
        connection.executemany(
            "INSERT INTO telemetry(sequence, state) VALUES (?, ?)",
            [(1, "PRESENT"), (2, "MAYBE"), (3, "VACANT")],
        )
        connection.commit()
        self.assertTrue(Path(f"{self.database_path}-wal").is_file())
        return connection

    def test_online_backup_includes_committed_wal_rows(self) -> None:
        source_connection = self.create_live_wal_database()
        try:
            result = backup.create_backup(
                self.settings(), now=datetime(2026, 8, 25, 12, tzinfo=UTC)
            )
        finally:
            source_connection.close()

        self.assertEqual(result.kind, "daily")
        self.assertEqual(result.path.name, "presence-daily-20260825T120000Z.sqlite3")
        self.assertEqual(result.path.stat().st_mode & 0o777, 0o600)
        with sqlite3.connect(result.path) as restored_connection:
            rows = restored_connection.execute(
                "SELECT sequence, state FROM telemetry ORDER BY sequence"
            ).fetchall()
            check = restored_connection.execute("PRAGMA integrity_check").fetchall()
            foreign_key_check = restored_connection.execute(
                "PRAGMA foreign_key_check"
            ).fetchall()
        self.assertEqual(
            rows,
            [(1, "PRESENT"), (2, "MAYBE"), (3, "VACANT")],
        )
        self.assertEqual(check, [("ok",)])
        self.assertEqual(foreign_key_check, [])

    def test_configured_weekday_creates_weekly_backup(self) -> None:
        source_connection = self.create_live_wal_database()
        try:
            result = backup.create_backup(
                self.settings(), now=datetime(2026, 8, 30, 12, tzinfo=UTC)
            )
        finally:
            source_connection.close()

        self.assertEqual(result.kind, "weekly")
        self.assertEqual(result.path.name, "presence-weekly-20260830T120000Z.sqlite3")

    def test_zero_weekly_retention_disables_weekly_classification(self) -> None:
        source_connection = self.create_live_wal_database()
        try:
            result = backup.create_backup(
                self.settings(weekly_retention=0),
                now=datetime(2026, 8, 30, 12, tzinfo=UTC),
            )
        finally:
            source_connection.close()

        self.assertEqual(result.kind, "daily")

    def test_retention_prunes_daily_and_weekly_generations_separately(self) -> None:
        source_connection = self.create_live_wal_database()
        self.backup_directory.mkdir()
        existing_names = (
            "presence-daily-20260820T120000Z.sqlite3",
            "presence-daily-20260821T120000Z.sqlite3",
            "presence-daily-20260822T120000Z.sqlite3",
            "presence-weekly-20260809T120000Z.sqlite3",
            "presence-weekly-20260816T120000Z.sqlite3",
            "presence-weekly-20260823T120000Z.sqlite3",
            "unmanaged.sqlite3",
        )
        for name in existing_names:
            (self.backup_directory / name).write_bytes(b"old")

        try:
            result = backup.create_backup(
                self.settings(daily_retention=2, weekly_retention=1),
                now=datetime(2026, 8, 25, 12, tzinfo=UTC),
            )
        finally:
            source_connection.close()

        remaining = {path.name for path in self.backup_directory.iterdir()}
        self.assertIn("presence-daily-20260825T120000Z.sqlite3", remaining)
        self.assertIn("presence-daily-20260822T120000Z.sqlite3", remaining)
        self.assertIn("presence-weekly-20260823T120000Z.sqlite3", remaining)
        self.assertIn("unmanaged.sqlite3", remaining)
        self.assertEqual(len(result.pruned), 4)

    def test_failed_integrity_check_never_publishes_partial_backup(self) -> None:
        source_connection = self.create_live_wal_database()
        try:
            with (
                mock.patch.object(
                    backup,
                    "_integrity_check",
                    side_effect=RuntimeError("injected check failure"),
                ),
                self.assertRaisesRegex(RuntimeError, "injected check failure"),
            ):
                backup.create_backup(
                    self.settings(),
                    now=datetime(2026, 8, 25, 12, tzinfo=UTC),
                )
        finally:
            source_connection.close()

        published = list(self.backup_directory.glob("presence-*.sqlite3"))
        temporary = list(self.backup_directory.glob(".presence-backup-*.tmp"))
        self.assertEqual(published, [])
        self.assertEqual(temporary, [])

    def test_foreign_key_violation_never_publishes_backup(self) -> None:
        connection = sqlite3.connect(self.database_path)
        connection.executescript(
            """
            PRAGMA journal_mode=WAL;
            PRAGMA foreign_keys=OFF;
            CREATE TABLE parent (id INTEGER PRIMARY KEY);
            CREATE TABLE child (
                id INTEGER PRIMARY KEY,
                parent_id INTEGER NOT NULL REFERENCES parent(id)
            );
            INSERT INTO child VALUES (1, 999);
            """
        )
        connection.commit()
        try:
            with self.assertRaisesRegex(RuntimeError, "foreign_key_check"):
                backup.create_backup(self.settings())
        finally:
            connection.close()

        self.assertEqual(list(self.backup_directory.glob("presence-*.sqlite3")), [])

    def test_capacity_reserve_is_checked_before_snapshot(self) -> None:
        source_connection = self.create_live_wal_database()
        try:
            with (
                mock.patch.object(
                    backup.shutil,
                    "disk_usage",
                    return_value=mock.Mock(free=1),
                ),
                self.assertRaisesRegex(RuntimeError, "insufficient backup space"),
            ):
                backup.create_backup(self.settings(min_free_bytes=10))
        finally:
            source_connection.close()

        self.assertEqual(list(self.backup_directory.glob("presence-*.sqlite3")), [])

    def test_next_run_removes_abandoned_temporary_file(self) -> None:
        source_connection = self.create_live_wal_database()
        self.backup_directory.mkdir()
        abandoned = self.backup_directory / ".presence-backup-abandoned.tmp"
        abandoned.write_bytes(b"partial")
        try:
            backup.create_backup(self.settings())
        finally:
            source_connection.close()

        self.assertFalse(abandoned.exists())

    def test_invalid_configuration_is_rejected_before_backup(self) -> None:
        self.database_path.write_bytes(b"unused")
        with self.assertRaisesRegex(ValueError, "daily retention"):
            backup.create_backup(self.settings(daily_retention=0))
        with self.assertRaisesRegex(ValueError, "weekly retention"):
            backup.create_backup(self.settings(weekly_retention=-1))
        with self.assertRaisesRegex(ValueError, "weekly weekday"):
            backup.create_backup(self.settings(weekly_weekday="someday"))
        with self.assertRaisesRegex(ValueError, "minimum free bytes"):
            backup.create_backup(self.settings(min_free_bytes=-1))

    def test_cli_returns_failure_for_missing_source_database(self) -> None:
        result = subprocess.run(
            [
                sys.executable,
                str(SCRIPT_PATH),
                "--database",
                str(self.database_path),
                "--backup-dir",
                str(self.backup_directory),
            ],
            check=False,
            capture_output=True,
            text=True,
        )

        self.assertEqual(result.returncode, 1)
        self.assertIn("presence backup failed", result.stderr)
        self.assertFalse(self.backup_directory.exists())

    def test_environment_settings_can_disable_weekly_backups(self) -> None:
        environment = {
            "PRESENCE_DB_PATH": str(self.database_path),
            "PRESENCE_BACKUP_DIR": str(self.backup_directory),
            "PRESENCE_BACKUP_DAILY_RETENTION": "5",
            "PRESENCE_BACKUP_WEEKLY_RETENTION": "0",
            "PRESENCE_BACKUP_WEEKDAY": "FRI",
            "PRESENCE_BACKUP_MIN_FREE_BYTES": "123456",
        }
        with mock.patch.dict(os.environ, environment, clear=True):
            settings = backup.settings_from_environment()

        self.assertEqual(settings.database_path, self.database_path)
        self.assertEqual(settings.backup_directory, self.backup_directory)
        self.assertEqual(settings.daily_retention, 5)
        self.assertEqual(settings.weekly_retention, 0)
        self.assertEqual(settings.weekly_weekday, "fri")
        self.assertEqual(settings.min_free_bytes, 123456)


if __name__ == "__main__":
    unittest.main()
