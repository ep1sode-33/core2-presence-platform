#!/usr/bin/env python3
"""Create and rotate consistent backups of the live presence SQLite database."""

from __future__ import annotations

import argparse
import fcntl
import os
import re
import shutil
import sqlite3
import sys
import tempfile
from collections.abc import Iterator
from contextlib import closing, contextmanager
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path
from typing import BinaryIO
from urllib.parse import quote

DEFAULT_DATABASE_PATH = Path("/var/lib/m5-presence/presence.db")
DEFAULT_BACKUP_DIRECTORY = Path("/var/lib/m5-presence-backup")
DEFAULT_DAILY_RETENTION = 14
DEFAULT_WEEKLY_RETENTION = 8
DEFAULT_WEEKLY_WEEKDAY = "sun"
DEFAULT_MIN_FREE_BYTES = 1_073_741_824
WEEKDAYS = ("mon", "tue", "wed", "thu", "fri", "sat", "sun")
BACKUP_NAME = re.compile(
    r"^presence-(?P<kind>daily|weekly)-"
    r"(?P<timestamp>\d{8}T\d{6}Z)\.sqlite3$"
)


@dataclass(frozen=True)
class BackupSettings:
    database_path: Path
    backup_directory: Path
    daily_retention: int
    weekly_retention: int
    weekly_weekday: str
    min_free_bytes: int


@dataclass(frozen=True)
class BackupResult:
    path: Path
    kind: str
    pruned: tuple[Path, ...]


def _environment_integer(name: str, default: int) -> int:
    value = os.getenv(name)
    if value is None:
        return default
    try:
        return int(value)
    except ValueError as error:
        raise ValueError(f"{name} must be an integer") from error


def settings_from_environment() -> BackupSettings:
    return BackupSettings(
        database_path=Path(
            os.getenv("PRESENCE_DB_PATH", str(DEFAULT_DATABASE_PATH))
        ).expanduser(),
        backup_directory=Path(
            os.getenv("PRESENCE_BACKUP_DIR", str(DEFAULT_BACKUP_DIRECTORY))
        ).expanduser(),
        daily_retention=_environment_integer(
            "PRESENCE_BACKUP_DAILY_RETENTION", DEFAULT_DAILY_RETENTION
        ),
        weekly_retention=_environment_integer(
            "PRESENCE_BACKUP_WEEKLY_RETENTION", DEFAULT_WEEKLY_RETENTION
        ),
        weekly_weekday=os.getenv("PRESENCE_BACKUP_WEEKDAY", DEFAULT_WEEKLY_WEEKDAY)
        .strip()
        .lower(),
        min_free_bytes=_environment_integer(
            "PRESENCE_BACKUP_MIN_FREE_BYTES", DEFAULT_MIN_FREE_BYTES
        ),
    )


def validate_settings(settings: BackupSettings) -> None:
    if settings.daily_retention < 1:
        raise ValueError("daily retention must be at least 1")
    if settings.weekly_retention < 0:
        raise ValueError("weekly retention must not be negative")
    if settings.weekly_weekday not in WEEKDAYS:
        raise ValueError("weekly weekday must be one of " + ", ".join(WEEKDAYS))
    if settings.min_free_bytes < 0:
        raise ValueError("minimum free bytes must not be negative")

    database_path = settings.database_path.resolve(strict=False)
    backup_directory = settings.backup_directory.resolve(strict=False)
    if database_path.parent == backup_directory and BACKUP_NAME.fullmatch(
        database_path.name
    ):
        raise ValueError("database path must not be a managed backup file")


def _read_only_database_uri(path: Path) -> str:
    absolute_path = path.resolve(strict=True)
    return f"file:{quote(str(absolute_path), safe='/')}?mode=ro"


