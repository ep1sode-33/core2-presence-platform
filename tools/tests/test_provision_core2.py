from __future__ import annotations

import base64
import os
import stat
import tempfile
import unittest
from dataclasses import dataclass
from io import StringIO
from pathlib import Path
from typing import Self
from unittest import mock

from tools import provision_core2 as provision

TEST_OTA_SECRET = base64.urlsafe_b64encode(bytes(range(32))).decode("ascii").rstrip("=")


@dataclass
class FakePort:
    device: str
    serial_number: str | None = None


class FakeSerialReader:
    def __init__(self, lines: list[bytes]) -> None:
        self.lines = iter(lines)

    def readline(self) -> bytes:
        return next(self.lines, b"")


class FakeSerialConnection(FakeSerialReader):
    def __init__(self, lines: list[bytes]) -> None:
        super().__init__(lines)
        self.writes: list[bytes] = []
        self.reset_called = False

    def __enter__(self) -> Self:
        return self

    def __exit__(self, *_args: object) -> None:
        return None

    def reset_input_buffer(self) -> None:
        self.reset_called = True

    def write(self, value: bytes) -> int:
        self.writes.append(value)
        return len(value)

    def flush(self) -> None:
        return None


class FakeSerialModule:
    class SerialException(Exception):
        pass

    def __init__(self, connection: FakeSerialConnection) -> None:
        self.connection = connection
        self.open_kwargs: dict[str, object] | None = None

    def Serial(self, **kwargs: object) -> FakeSerialConnection:
        self.open_kwargs = kwargs
        return self.connection


class ProtocolEncodingTests(unittest.TestCase):
    def test_hello_line_is_exact(self) -> None:
        self.assertEqual(provision.build_hello_line(), "PROVISION,HELLO")
        self.assertEqual(
            provision.encode_protocol_line(provision.build_hello_line()),
            b"PROVISION,HELLO\n",
        )

    def test_b64url_encoding_is_utf8_urlsafe_and_unpadded(self) -> None:
        encoded = provision.b64url_nopad("café / ?")
        self.assertNotIn("=", encoded)
        self.assertNotIn("/", encoded)
        padding = "=" * (-len(encoded) % 4)
        self.assertEqual(
            base64.urlsafe_b64decode(encoded + padding).decode("utf-8"),
            "café / ?",
        )

    def test_set_line_has_exact_field_order_and_no_raw_secrets(self) -> None:
        line = provision.build_set_line(
            "01abcdef",
            "Home WiFi",
            "p@ss,word",
            "http://192.168.0.46:8081",
            "token/value+secret",
            TEST_OTA_SECRET,
        )
        expected_values = [
            provision.b64url_nopad(value)
            for value in (
                "Home WiFi",
                "p@ss,word",
                "http://192.168.0.46:8081",
                "token/value+secret",
                TEST_OTA_SECRET,
            )
        ]
        self.assertEqual(
            line,
            "PROVISION,SET,01abcdef," + ",".join(expected_values),
        )
        self.assertNotIn("p@ss,word", line)
        self.assertNotIn("token/value+secret", line)
        self.assertNotIn("=", line)

    def test_set_rejects_non_lowercase_challenge(self) -> None:
        with self.assertRaises(provision.ProvisioningError):
            provision.build_set_line(
                "01ABCDEF", "ssid", "password", "http://h", "token", TEST_OTA_SECRET
            )

    def test_set_rejects_credentials_embedded_in_base_url(self) -> None:
        with self.assertRaises(provision.ProvisioningError):
            provision.build_set_line(
                "01abcdef",
                "ssid",
                "password",
                "http://user:secret@host",
                "token",
                TEST_OTA_SECRET,
            )

    def test_set_rejects_malformed_base_url(self) -> None:
        with self.assertRaises(provision.ProvisioningError):
            provision.build_set_line(
                "01abcdef", "ssid", "password", "http://[", "token", TEST_OTA_SECRET
            )

    def test_set_accepts_open_wifi_password(self) -> None:
        line = provision.build_set_line(
            "01abcdef",
            "Open WiFi",
            "",
            "http://192.168.0.46:8081",
            "token",
            TEST_OTA_SECRET,
        )
        self.assertIn(",,", line)

    def test_set_rejects_https_query_and_fragment(self) -> None:
        for url in (
            "https://example.test",
            "http://example.test/path?query=1",
            "http://example.test/path#fragment",
        ):
            with self.subTest(url=url), self.assertRaises(provision.ProvisioningError):
                provision.build_set_line(
                    "01abcdef", "ssid", "password", url, "token", TEST_OTA_SECRET
                )

    def test_set_rejects_malformed_ota_secret(self) -> None:
        for value in ("", "too-short", "A" * 42, "A" * 44, "+" + "A" * 42):
            with (
                self.subTest(value=value),
                self.assertRaises(provision.ProvisioningError),
            ):
                provision.build_set_line(
                    "01abcdef",
                    "ssid",
                    "password",
                    "http://192.168.0.46:8081",
                    "token",
                    value,
                )


