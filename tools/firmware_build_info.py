"""Inject one validated firmware/build identity into every PlatformIO build."""

# PlatformIO/SCons injects ``Import`` and ``env`` before evaluating this file.
# ruff: noqa: F821

from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

Import("env")  # type: ignore[name-defined]

_VERSION = re.compile(r"[A-Za-z0-9._+-]{1,23}")
_BUILD_ID = re.compile(r"[A-Za-z0-9._+-]{1,47}")
_HARDWARE_MODEL = "m5go-classic-esp32-16m"


def _git_build_id(project_dir: Path) -> str:
    commit = subprocess.run(
        ("git", "rev-parse", "--short=12", "HEAD"),
        cwd=project_dir,
        check=False,
        capture_output=True,
        text=True,
    )
    if commit.returncode != 0 or not re.fullmatch(
        r"[0-9a-f]{7,12}", commit.stdout.strip()
    ):
        return "local"
    dirty = subprocess.run(
        ("git", "status", "--porcelain", "--untracked-files=no"),
        cwd=project_dir,
        check=False,
        capture_output=True,
        text=True,
    )
    suffix = "+dirty" if dirty.returncode == 0 and dirty.stdout else ""
    return f"git.{commit.stdout.strip()}{suffix}"


project_dir = Path(env.subst("$PROJECT_DIR"))  # type: ignore[name-defined]
firmware_version = os.environ.get("M5GO_FIRMWARE_VERSION", "0.7.0-dev")
build_id = os.environ.get("M5GO_BUILD_ID", "") or _git_build_id(project_dir)
if _VERSION.fullmatch(firmware_version) is None:
    raise RuntimeError("M5GO_FIRMWARE_VERSION is not canonical or exceeds 23 bytes")
if _BUILD_ID.fullmatch(build_id) is None:
    raise RuntimeError("M5GO_BUILD_ID is not canonical or exceeds 47 bytes")

env.Append(  # type: ignore[name-defined]
    CPPDEFINES=[
        ("M5GO_HARDWARE_MODEL", f'\\"{_HARDWARE_MODEL}\\"'),
        ("M5GO_FIRMWARE_VERSION", f'\\"{firmware_version}\\"'),
        ("M5GO_BUILD_ID", f'\\"{build_id}\\"'),
    ]
)