def _fsync_file(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _fsync_directory(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _integrity_check(connection: sqlite3.Connection) -> None:
    rows = connection.execute("PRAGMA integrity_check").fetchall()
    messages = [str(row[0]) for row in rows]
    if messages != ["ok"]:
        detail = "; ".join(messages) if messages else "no result"
        raise RuntimeError(f"backup failed SQLite integrity_check: {detail}")
    foreign_key_errors = connection.execute("PRAGMA foreign_key_check").fetchall()
    if foreign_key_errors:
        raise RuntimeError(
            "backup failed SQLite foreign_key_check: "
            f"{len(foreign_key_errors)} violation(s)"
        )


@contextmanager
def _exclusive_lock(backup_directory: Path) -> Iterator[None]:
    lock_path = backup_directory / ".backup.lock"
    lock_file: BinaryIO | None = None
    try:
        lock_file = lock_path.open("a+b")
        os.chmod(lock_path, 0o600)
        try:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise RuntimeError("another presence backup is already running") from error
        yield
    finally:
        if lock_file is not None:
            lock_file.close()


def _snapshot_to_temporary_file(source: Path, destination: Path) -> None:
    source_uri = _read_only_database_uri(source)
    with (
        closing(sqlite3.connect(source_uri, uri=True, timeout=30)) as source_connection,
        closing(sqlite3.connect(destination)) as destination_connection,
    ):
        source_connection.backup(destination_connection)
        _integrity_check(destination_connection)

    os.chmod(destination, 0o600)
    _fsync_file(destination)


def _backup_kind(now: datetime, settings: BackupSettings) -> str:
    if settings.weekly_retention == 0:
        return "daily"
    return "weekly" if WEEKDAYS[now.weekday()] == settings.weekly_weekday else "daily"


def _prune_backups(
    backup_directory: Path,
    *,
    daily_retention: int,
    weekly_retention: int,
) -> tuple[Path, ...]:
    retained_by_kind = {
        "daily": daily_retention,
        "weekly": weekly_retention,
    }
    backups_by_kind: dict[str, list[Path]] = {"daily": [], "weekly": []}
    for entry in backup_directory.iterdir():
        match = BACKUP_NAME.fullmatch(entry.name)
        if match is not None:
            backups_by_kind[match.group("kind")].append(entry)

    pruned: list[Path] = []
    for kind, backups in backups_by_kind.items():
        backups.sort(key=lambda path: path.name, reverse=True)
        for expired_path in backups[retained_by_kind[kind] :]:
            expired_path.unlink()
            pruned.append(expired_path)

    if pruned:
        _fsync_directory(backup_directory)
    return tuple(sorted(pruned))


def _remove_abandoned_temporary_files(backup_directory: Path) -> tuple[Path, ...]:
    removed: list[Path] = []
    for temporary_path in backup_directory.glob(".presence-backup-*.tmp"):
        if temporary_path.is_file():
            temporary_path.unlink()
            removed.append(temporary_path)
    if removed:
        _fsync_directory(backup_directory)
    return tuple(sorted(removed))


def _require_backup_capacity(settings: BackupSettings) -> None:
    source_bytes = settings.database_path.stat().st_size
    free_bytes = shutil.disk_usage(settings.backup_directory).free
    required_bytes = source_bytes + settings.min_free_bytes
    if free_bytes < required_bytes:
        raise RuntimeError(
            "insufficient backup space: "
            f"free={free_bytes} required={required_bytes} "
            f"(database={source_bytes} reserve={settings.min_free_bytes})"
        )


def create_backup(
    settings: BackupSettings,
    *,
    now: datetime | None = None,
) -> BackupResult:
    validate_settings(settings)
    if not settings.database_path.is_file():
        raise FileNotFoundError(
            f"presence database does not exist: {settings.database_path}"
        )

    backup_directory = settings.backup_directory
    backup_directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    os.chmod(backup_directory, 0o700)

    current_time = now or datetime.now(UTC)
    if current_time.tzinfo is None:
        raise ValueError("backup timestamp must be timezone-aware")
    utc_time = current_time.astimezone(UTC)
    kind = _backup_kind(utc_time, settings)
    timestamp = utc_time.strftime("%Y%m%dT%H%M%SZ")
    final_path = backup_directory / f"presence-{kind}-{timestamp}.sqlite3"

    with _exclusive_lock(backup_directory):
        _remove_abandoned_temporary_files(backup_directory)
        _require_backup_capacity(settings)
        temporary_descriptor, temporary_name = tempfile.mkstemp(
            dir=backup_directory,
            prefix=".presence-backup-",
            suffix=".tmp",
        )
        os.close(temporary_descriptor)
        temporary_path = Path(temporary_name)
        try:
            _snapshot_to_temporary_file(settings.database_path, temporary_path)
            os.replace(temporary_path, final_path)
            _fsync_directory(backup_directory)
        finally:
            temporary_path.unlink(missing_ok=True)

        pruned = _prune_backups(
            backup_directory,
            daily_retention=settings.daily_retention,
            weekly_retention=settings.weekly_retention,
        )

    return BackupResult(path=final_path, kind=kind, pruned=pruned)


def _argument_parser(defaults: BackupSettings) -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", type=Path, default=defaults.database_path)
    parser.add_argument("--backup-dir", type=Path, default=defaults.backup_directory)
    parser.add_argument("--daily-retention", type=int, default=defaults.daily_retention)
    parser.add_argument(
        "--weekly-retention", type=int, default=defaults.weekly_retention
    )
    parser.add_argument(
        "--weekly-weekday", default=defaults.weekly_weekday, choices=WEEKDAYS
    )
    parser.add_argument("--min-free-bytes", type=int, default=defaults.min_free_bytes)
    return parser


def main() -> int:
    try:
        defaults = settings_from_environment()
        arguments = _argument_parser(defaults).parse_args()
        settings = BackupSettings(
            database_path=arguments.database,
            backup_directory=arguments.backup_dir,
            daily_retention=arguments.daily_retention,
            weekly_retention=arguments.weekly_retention,
            weekly_weekday=arguments.weekly_weekday,
            min_free_bytes=arguments.min_free_bytes,
        )
        result = create_backup(settings)
    except (OSError, RuntimeError, sqlite3.Error, ValueError) as error:
        print(f"presence backup failed: {error}", file=sys.stderr)
        return 1

    print(
        f"presence backup complete: {result.path} "
        f"kind={result.kind} integrity_check=ok foreign_key_check=ok "
        f"pruned={len(result.pruned)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