class ProtocolParsingTests(unittest.TestCase):
    def test_parse_challenge(self) -> None:
        challenge = provision.parse_challenge_line(
            "PROVISION,CHALLENGE,core2-aabbccddeeff,01abcdef,1\r\n"
        )
        self.assertEqual(challenge.device_id, "core2-aabbccddeeff")
        self.assertEqual(challenge.challenge, "01abcdef")
        self.assertTrue(challenge.configured)

    def test_parse_challenge_rejects_bad_configured_flag(self) -> None:
        with self.assertRaises(provision.ProtocolError):
            provision.parse_challenge_line(
                "PROVISION,CHALLENGE,core2-aabbccddeeff,01abcdef,true"
            )

    def test_parse_challenge_rejects_uppercase_hex(self) -> None:
        with self.assertRaises(provision.ProtocolError):
            provision.parse_challenge_line(
                "PROVISION,CHALLENGE,core2-aabbccddeeff,01ABCDEF,0"
            )

    def test_parse_ok(self) -> None:
        result = provision.parse_result_line(
            "PROVISION,OK,core2-aabbccddeeff,restart_required\n"
        )
        self.assertEqual(result.device_id, "core2-aabbccddeeff")
        self.assertTrue(result.restart_required)

    def test_parse_device_error(self) -> None:
        with self.assertRaises(provision.DeviceProvisioningError) as caught:
            provision.parse_result_line("PROVISION,ERROR,INVALID_CHALLENGE")
        self.assertEqual(caught.exception.code, "INVALID_CHALLENGE")

    def test_reader_ignores_regular_firmware_output(self) -> None:
        reader = FakeSerialReader(
            [
                b"DATA,123,0,0\n",
                b"EVENT,DISPLAY,ON\n",
                b"PROVISION,CHALLENGE,core2-aabbccddeeff,01abcdef,0\n",
            ]
        )
        ticks = iter((0.0, 0.1, 0.2, 0.3, 0.4))
        line = provision.read_protocol_line(
            reader,
            accepted_kinds=("CHALLENGE", "ERROR"),
            timeout_seconds=1.0,
            monotonic=lambda: next(ticks),
        )
        self.assertEqual(
            line,
            "PROVISION,CHALLENGE,core2-aabbccddeeff,01abcdef,0",
        )

    def test_reader_rejects_unexpected_provisioning_kind(self) -> None:
        reader = FakeSerialReader([b"PROVISION,OK,device,restart_required\n"])
        ticks = iter((0.0, 0.1, 0.2))
        with self.assertRaises(provision.ProtocolError):
            provision.read_protocol_line(
                reader,
                accepted_kinds=("CHALLENGE",),
                timeout_seconds=1.0,
                monotonic=lambda: next(ticks),
            )


