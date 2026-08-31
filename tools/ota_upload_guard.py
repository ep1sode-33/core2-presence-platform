"""PlatformIO preflight for the physically gated development OTA target."""

# PlatformIO/SCons injects ``Import`` and ``env`` before evaluating this file.
# ruff: noqa: F821

from __future__ import annotations

import ipaddress
import os
import re

Import("env")  # type: ignore[name-defined]  # Provided by PlatformIO/SCons.


def _is_upload_target() -> bool:
    # BUILD_TARGETS is available through the construction environment after
    # PlatformIO has expanded aliases; COMMAND_LINE_TARGETS covers direct use.
    targets = {str(value) for value in env.get("BUILD_TARGETS", [])}  # type: ignore[name-defined]
    try:
        from SCons.Script import COMMAND_LINE_TARGETS

        targets.update(str(value) for value in COMMAND_LINE_TARGETS)
    except ImportError:
        pass
    return bool(targets & {"upload", "program"})


if _is_upload_target():
    host = os.environ.get("M5GO_OTA_HOST", "")
    secret = os.environ.get("M5GO_OTA_SECRET", "")
    try:
        address = ipaddress.ip_address(host)
    except ValueError as error:
        raise RuntimeError(
            "M5GO_OTA_HOST must be a literal 192.168.0.0/24 IPv4 address"
        ) from error
    if address not in ipaddress.ip_network("192.168.0.0/24"):
        raise RuntimeError("development OTA is restricted to 192.168.0.0/24")
    if not re.fullmatch(r"[A-Za-z0-9_-]{43}", secret):
        raise RuntimeError(
            "M5GO_OTA_SECRET must be 32 random bytes encoded as 43-char "
            "unpadded base64url"
        )
    # Replace only after validation, so espota cannot receive an unchecked or
    # stale upload_port from project configuration.
    env.Replace(UPLOAD_PORT=host)  # type: ignore[name-defined]
else:
    # Espressif's builder diagnoses an unset espota host even for a build-only
    # command. This loopback sentinel is never used by an upload target; the
    # branch above either injects a validated LAN host or aborts preflight.
    env.Replace(UPLOAD_PORT="127.0.0.1")  # type: ignore[name-defined]
