#!/usr/bin/env python3
"""Provision the M5GO over the USB serial line protocol.

The protocol helpers in this module intentionally have no pyserial dependency so
they can be unit tested on a development machine without attached hardware.
"""

from __future__ import annotations

import argparse
import base64
import getpass
import re
import sys
import time
from collections.abc import Iterable, Sequence
from dataclasses import dataclass
from typing import TextIO
from urllib.parse import urlsplit

DEFAULT_PORT_FRAGMENT = "usbserial-588D0027491"
DEFAULT_BASE_URL = "http://192.168.0.46:8081"
DEFAULT_BAUD = 115200
HELLO_LINE = "PROVISION,HELLO"

_CHALLENGE_RE = re.compile(r"^[0-9a-f]{8}$")
_SAFE_FIELD_RE = re.compile(r"^[A-Za-z0-9._:-]+$")


class ProvisioningError(Exception):
    """Base class for expected, user-facing provisioning failures."""


class ProtocolError(ProvisioningError):
    """The device returned a malformed or unexpected protocol response."""


class DeviceProvisioningError(ProvisioningError):
    """The device rejected a provisioning request."""

    def __init__(self, code: str) -> None:
        self.code = code
        super().__init__(f"device rejected provisioning: {code}")


class PortDiscoveryError(ProvisioningError):
    """The expected USB serial port could not be selected unambiguously."""


@dataclass(frozen=True)
class Challenge:
    device_id: str
    challenge: str
    configured: bool


@dataclass(frozen=True)
class ProvisionResult:
    device_id: str
    restart_required: bool


def _validate_safe_field(value: str, field_name: str) -> None:
    if not value or len(value) > 128 or not _SAFE_FIELD_RE.fullmatch(value):
        raise ProtocolError(f"invalid {field_name} in device response")


def b64url_nopad(value: str) -> str:
    """Encode UTF-8 text as RFC 4648 URL-safe base64 without padding."""

    return base64.urlsafe_b64encode(value.encode("utf-8")).decode("ascii").rstrip("=")


def encode_protocol_line(line: str) -> bytes:
    """Encode one already-built protocol line for the serial transport."""

    if not line or "\r" in line or "\n" in line:
        raise ValueError("protocol line must contain exactly one non-empty line")
    return (line + "\n").encode("ascii")


def build_hello_line() -> str:
    return HELLO_LINE


def parse_challenge_line(line: str) -> Challenge:
    """Parse and validate a PROVISION,CHALLENGE response."""

    parts = line.rstrip("\r\n").split(",")
    if len(parts) != 5 or parts[:2] != ["PROVISION", "CHALLENGE"]:
        raise ProtocolError("expected a PROVISION,CHALLENGE response")

    _, _, device_id, challenge, configured = parts
    _validate_safe_field(device_id, "device_id")
    if not _CHALLENGE_RE.fullmatch(challenge):
        raise ProtocolError("device returned an invalid challenge")
    if configured not in {"0", "1"}:
        raise ProtocolError("device returned an invalid configured flag")

    return Challenge(
        device_id=device_id,
        challenge=challenge,
        configured=configured == "1",
    )


def _validate_input(
    value: str, field_name: str, *, allow_empty: bool = False
) -> None:
    if not value and not allow_empty:
        raise ProvisioningError(f"{field_name} must not be empty")
    if "\x00" in value or "\r" in value or "\n" in value:
        raise ProvisioningError(
            f"{field_name} contains an unsupported control character"
        )


def _validate_base_url(base_url: str) -> None:
    _validate_input(base_url, "base URL")
    try:
        parsed = urlsplit(base_url)
        # Accessing hostname also validates bracketed IPv6 host syntax.
        _ = parsed.hostname
    except ValueError:
        raise ProvisioningError("base URL must be an absolute HTTP URL") from None
    if parsed.scheme != "http" or not parsed.netloc:
        raise ProvisioningError("base URL must be an absolute HTTP URL")
    if parsed.username is not None or parsed.password is not None:
        raise ProvisioningError("base URL must not contain credentials")
    if parsed.query or parsed.fragment:
        raise ProvisioningError("base URL must not contain a query or fragment")