class PortAndInputTests(unittest.TestCase):
    def test_discover_port_by_device_name(self) -> None:
        ports = [
            FakePort("/dev/cu.Bluetooth-Incoming-Port"),
            FakePort("/dev/cu.usbserial-588D0027491"),
        ]
        self.assertEqual(
            provision.discover_port(ports),
            "/dev/cu.usbserial-588D0027491",
        )

    def test_discover_port_by_serial_number(self) -> None:
        ports = [FakePort("/dev/ttyUSB0", "usbserial-588D0027491")]
        self.assertEqual(provision.discover_port(ports), "/dev/ttyUSB0")

    def test_discover_requires_one_match(self) -> None:
        with self.assertRaises(provision.PortDiscoveryError):
            provision.discover_port([])
        with self.assertRaises(provision.PortDiscoveryError):
            provision.discover_port(
                [
                    FakePort("/dev/cu.usbserial-588D0027491"),
                    FakePort("/dev/tty.usbserial-588D0027491"),
                ]
            )

    def test_secrets_stdin_reads_exactly_two_lines(self) -> None:
        password, token = provision.read_secrets_from_stdin(
            StringIO("  password with spaces  \napi-token\n")
        )
        self.assertEqual(password, "  password with spaces  ")
        self.assertEqual(token, "api-token")

    def test_secrets_stdin_rejects_missing_token_line(self) -> None:
        with self.assertRaises(provision.ProvisioningError):
            provision.read_secrets_from_stdin(StringIO("password\n"))

    def test_secrets_stdin_accepts_open_wifi_password(self) -> None:
        password, token = provision.read_secrets_from_stdin(StringIO("\napi-token\n"))
        self.assertEqual(password, "")
        self.assertEqual(token, "api-token")

    def test_cli_has_no_secret_value_arguments(self) -> None:
        option_strings = {
            option
            for action in provision.create_argument_parser()._actions
            for option in action.option_strings
        }
        self.assertNotIn("--password", option_strings)
        self.assertNotIn("--token", option_strings)
        self.assertNotIn("--ota-secret", option_strings)
        self.assertIn("--ota-secret-store", option_strings)
        self.assertIn("--rotate-ota-secret", option_strings)

    def test_ota_secret_store_creates_reuses_and_rotates_securely(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            store = Path(temporary_directory) / "secrets"
            first = provision.load_or_create_ota_secret(
                store,
                "core2-aabbccddeeff",
                random_bytes=lambda count: bytes([1]) * count,
            )
            reused = provision.load_or_create_ota_secret(
                store,
                "core2-aabbccddeeff",
                random_bytes=lambda count: bytes([2]) * count,
            )
            rotated = provision.load_or_create_ota_secret(
                store,
                "core2-aabbccddeeff",
                rotate=True,
                random_bytes=lambda count: bytes([3]) * count,
            )

            secret_path = store / "core2-aabbccddeeff.secret"
            self.assertEqual(first, reused)
            self.assertNotEqual(first, rotated)
            self.assertEqual(secret_path.read_text(encoding="ascii").strip(), rotated)
            self.assertEqual(stat.S_IMODE(store.stat().st_mode), 0o700)
            self.assertEqual(stat.S_IMODE(secret_path.stat().st_mode), 0o600)
            self.assertEqual(len(rotated), 43)

    def test_ota_secret_store_rejects_insecure_existing_file(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            store = Path(temporary_directory)
            secret_path = store / "core2-aabbccddeeff.secret"
            secret_path.write_text(TEST_OTA_SECRET + "\n", encoding="ascii")
            os.chmod(secret_path, 0o644)
            with self.assertRaises(provision.ProvisioningError):
                provision.load_or_create_ota_secret(store, "core2-aabbccddeeff")


class TransactionTests(unittest.TestCase):
    def test_provision_performs_exact_handshake_without_printing_secrets(self) -> None:
        connection = FakeSerialConnection(
            [
                b"boot diagnostic\n",
                b"PROVISION,CHALLENGE,core2-aabbccddeeff,01abcdef,0\n",
                b"PROVISION,OK,core2-aabbccddeeff,restart_required\n",
            ]
        )
        serial_module = FakeSerialModule(connection)
        output = StringIO()

        with mock.patch.object(provision.time, "sleep"):
            result = provision.provision(
                serial_module=serial_module,
                port="/dev/cu.usbserial-588D0027491",
                baud=115200,
                ssid="Home WiFi",
                password="do-not-print-password",
                base_url=provision.DEFAULT_BASE_URL,
                token="do-not-print-token",
                ota_secret_provider=lambda _device_id: TEST_OTA_SECRET,
                stdout=output,
            )

        self.assertEqual(result.device_id, "core2-aabbccddeeff")
        self.assertTrue(connection.reset_called)
        self.assertEqual(connection.writes[0], b"PROVISION,HELLO\n")
        expected_set = provision.build_set_line(
            "01abcdef",
            "Home WiFi",
            "do-not-print-password",
            provision.DEFAULT_BASE_URL,
            "do-not-print-token",
            TEST_OTA_SECRET,
        )
        self.assertEqual(
            connection.writes[1], provision.encode_protocol_line(expected_set)
        )
        self.assertNotIn("do-not-print-password", output.getvalue())
        self.assertNotIn("do-not-print-token", output.getvalue())
        self.assertNotIn(TEST_OTA_SECRET, output.getvalue())

    def test_provision_rejects_result_from_different_device(self) -> None:
        connection = FakeSerialConnection(
            [
                b"PROVISION,CHALLENGE,core2-aabbccddeeff,01abcdef,1\n",
                b"PROVISION,OK,core2-001122334455,restart_required\n",
            ]
        )
        serial_module = FakeSerialModule(connection)

        with (
            mock.patch.object(provision.time, "sleep"),
            self.assertRaises(provision.ProtocolError),
        ):
            provision.provision(
                serial_module=serial_module,
                port="/dev/cu.usbserial-588D0027491",
                baud=115200,
                ssid="ssid",
                password="password",
                base_url=provision.DEFAULT_BASE_URL,
                token="token",
                ota_secret_provider=lambda _device_id: TEST_OTA_SECRET,
                stdout=StringIO(),
            )


if __name__ == "__main__":
    unittest.main()
