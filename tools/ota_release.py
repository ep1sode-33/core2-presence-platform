#!/usr/bin/env python3
"""Build and verify immutable M5GO signed OTA release bundles.

The signing key is always supplied by the caller. This module never creates or
stores a private key. OpenSSL is used for ECDSA so the tool has no Python crypto
package dependency.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import zipfile
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

MANIFEST_MAGIC = b"M5OT"
MANIFEST_FORMAT_VERSION = 1
SIGNATURE_FORMAT_VERSION = 1
SIGNATURE_SIZE = 64
SHA256_SIZE = 32
MAX_MANIFEST_SIZE = 320
MAX_HARDWARE_LENGTH = 48
MAX_VERSION_LENGTH = 32
MAX_BUILD_ID_LENGTH = 64
MAX_KEY_ID_LENGTH = 32
MAX_FIRMWARE_SIZE = 0x640000
MAX_ELF_SIZE = 64 * 1024 * 1024
MAX_RELEASE_COUNTER = (1 << 63) - 1
M5GO_HARDWARE_MODEL = "m5go-classic-esp32-16m"

BUNDLE_FIRMWARE = "firmware.bin"
BUNDLE_ELF = "firmware.elf"
BUNDLE_MANIFEST = "manifest.bin"
BUNDLE_SIGNATURE = "manifest.sig"
BUNDLE_MEMBERS = (
    BUNDLE_FIRMWARE,
    BUNDLE_ELF,
    BUNDLE_MANIFEST,
    BUNDLE_SIGNATURE,
)

ARTIFACT_IDENTITY_MAGIC = b"\x89M5GO-FW-ID\r\n\x1a\n\x7f"
ARTIFACT_IDENTITY_TRAILER = b"\xffM5GO-FW-ID-END\x00"
ARTIFACT_IDENTITY_FORMAT_VERSION = 1
ARTIFACT_IDENTITY_HARDWARE_CAPACITY = 49
ARTIFACT_IDENTITY_VERSION_CAPACITY = 33
ARTIFACT_IDENTITY_BUILD_ID_CAPACITY = 65
ARTIFACT_IDENTITY_SIZE = (
    len(ARTIFACT_IDENTITY_MAGIC)
    + 4
    + ARTIFACT_IDENTITY_HARDWARE_CAPACITY
    + ARTIFACT_IDENTITY_VERSION_CAPACITY
    + ARTIFACT_IDENTITY_BUILD_ID_CAPACITY
    + len(ARTIFACT_IDENTITY_TRAILER)
)

P256_ORDER = int("FFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551", 16)
P256_HALF_ORDER = P256_ORDER // 2
EC_PUBLIC_KEY_OID = bytes.fromhex("06072a8648ce3d0201")
P256_CURVE_OID = bytes.fromhex("06082a8648ce3d030107")


class OtaReleaseError(ValueError):
    """An OTA artifact is malformed, unsafe, or cryptographically invalid."""


@dataclass(frozen=True)
class OtaManifest:
    hardware: str
    firmware_version: str
    release_counter: int
    build_id: str
    signing_key_id: str
    firmware_size: int
    firmware_sha256: bytes
    elf_size: int
    elf_sha256: bytes
    signature_format_version: int = SIGNATURE_FORMAT_VERSION


@dataclass(frozen=True)
class ArtifactIdentity:
    hardware: str
    firmware_version: str
    build_id: str


def _encode_text(name: str, value: str, maximum: int) -> bytes:
    try:
        encoded = value.encode("ascii")
    except UnicodeEncodeError as error:
        raise OtaReleaseError(f"{name} must be ASCII") from error
    if not encoded or len(encoded) > maximum:
        raise OtaReleaseError(f"{name} length must be 1..{maximum} bytes")
    if any(
        not (
            ord("a") <= byte <= ord("z")
            or ord("A") <= byte <= ord("Z")
            or ord("0") <= byte <= ord("9")
            or byte in b"._+-"
        )
        for byte in encoded
    ):
        raise OtaReleaseError(f"{name} contains a non-canonical character")
    return struct.pack("!H", len(encoded)) + encoded


def _encode_digest(name: str, value: bytes) -> bytes:
    if len(value) != SHA256_SIZE:
        raise OtaReleaseError(f"{name} must be exactly {SHA256_SIZE} bytes")
    return struct.pack("!H", len(value)) + value


def _encode_identity_field(name: str, value: str, capacity: int) -> bytes:
    encoded = _encode_text(name, value, capacity - 1)[2:]
    return encoded + bytes(capacity - len(encoded))


def encode_artifact_identity_marker(identity: ArtifactIdentity) -> bytes:
    """Encode the fixed record compiled into both release artifacts."""

    hardware = _encode_identity_field(
        "hardware", identity.hardware, ARTIFACT_IDENTITY_HARDWARE_CAPACITY
    )
    firmware_version = _encode_identity_field(
        "firmware_version",
        identity.firmware_version,
        ARTIFACT_IDENTITY_VERSION_CAPACITY,
    )
    build_id = _encode_identity_field(
        "build_id", identity.build_id, ARTIFACT_IDENTITY_BUILD_ID_CAPACITY
    )
    marker = b"".join(
        (
            ARTIFACT_IDENTITY_MAGIC,
            bytes(
                (
                    ARTIFACT_IDENTITY_FORMAT_VERSION,
                    len(identity.hardware),
                    len(identity.firmware_version),
                    len(identity.build_id),
                )
            ),
            hardware,
            firmware_version,
            build_id,
            ARTIFACT_IDENTITY_TRAILER,
        )
    )
    if len(marker) != ARTIFACT_IDENTITY_SIZE:
        raise AssertionError("artifact identity marker layout changed")
    return marker


def _decode_identity_field(
    record: bytes,
    position: int,
    length: int,
    capacity: int,
    name: str,
) -> tuple[str, int]:
    end = position + capacity
    field = record[position:end]
    if not 0 < length < capacity:
        raise OtaReleaseError(f"artifact {name} length is outside bounds")
    if any(field[length:]):
        raise OtaReleaseError(f"artifact {name} padding is not canonical")
    try:
        value = field[:length].decode("ascii")
    except UnicodeDecodeError as error:
        raise OtaReleaseError(f"artifact {name} must be ASCII") from error
    if _encode_text(name, value, capacity - 1)[2:] != field[:length]:
        raise OtaReleaseError(f"artifact {name} is not canonical")
    return value, end


def extract_artifact_identity(data: bytes, artifact_name: str) -> ArtifactIdentity:
    """Return the one unambiguous versioned identity embedded in an artifact."""

    marker_count = data.count(ARTIFACT_IDENTITY_MAGIC)
    if marker_count == 0:
        raise OtaReleaseError(
            f"{artifact_name} is missing the firmware identity marker"
        )
    if marker_count != 1:
        raise OtaReleaseError(
            f"{artifact_name} contains ambiguous firmware identity markers"
        )
    start = data.index(ARTIFACT_IDENTITY_MAGIC)
    end = start + ARTIFACT_IDENTITY_SIZE
    if end > len(data):
        raise OtaReleaseError(f"{artifact_name} firmware identity marker is truncated")
    record = data[start:end]
    if record[-len(ARTIFACT_IDENTITY_TRAILER) :] != ARTIFACT_IDENTITY_TRAILER:
        raise OtaReleaseError(f"{artifact_name} firmware identity trailer is invalid")
    position = len(ARTIFACT_IDENTITY_MAGIC)
    format_version, hardware_length, version_length, build_id_length = record[
        position : position + 4
    ]
    position += 4
    if format_version != ARTIFACT_IDENTITY_FORMAT_VERSION:
        raise OtaReleaseError(
            f"{artifact_name} firmware identity version is unsupported"
        )
    hardware, position = _decode_identity_field(
        record,
        position,
        hardware_length,
        ARTIFACT_IDENTITY_HARDWARE_CAPACITY,
        "hardware",
    )
    firmware_version, position = _decode_identity_field(
        record,
        position,
        version_length,
        ARTIFACT_IDENTITY_VERSION_CAPACITY,
        "firmware_version",
    )
    build_id, position = _decode_identity_field(
        record,
        position,
        build_id_length,
        ARTIFACT_IDENTITY_BUILD_ID_CAPACITY,
        "build_id",
    )
    if position != ARTIFACT_IDENTITY_SIZE - len(ARTIFACT_IDENTITY_TRAILER):
        raise AssertionError("artifact identity parser layout changed")
    identity = ArtifactIdentity(hardware, firmware_version, build_id)
    if encode_artifact_identity_marker(identity) != record:
        raise OtaReleaseError(
            f"{artifact_name} firmware identity marker is not canonical"
        )
    return identity


def _require_manifest_artifact_identity(
    manifest: OtaManifest,
    firmware: bytes,
    elf: bytes,
) -> None:
    firmware_identity = extract_artifact_identity(firmware, BUNDLE_FIRMWARE)
    elf_identity = extract_artifact_identity(elf, BUNDLE_ELF)
    expected = ArtifactIdentity(
        hardware=manifest.hardware,
        firmware_version=manifest.firmware_version,
        build_id=manifest.build_id,
    )
    if firmware_identity != elf_identity:
        raise OtaReleaseError("firmware BIN and ELF identity markers do not match")
    if firmware_identity.hardware != M5GO_HARDWARE_MODEL:
        raise OtaReleaseError(
            f"firmware artifact hardware must be {M5GO_HARDWARE_MODEL}"
        )
    if firmware_identity != expected:
        raise OtaReleaseError(
            "firmware artifact identity does not match the signed manifest"
        )


def encode_manifest(manifest: OtaManifest) -> bytes:
    """Return the one canonical network-byte-order encoding for manifest v1."""

    if not 0 < manifest.release_counter <= MAX_RELEASE_COUNTER:
        raise OtaReleaseError("release_counter must be in 1..2^63-1")
    if not 0 < manifest.firmware_size <= MAX_FIRMWARE_SIZE:
        raise OtaReleaseError(
            f"firmware_size must fit the {MAX_FIRMWARE_SIZE}-byte OTA slot"
        )
    if not 0 < manifest.elf_size <= MAX_ELF_SIZE:
        raise OtaReleaseError(f"elf_size must be in 1..{MAX_ELF_SIZE}")
    if manifest.signature_format_version != SIGNATURE_FORMAT_VERSION:
        raise OtaReleaseError("unsupported signature format version")

    body = b"".join(
        (
            _encode_text("hardware", manifest.hardware, MAX_HARDWARE_LENGTH),
            _encode_text(
                "firmware_version", manifest.firmware_version, MAX_VERSION_LENGTH
            ),
            struct.pack("!Q", manifest.release_counter),
            _encode_text("build_id", manifest.build_id, MAX_BUILD_ID_LENGTH),
            _encode_text("signing_key_id", manifest.signing_key_id, MAX_KEY_ID_LENGTH),
            struct.pack("!I", manifest.firmware_size),
            _encode_digest("firmware_sha256", manifest.firmware_sha256),
            struct.pack("!I", manifest.elf_size),
            _encode_digest("elf_sha256", manifest.elf_sha256),
        )
    )
    total_length = 8 + len(body)
    if total_length > MAX_MANIFEST_SIZE:
        raise OtaReleaseError("manifest exceeds the v1 size limit")
    return (
        MANIFEST_MAGIC
        + bytes((MANIFEST_FORMAT_VERSION, manifest.signature_format_version))
        + struct.pack("!H", total_length)
        + body
    )


class _ManifestReader:
    def __init__(self, data: bytes) -> None:
        self.data = data
        self.position = 8

    def exact(self, size: int) -> bytes:
        if size < 0 or self.position + size > len(self.data):
            raise OtaReleaseError("manifest is truncated")
        result = self.data[self.position : self.position + size]
        self.position += size
        return result

    def u16(self) -> int:
        return struct.unpack("!H", self.exact(2))[0]

    def u32(self) -> int:
        return struct.unpack("!I", self.exact(4))[0]

    def u64(self) -> int:
        return struct.unpack("!Q", self.exact(8))[0]

    def text(self, name: str, maximum: int) -> str:
        size = self.u16()
        value = self.exact(size)
        try:
            decoded = value.decode("ascii")
        except UnicodeDecodeError as error:
            raise OtaReleaseError(f"{name} must be ASCII") from error
        # Reuse the canonical encoder so decoding cannot accept a second spelling.
        if _encode_text(name, decoded, maximum)[2:] != value:
            raise OtaReleaseError(f"{name} is not canonical")
        return decoded

    def digest(self, name: str) -> bytes:
        size = self.u16()
        if size != SHA256_SIZE:
            raise OtaReleaseError(f"{name} has the wrong length")
        return self.exact(size)


def decode_manifest(data: bytes) -> OtaManifest:
    if len(data) < 8 or len(data) > MAX_MANIFEST_SIZE:
        raise OtaReleaseError("manifest length is outside the v1 bounds")
    if data[:4] != MANIFEST_MAGIC:
        raise OtaReleaseError("bad manifest magic")
    if data[4] != MANIFEST_FORMAT_VERSION:
        raise OtaReleaseError("unsupported manifest format version")
    if data[5] != SIGNATURE_FORMAT_VERSION:
        raise OtaReleaseError("unsupported signature format version")
    if struct.unpack("!H", data[6:8])[0] != len(data):
        raise OtaReleaseError("manifest total length does not match")

    reader = _ManifestReader(data)
    hardware = reader.text("hardware", MAX_HARDWARE_LENGTH)
    firmware_version = reader.text("firmware_version", MAX_VERSION_LENGTH)
    release_counter = reader.u64()
    build_id = reader.text("build_id", MAX_BUILD_ID_LENGTH)
    signing_key_id = reader.text("signing_key_id", MAX_KEY_ID_LENGTH)
    firmware_size = reader.u32()
    firmware_sha256 = reader.digest("firmware_sha256")
    elf_size = reader.u32()
    elf_sha256 = reader.digest("elf_sha256")
    if reader.position != len(data):
        raise OtaReleaseError("manifest has trailing data")

    manifest = OtaManifest(
        hardware=hardware,
        firmware_version=firmware_version,
        release_counter=release_counter,
        build_id=build_id,
        signing_key_id=signing_key_id,
        firmware_size=firmware_size,
        firmware_sha256=firmware_sha256,
        elf_size=elf_size,
        elf_sha256=elf_sha256,
        signature_format_version=data[5],
    )
    if encode_manifest(manifest) != data:
        raise OtaReleaseError("manifest is not canonically encoded")
    return manifest


def _read_der_length(data: bytes, position: int) -> tuple[int, int]:
    if position >= len(data):
        raise OtaReleaseError("truncated DER length")
    first = data[position]
    position += 1
    if first < 0x80:
        return first, position
    octets = first & 0x7F
    if octets == 0 or octets > 4 or position + octets > len(data):
        raise OtaReleaseError("invalid DER length")
    if data[position] == 0:
        raise OtaReleaseError("non-canonical DER length")
    length = int.from_bytes(data[position : position + octets], "big")
    if length < 0x80:
        raise OtaReleaseError("non-canonical DER length")
    return length, position + octets


def _read_der_tlv(data: bytes, position: int, expected_tag: int) -> tuple[bytes, int]:
    if position >= len(data) or data[position] != expected_tag:
        raise OtaReleaseError("unexpected DER tag")
    length, value_position = _read_der_length(data, position + 1)
    end = value_position + length
    if end > len(data):
        raise OtaReleaseError("truncated DER value")
    return data[value_position:end], end


def _decode_der_integer(value: bytes) -> int:
    if not value or value[0] & 0x80:
        raise OtaReleaseError("ECDSA integer is missing a positive sign")
    if len(value) > 1 and value[0] == 0 and not value[1] & 0x80:
        raise OtaReleaseError("ECDSA integer has redundant padding")
    result = int.from_bytes(value, "big")
    if not 0 < result < P256_ORDER:
        raise OtaReleaseError("ECDSA integer is outside the P-256 order")
    return result


def der_signature_to_raw(der_signature: bytes) -> bytes:
    sequence, end = _read_der_tlv(der_signature, 0, 0x30)
    if end != len(der_signature):
        raise OtaReleaseError("ECDSA signature has trailing DER data")
    r_bytes, position = _read_der_tlv(sequence, 0, 0x02)
    s_bytes, position = _read_der_tlv(sequence, position, 0x02)
    if position != len(sequence):
        raise OtaReleaseError("ECDSA signature has trailing sequence data")
    r = _decode_der_integer(r_bytes)
    s = _decode_der_integer(s_bytes)
    # Require one canonical representation so signature files are stable inputs.
    if s > P256_HALF_ORDER:
        s = P256_ORDER - s
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


def _encode_der_length(length: int) -> bytes:
    if length < 0:
        raise OtaReleaseError("negative DER length")
    if length < 0x80:
        return bytes((length,))
    encoded = length.to_bytes((length.bit_length() + 7) // 8, "big")
    return bytes((0x80 | len(encoded),)) + encoded


def _encode_der_integer(value: int) -> bytes:
    if not 0 < value < P256_ORDER:
        raise OtaReleaseError("ECDSA integer is outside the P-256 order")
    encoded = value.to_bytes((value.bit_length() + 7) // 8, "big")
    if encoded[0] & 0x80:
        encoded = b"\x00" + encoded
    return b"\x02" + _encode_der_length(len(encoded)) + encoded


def raw_signature_to_der(raw_signature: bytes) -> bytes:
    if len(raw_signature) != SIGNATURE_SIZE:
        raise OtaReleaseError("signature must be exactly 64 bytes")
    r = int.from_bytes(raw_signature[:32], "big")
    s = int.from_bytes(raw_signature[32:], "big")
    if s > P256_HALF_ORDER:
        raise OtaReleaseError("signature is not canonical low-S P-256")
    body = _encode_der_integer(r) + _encode_der_integer(s)
    return b"\x30" + _encode_der_length(len(body)) + body


def _run_openssl(arguments: Sequence[str]) -> bytes:
    try:
        completed = subprocess.run(
            ("openssl", *arguments),
            check=False,
            capture_output=True,
        )
    except FileNotFoundError as error:
        raise OtaReleaseError("OpenSSL is required but was not found") from error
    if completed.returncode != 0:
        detail = completed.stderr.decode("utf-8", errors="replace").strip()
        raise OtaReleaseError(f"OpenSSL failed: {detail or 'unknown error'}")
    return completed.stdout


def _validate_p256_spki(der_public_key: bytes) -> bytes:
    sequence, end = _read_der_tlv(der_public_key, 0, 0x30)
    if end != len(der_public_key):
        raise OtaReleaseError("public key has trailing DER data")
    algorithm, position = _read_der_tlv(sequence, 0, 0x30)
    if algorithm != EC_PUBLIC_KEY_OID + P256_CURVE_OID:
        raise OtaReleaseError("signing key must use ECDSA P-256/prime256v1")
    bit_string, position = _read_der_tlv(sequence, position, 0x03)
    if position != len(sequence) or len(bit_string) != 66 or bit_string[0] != 0:
        raise OtaReleaseError("public key must be uncompressed P-256")
    public_point = bit_string[1:]
    if public_point[0] != 0x04:
        raise OtaReleaseError("public key must use SEC1 uncompressed form")
    return public_point


def public_point_from_private_key(
    private_key: Path, password_environment: str | None = None
) -> bytes:
    arguments = ["pkey", "-in", os.fspath(private_key), "-pubout", "-outform", "DER"]
    if password_environment:
        if password_environment not in os.environ:
            raise OtaReleaseError(
                f"private-key password environment variable is unset: {password_environment}"
            )
        arguments.extend(("-passin", f"env:{password_environment}"))
    return _validate_p256_spki(_run_openssl(arguments))


def public_point_from_public_key(public_key: Path) -> bytes:
    der = _run_openssl(
        ("pkey", "-pubin", "-in", os.fspath(public_key), "-outform", "DER")
    )
    return _validate_p256_spki(der)


def write_trust_header(
    *,
    keys: Sequence[tuple[str, Path]],
    output: Path,
    overwrite: bool = False,
) -> None:
    """Write the public-only current/next OTA trust set consumed by firmware."""

    if not 1 <= len(keys) <= 2:
        raise OtaReleaseError("firmware trust set requires one or two public keys")
    if output.exists() and not overwrite:
        raise OtaReleaseError(f"refusing to overwrite existing trust header: {output}")
    seen_ids: set[str] = set()
    entries: list[tuple[str, bytes]] = []
    for key_id, public_key in keys:
        if (
            not key_id
            or len(key_id) > MAX_KEY_ID_LENGTH
            or any(
                not (
                    character.isascii() and (character.isalnum() or character in "._-")
                )
                for character in key_id
            )
        ):
            raise OtaReleaseError("OTA trust key ID is not canonical ASCII")
        if key_id in seen_ids:
            raise OtaReleaseError("OTA trust key IDs must be unique")
        seen_ids.add(key_id)
        entries.append((key_id, public_point_from_public_key(public_key)))

    lines = [
        "#pragma once",
        "",
        "// Generated by tools/ota_release.py. Public keys only; do not edit.",
        "inline constexpr OtaTrustKey kCompiledOtaTrustKeys[] = {",
    ]
    for key_id, point in entries:
        encoded = ", ".join(f"0x{byte:02x}" for byte in point)
        lines.append(f'    {{"{key_id}", {{{encoded}}}}},')
    lines.extend(
        (
            "};",
            "inline constexpr size_t kCompiledOtaTrustKeyCount =",
            "    sizeof(kCompiledOtaTrustKeys) / sizeof(kCompiledOtaTrustKeys[0]);",
            "",
        )
    )
    payload = "\n".join(lines)
    output.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{output.name}.", suffix=".tmp", dir=output.parent
    )
    temporary_output = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="ascii", newline="\n") as target:
            target.write(payload)
            target.flush()
            os.fsync(target.fileno())
        os.replace(temporary_output, output)
    finally:
        if temporary_output.exists():
            temporary_output.unlink()


def _private_key_arguments(
    private_key: Path, password_environment: str | None
) -> list[str]:
    result = ["-sign", os.fspath(private_key)]
    if password_environment:
        if password_environment not in os.environ:
            raise OtaReleaseError(
                f"private-key password environment variable is unset: {password_environment}"
            )
        result.extend(("-passin", f"env:{password_environment}"))
    return result


def sign_manifest(
    manifest: bytes,
    private_key: Path,
    password_environment: str | None = None,
) -> bytes:
    # Validate the exact curve before signing; OpenSSL otherwise accepts any EC key.
    public_point_from_private_key(private_key, password_environment)
    with tempfile.TemporaryDirectory(prefix="m5go-ota-sign-") as temporary:
        temporary_path = Path(temporary)
        manifest_path = temporary_path / "manifest.bin"
        signature_path = temporary_path / "manifest.der"
        manifest_path.write_bytes(manifest)
        arguments = ["dgst", "-sha256"]
        arguments.extend(_private_key_arguments(private_key, password_environment))
        arguments.extend(("-out", os.fspath(signature_path), os.fspath(manifest_path)))
        _run_openssl(arguments)
        return der_signature_to_raw(signature_path.read_bytes())


def verify_manifest_signature(
    manifest: bytes, raw_signature: bytes, public_key: Path
) -> None:
    # Parsing enforces both P-256 and low-S canonical form before OpenSSL sees it.
    public_point_from_public_key(public_key)
    der_signature = raw_signature_to_der(raw_signature)
    with tempfile.TemporaryDirectory(prefix="m5go-ota-verify-") as temporary:
        temporary_path = Path(temporary)
        manifest_path = temporary_path / "manifest.bin"
        signature_path = temporary_path / "manifest.der"
        manifest_path.write_bytes(manifest)
        signature_path.write_bytes(der_signature)
        _run_openssl(
            (
                "dgst",
                "-sha256",
                "-verify",
                os.fspath(public_key),
                "-signature",
                os.fspath(signature_path),
                os.fspath(manifest_path),
            )
        )


def _read_file_bounded(path: Path, maximum_size: int, name: str) -> bytes:
    with path.open("rb") as source:
        payload = source.read(maximum_size + 1)
    if not payload or len(payload) > maximum_size:
        raise OtaReleaseError(f"{name} length is outside bounds")
    return payload


def _zip_info(name: str) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_STORED
    info.create_system = 3
    info.external_attr = (0o100644 & 0xFFFF) << 16
    return info


def create_bundle(
    *,
    firmware_bin: Path,
    firmware_elf: Path,
    private_key: Path,
    output: Path,
    hardware: str,
    firmware_version: str,
    release_counter: int,
    build_id: str,
    signing_key_id: str,
    private_key_password_environment: str | None = None,
    overwrite: bool = False,
) -> OtaManifest:
    if output.exists() and not overwrite:
        raise OtaReleaseError(f"refusing to overwrite existing bundle: {output}")
    firmware = _read_file_bounded(firmware_bin, MAX_FIRMWARE_SIZE, "firmware")
    elf = _read_file_bounded(firmware_elf, MAX_ELF_SIZE, "ELF")
    manifest = OtaManifest(
        hardware=hardware,
        firmware_version=firmware_version,
        release_counter=release_counter,
        build_id=build_id,
        signing_key_id=signing_key_id,
        firmware_size=len(firmware),
        firmware_sha256=hashlib.sha256(firmware).digest(),
        elf_size=len(elf),
        elf_sha256=hashlib.sha256(elf).digest(),
    )
    encoded_manifest = encode_manifest(manifest)
    _require_manifest_artifact_identity(manifest, firmware, elf)
    signature = sign_manifest(
        encoded_manifest, private_key, private_key_password_environment
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{output.name}.", suffix=".tmp", dir=output.parent
    )
    os.close(descriptor)
    temporary_output = Path(temporary_name)
    try:
        with zipfile.ZipFile(temporary_output, "w") as bundle:
            bundle.writestr(_zip_info(BUNDLE_FIRMWARE), firmware)
            bundle.writestr(_zip_info(BUNDLE_ELF), elf)
            bundle.writestr(_zip_info(BUNDLE_MANIFEST), encoded_manifest)
            bundle.writestr(_zip_info(BUNDLE_SIGNATURE), signature)
        os.replace(temporary_output, output)
    finally:
        if temporary_output.exists():
            temporary_output.unlink()
    return manifest


def verify_bundle(
    *,
    bundle_path: Path,
    public_key: Path,
    expected_hardware: str | None = None,
    current_release_counter: int | None = None,
) -> OtaManifest:
    if current_release_counter is not None and not (
        0 <= current_release_counter <= MAX_RELEASE_COUNTER
    ):
        raise OtaReleaseError("current release counter is outside signed-64 bounds")
    with zipfile.ZipFile(bundle_path, "r") as bundle:
        infos = bundle.infolist()
        names = [info.filename for info in infos]
        if tuple(names) != BUNDLE_MEMBERS or len(set(names)) != len(names):
            raise OtaReleaseError(
                "bundle must contain exactly the four canonical members in order"
            )
        if any(
            info.compress_type != zipfile.ZIP_STORED
            or info.file_size != info.compress_size
            for info in infos
        ):
            raise OtaReleaseError("bundle members must be stored without compression")
        info_by_name = {info.filename: info for info in infos}
        if not 8 <= info_by_name[BUNDLE_MANIFEST].file_size <= MAX_MANIFEST_SIZE:
            raise OtaReleaseError("manifest length is outside the v1 bounds")
        if info_by_name[BUNDLE_SIGNATURE].file_size != SIGNATURE_SIZE:
            raise OtaReleaseError("manifest signature has the wrong size")
        if not 0 < info_by_name[BUNDLE_FIRMWARE].file_size <= MAX_FIRMWARE_SIZE:
            raise OtaReleaseError("firmware does not fit the OTA application slot")
        if not 0 < info_by_name[BUNDLE_ELF].file_size <= MAX_ELF_SIZE:
            raise OtaReleaseError("ELF length is outside bounds")
        manifest_bytes = bundle.read(BUNDLE_MANIFEST)
        signature = bundle.read(BUNDLE_SIGNATURE)
        manifest = decode_manifest(manifest_bytes)
        verify_manifest_signature(manifest_bytes, signature, public_key)
        if expected_hardware is not None and manifest.hardware != expected_hardware:
            raise OtaReleaseError("bundle hardware does not match the expected model")
        if (
            current_release_counter is not None
            and manifest.release_counter <= current_release_counter
        ):
            raise OtaReleaseError("bundle is not newer than the confirmed release")

        firmware_info = info_by_name[BUNDLE_FIRMWARE]
        if firmware_info.file_size != manifest.firmware_size:
            raise OtaReleaseError("firmware length does not match the manifest")
        firmware = bundle.read(firmware_info)
        if hashlib.sha256(firmware).digest() != manifest.firmware_sha256:
            raise OtaReleaseError("firmware SHA-256 does not match the manifest")

        elf_info = info_by_name[BUNDLE_ELF]
        if elf_info.file_size != manifest.elf_size or elf_info.file_size > MAX_ELF_SIZE:
            raise OtaReleaseError("ELF length does not match the manifest")
        elf = bundle.read(elf_info)
        if hashlib.sha256(elf).digest() != manifest.elf_sha256:
            raise OtaReleaseError("ELF SHA-256 does not match the manifest")
        _require_manifest_artifact_identity(manifest, firmware, elf)
    return manifest


def create_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subcommands = parser.add_subparsers(dest="command", required=True)

    create = subcommands.add_parser("create", help="create and sign one release bundle")
    create.add_argument("--firmware-bin", type=Path, required=True)
    create.add_argument("--firmware-elf", type=Path, required=True)
    create.add_argument("--private-key", type=Path, required=True)
    create.add_argument("--private-key-pass-env")
    create.add_argument("--output", type=Path, required=True)
    create.add_argument("--hardware", required=True)
    create.add_argument("--firmware-version", required=True)
    create.add_argument("--release-counter", type=int, required=True)
    create.add_argument("--build-id", required=True)
    create.add_argument("--signing-key-id", required=True)
    create.add_argument("--overwrite", action="store_true")

    verify = subcommands.add_parser("verify", help="verify a signed release bundle")
    verify.add_argument("--bundle", type=Path, required=True)
    verify.add_argument("--public-key", type=Path, required=True)
    verify.add_argument("--expected-hardware")
    verify.add_argument("--current-release-counter", type=int)

    public_key = subcommands.add_parser(
        "public-key", help="print the uncompressed P-256 public point as hex"
    )
    public_key.add_argument("--private-key", type=Path, required=True)
    public_key.add_argument("--private-key-pass-env")

    trust_header = subcommands.add_parser(
        "trust-header",
        help="generate the firmware's ignored public-key trust header",
    )
    trust_header.add_argument(
        "--key",
        action="append",
        required=True,
        metavar="KEY_ID=PUBLIC_PEM",
        help="trusted key entry; repeat once for a next rotation key",
    )
    trust_header.add_argument(
        "--output", type=Path, default=Path("src/ota_trust_keys.generated.h")
    )
    trust_header.add_argument("--overwrite", action="store_true")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    arguments = create_argument_parser().parse_args(argv)
    try:
        if arguments.command == "create":
            manifest = create_bundle(
                firmware_bin=arguments.firmware_bin,
                firmware_elf=arguments.firmware_elf,
                private_key=arguments.private_key,
                output=arguments.output,
                hardware=arguments.hardware,
                firmware_version=arguments.firmware_version,
                release_counter=arguments.release_counter,
                build_id=arguments.build_id,
                signing_key_id=arguments.signing_key_id,
                private_key_password_environment=arguments.private_key_pass_env,
                overwrite=arguments.overwrite,
            )
            print(
                f"created {arguments.output} release={manifest.release_counter} "
                f"firmware_sha256={manifest.firmware_sha256.hex()}"
            )
            return 0
        if arguments.command == "verify":
            manifest = verify_bundle(
                bundle_path=arguments.bundle,
                public_key=arguments.public_key,
                expected_hardware=arguments.expected_hardware,
                current_release_counter=arguments.current_release_counter,
            )
            print(
                f"verified release={manifest.release_counter} "
                f"version={manifest.firmware_version} key={manifest.signing_key_id}"
            )
            return 0
        if arguments.command == "public-key":
            public_point = public_point_from_private_key(
                arguments.private_key, arguments.private_key_pass_env
            )
            print(public_point.hex())
            return 0
        parsed_keys: list[tuple[str, Path]] = []
        for value in arguments.key:
            key_id, separator, path = value.partition("=")
            if not separator or not path:
                raise OtaReleaseError("--key must use KEY_ID=PUBLIC_PEM")
            parsed_keys.append((key_id, Path(path)))
        write_trust_header(
            keys=parsed_keys,
            output=arguments.output,
            overwrite=arguments.overwrite,
        )
        print(f"wrote {arguments.output} with {len(parsed_keys)} public trust key(s)")
        return 0
    except (OSError, OtaReleaseError, zipfile.BadZipFile) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