def build_set_line(
    challenge: str,
    ssid: str,
    password: str,
    base_url: str,
    token: str,
) -> str:
    """Build a PROVISION,SET line without ever interpolating raw secrets."""

    if not _CHALLENGE_RE.fullmatch(challenge):
        raise ProvisioningError("challenge must be exactly 8 lowercase hex characters")
    _validate_input(ssid, "SSID")
    _validate_input(password, "Wi-Fi password", allow_empty=True)
    _validate_base_url(base_url)
    _validate_input(token, "API token")

    encoded_fields = (
        b64url_nopad(ssid),
        b64url_nopad(password),
        b64url_nopad(base_url),
        b64url_nopad(token),
    )
    return "PROVISION,SET," + challenge + "," + ",".join(encoded_fields)


def parse_result_line(line: str) -> ProvisionResult:
    """Parse a terminal OK response or raise for a device ERROR response."""

    parts = line.rstrip("\r\n").split(",")
    if len(parts) == 3 and parts[:2] == ["PROVISION", "ERROR"]:
        code = parts[2]
        _validate_safe_field(code, "error code")
        raise DeviceProvisioningError(code)

    if (
        len(parts) != 4
        or parts[:2] != ["PROVISION", "OK"]
        or parts[3] != "restart_required"
    ):
        raise ProtocolError("expected a PROVISION,OK or PROVISION,ERROR response")

    device_id = parts[2]
    _validate_safe_field(device_id, "device_id")
    return ProvisionResult(device_id=device_id, restart_required=True)


def discover_port(
    ports: Iterable[object], fragment: str = DEFAULT_PORT_FRAGMENT
) -> str:
    """Select exactly one pyserial ListPortInfo matching the M5GO port name."""

    matches: set[str] = set()
    for port in ports:
        device = str(getattr(port, "device", ""))
        serial_number = str(getattr(port, "serial_number", ""))
        if fragment in device or fragment in serial_number:
            matches.add(device)

    matches.discard("")
    if not matches:
        raise PortDiscoveryError(
            f"no USB serial port matching {fragment!r}; reconnect the M5GO or use --port"
        )
    if len(matches) > 1:
        raise PortDiscoveryError(
            f"multiple USB serial ports match {fragment!r}; choose one with --port"
        )
    return next(iter(matches))


def read_protocol_line(
    serial_port: object,
    *,
    accepted_kinds: Sequence[str],
    timeout_seconds: float,
    monotonic=time.monotonic,
) -> str:
    """Wait for a relevant provisioning line, ignoring normal firmware logs."""

    deadline = monotonic() + timeout_seconds
    accepted = set(accepted_kinds)
    while monotonic() < deadline:
        raw = serial_port.readline()
        if not raw:
            continue
        try:
            line = raw.decode("utf-8").rstrip("\r\n")
        except UnicodeDecodeError:
            continue
        if not line.startswith("PROVISION,"):
            continue

        parts = line.split(",", 2)
        kind = parts[1] if len(parts) >= 2 else ""
        if kind in accepted:
            return line
        raise ProtocolError("device returned an unexpected provisioning response")

    raise ProtocolError("timed out waiting for a provisioning response")


def read_secrets_from_stdin(stream: TextIO) -> tuple[str, str]:
    """Read password then token, one line each, without echoing either value."""

    password_line = stream.readline()
    token_line = stream.readline()
    if password_line == "" or token_line == "":
        raise ProvisioningError(
            "--secrets-stdin requires two lines: Wi-Fi password, then API token"
        )
    password = password_line.rstrip("\r\n")
    token = token_line.rstrip("\r\n")
    _validate_input(password, "Wi-Fi password", allow_empty=True)
    _validate_input(token, "API token")
    return password, token


def create_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Provision M5GO Wi-Fi and API credentials over USB serial."
    )
    parser.add_argument(
        "--port",
        help=(
            "serial device path; by default, auto-discover a port containing "
            f"{DEFAULT_PORT_FRAGMENT}"
        ),
    )
    parser.add_argument("--ssid", help="Wi-Fi network name; prompts when omitted")
    parser.add_argument(
        "--base-url",
        default=DEFAULT_BASE_URL,
        help=f"backend base URL (default: {DEFAULT_BASE_URL})",
    )
    parser.add_argument(
        "--secrets-stdin",
        action="store_true",
        help="read Wi-Fi password and API token from stdin, one line each",
    )
    parser.add_argument(
        "--baud",
        type=int,
        default=DEFAULT_BAUD,
        help=f"serial baud rate (default: {DEFAULT_BAUD})",
    )
    return parser


def _load_pyserial():
    try:
        import serial  # type: ignore[import-not-found]
        from serial.tools import list_ports  # type: ignore[import-not-found]
    except ModuleNotFoundError as exc:
        raise ProvisioningError(
            "pyserial is required for USB access; install it with "
            "'python3 -m pip install pyserial'"
        ) from exc
    return serial, list_ports


def _prompt_inputs(
    args: argparse.Namespace,
    *,
    stdin: TextIO,
    stdout: TextIO,
) -> tuple[str, str, str]:
    ssid = args.ssid
    if args.secrets_stdin and ssid is None:
        raise ProvisioningError("--ssid is required when using --secrets-stdin")
    if ssid is None:
        print("Wi-Fi SSID: ", end="", flush=True, file=stdout)
        ssid = stdin.readline().rstrip("\r\n")
    _validate_input(ssid, "SSID")

    if args.secrets_stdin:
        password, token = read_secrets_from_stdin(stdin)
    else:
        try:
            password = getpass.getpass("Wi-Fi password: ")
            token = getpass.getpass("API token: ")
        except EOFError:
            raise ProvisioningError(
                "hidden input is unavailable; use --secrets-stdin"
            ) from None
        _validate_input(password, "Wi-Fi password", allow_empty=True)
        _validate_input(token, "API token")

    _validate_base_url(args.base_url)
    return ssid, password, token


def provision(
    *,
    serial_module: object,
    port: str,
    baud: int,
    ssid: str,
    password: str,
    base_url: str,
    token: str,
    stdout: TextIO,
) -> ProvisionResult:
    """Execute one HELLO/challenge/SET transaction."""

    try:
        connection = serial_module.Serial(
            port=port,
            baudrate=baud,
            timeout=0.25,
            write_timeout=2.0,
        )
        with connection:
            # A short settling period covers USB-UART adapters that reset the MCU
            # when the serial device is opened.
            time.sleep(0.75)
            connection.reset_input_buffer()
            connection.write(encode_protocol_line(build_hello_line()))
            connection.flush()

            challenge_line = read_protocol_line(
                connection,
                accepted_kinds=("CHALLENGE", "ERROR"),
                timeout_seconds=5.0,
            )
            if challenge_line.startswith("PROVISION,ERROR,"):
                parse_result_line(challenge_line)
                raise AssertionError("parse_result_line must raise for ERROR")
            challenge = parse_challenge_line(challenge_line)
            configured = "yes" if challenge.configured else "no"
            print(
                f"Device {challenge.device_id} detected (configured: {configured}).",
                file=stdout,
            )

            set_line = build_set_line(
                challenge.challenge,
                ssid,
                password,
                base_url,
                token,
            )
            connection.write(encode_protocol_line(set_line))
            connection.flush()

            result_line = read_protocol_line(
                connection,
                accepted_kinds=("OK", "ERROR"),
                timeout_seconds=10.0,
            )
            result = parse_result_line(result_line)
            if result.device_id != challenge.device_id:
                raise ProtocolError("result device_id does not match the challenge")
            return result
    except ProvisioningError:
        raise
    except Exception as exc:
        serial_exception = getattr(serial_module, "SerialException", None)
        is_serial_exception = isinstance(serial_exception, type) and isinstance(
            exc, serial_exception
        )
        if isinstance(exc, OSError) or is_serial_exception:
            # Do not include the exception text: a transport implementation could
            # include the just-written buffer, which contains encoded secrets.
            raise ProvisioningError(
                "serial connection failed; check the port and reconnect the M5GO"
            ) from None
        raise


def main(
    argv: Sequence[str] | None = None,
    *,
    stdin: TextIO | None = None,
    stdout: TextIO | None = None,
    stderr: TextIO | None = None,
) -> int:
    stdin = sys.stdin if stdin is None else stdin
    stdout = sys.stdout if stdout is None else stdout
    stderr = sys.stderr if stderr is None else stderr
    args = create_argument_parser().parse_args(argv)

    try:
        if args.baud <= 0:
            raise ProvisioningError("baud rate must be positive")
        serial_module, list_ports = _load_pyserial()
        port = args.port or discover_port(list_ports.comports())
        ssid, password, token = _prompt_inputs(args, stdin=stdin, stdout=stdout)
        print(f"Connecting to {port}...", file=stdout)
        result = provision(
            serial_module=serial_module,
            port=port,
            baud=args.baud,
            ssid=ssid,
            password=password,
            base_url=args.base_url,
            token=token,
            stdout=stdout,
        )
        print(f"Provisioned {result.device_id}; restart required.", file=stdout)
        return 0
    except (ProvisioningError, KeyboardInterrupt) as exc:
        if isinstance(exc, KeyboardInterrupt):
            print("Provisioning cancelled.", file=stderr)
        else:
            print(f"Provisioning failed: {exc}", file=stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
