from __future__ import annotations

import base64
import binascii
import hashlib
import io
import os
import subprocess
import tempfile
import zipfile
from dataclasses import dataclass
from pathlib import Path

MANIFEST_MAGIC = b"M5OT"
MANIFEST_FORMAT_VERSION = 1
SIGNATURE_FORMAT_VERSION = 1
SIGNATURE_SIZE = 64
MAX_MANIFEST_SIZE = 320
MAX_FIRMWARE_SIZE = 0x640000
MAX_ELF_SIZE = 64 * 1024 * 1024
MAX_BUNDLE_SIZE = MAX_FIRMWARE_SIZE + MAX_ELF_SIZE + 16 * 1024
MAX_SIGNED_SQLITE_INTEGER = (1 << 63) - 1
M5GO_HARDWARE_MODEL = "m5go-classic-esp32-16m"
P256_ORDER = int("FFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551", 16)
P256_HALF_ORDER = P256_ORDER // 2
EC_PUBLIC_KEY_OID = bytes.fromhex("06072a8648ce3d0201")
P256_CURVE_OID = bytes.fromhex("06082a8648ce3d030107")
BUNDLE_MEMBERS = (
    "firmware.bin",
    "firmware.elf",
    "manifest.bin",
    "manifest.sig",
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


class ReleaseBundleError(ValueError):
    pass


@dataclass(frozen=True)
class VerifiedReleaseBundle:
    release_id: str
    hardware: str
    firmware_version: str
    release_counter: int
    build_id: str
    signing_key_id: str
    firmware_size: int
    firmware_sha256: str
    elf_size: int
    elf_sha256: str
    manifest: bytes
    signature: bytes
    firmware: bytes
    elf: bytes


@dataclass(frozen=True)
class _ArtifactIdentity:
    hardware: str
    firmware_version: str
    build_id: str


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
        raise ReleaseBundleError(f"artifact {name} length is outside bounds")
    if any(field[length:]):
        raise ReleaseBundleError(f"artifact {name} padding is not canonical")
    value = field[:length]
    try:
        decoded = value.decode("ascii")
    except UnicodeDecodeError as error:
        raise ReleaseBundleError(f"artifact {name} must be ASCII") from error
    if any(
        not (
            byte in b"._+-"
            or ord("0") <= byte <= ord("9")
            or ord("A") <= byte <= ord("Z")
            or ord("a") <= byte <= ord("z")
        )
        for byte in value
    ):
        raise ReleaseBundleError(f"artifact {name} is not canonical")
    return decoded, end


def _extract_artifact_identity(data: bytes, artifact_name: str) -> _ArtifactIdentity:
    marker_count = data.count(ARTIFACT_IDENTITY_MAGIC)
    if marker_count == 0:
        raise ReleaseBundleError(
            f"{artifact_name} is missing the firmware identity marker"
        )
    if marker_count != 1:
        raise ReleaseBundleError(
            f"{artifact_name} contains ambiguous firmware identity markers"
        )
    start = data.index(ARTIFACT_IDENTITY_MAGIC)
    end = start + ARTIFACT_IDENTITY_SIZE
    if end > len(data):
        raise ReleaseBundleError(
            f"{artifact_name} firmware identity marker is truncated"
        )
    record = data[start:end]
    if record[-len(ARTIFACT_IDENTITY_TRAILER) :] != ARTIFACT_IDENTITY_TRAILER:
        raise ReleaseBundleError(
            f"{artifact_name} firmware identity trailer is invalid"
        )
    position = len(ARTIFACT_IDENTITY_MAGIC)
    format_version, hardware_length, version_length, build_id_length = record[
        position : position + 4
    ]
    position += 4
    if format_version != ARTIFACT_IDENTITY_FORMAT_VERSION:
        raise ReleaseBundleError(
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
    return _ArtifactIdentity(hardware, firmware_version, build_id)


def _require_manifest_artifact_identity(
    decoded: dict[str, object], firmware: bytes, elf: bytes
) -> None:
    firmware_identity = _extract_artifact_identity(firmware, "firmware.bin")
    elf_identity = _extract_artifact_identity(elf, "firmware.elf")
    if firmware_identity != elf_identity:
        raise ReleaseBundleError("firmware BIN and ELF identity markers do not match")
    if firmware_identity.hardware != M5GO_HARDWARE_MODEL:
        raise ReleaseBundleError(
            f"firmware artifact hardware must be {M5GO_HARDWARE_MODEL}"
        )
    expected = _ArtifactIdentity(
        hardware=str(decoded["hardware"]),
        firmware_version=str(decoded["firmware_version"]),
        build_id=str(decoded["build_id"]),
    )
    if firmware_identity != expected:
        raise ReleaseBundleError(
            "firmware artifact identity does not match the signed manifest"
        )


class _Reader:
    def __init__(self, data: bytes) -> None:
        self.data = data
        self.position = 8

    def exact(self, size: int) -> bytes:
        if size < 0 or self.position + size > len(self.data):
            raise ReleaseBundleError("manifest is truncated")
        result = self.data[self.position : self.position + size]
        self.position += size
        return result

    def integer(self, size: int) -> int:
        return int.from_bytes(self.exact(size), "big")

    def text(self, name: str, maximum: int) -> str:
        size = self.integer(2)
        value = self.exact(size)
        try:
            decoded = value.decode("ascii")
        except UnicodeDecodeError as error:
            raise ReleaseBundleError(f"{name} must be ASCII") from error
        if not decoded or len(value) > maximum:
            raise ReleaseBundleError(f"{name} length is outside bounds")
        if any(
            not (
                byte in b"._+-"
                or ord("0") <= byte <= ord("9")
                or ord("A") <= byte <= ord("Z")
                or ord("a") <= byte <= ord("z")
            )
            for byte in value
        ):
            raise ReleaseBundleError(f"{name} is not canonically encoded")
        return decoded

    def digest(self, name: str) -> bytes:
        size = self.integer(2)
        if size != 32:
            raise ReleaseBundleError(f"{name} has the wrong length")
        return self.exact(size)


def _read_der_length(data: bytes, position: int) -> tuple[int, int]:
    if position >= len(data):
        raise ReleaseBundleError("truncated DER length")
    first = data[position]
    position += 1
    if first < 0x80:
        return first, position
    octets = first & 0x7F
    if octets == 0 or octets > 4 or position + octets > len(data):
        raise ReleaseBundleError("invalid DER length")
    if data[position] == 0:
        raise ReleaseBundleError("non-canonical DER length")
    length = int.from_bytes(data[position : position + octets], "big")
    if length < 0x80:
        raise ReleaseBundleError("non-canonical DER length")
    return length, position + octets


def _read_der_tlv(data: bytes, position: int, expected_tag: int) -> tuple[bytes, int]:
    if position >= len(data) or data[position] != expected_tag:
        raise ReleaseBundleError("unexpected DER tag")
    length, value_position = _read_der_length(data, position + 1)
    end = value_position + length
    if end > len(data):
        raise ReleaseBundleError("truncated DER value")
    return data[value_position:end], end


def _encode_der_length(length: int) -> bytes:
    if length < 0x80:
        return bytes((length,))
    encoded = length.to_bytes((length.bit_length() + 7) // 8, "big")
    return bytes((0x80 | len(encoded),)) + encoded


def _encode_der_integer(value: int) -> bytes:
    encoded = value.to_bytes((value.bit_length() + 7) // 8, "big")
    if encoded[0] & 0x80:
        encoded = b"\x00" + encoded
    return b"\x02" + _encode_der_length(len(encoded)) + encoded


def _signature_to_der(signature: bytes) -> bytes:
    if len(signature) != SIGNATURE_SIZE:
        raise ReleaseBundleError("manifest signature must be exactly 64 bytes")
    r = int.from_bytes(signature[:32], "big")
    s = int.from_bytes(signature[32:], "big")
    if not 0 < r < P256_ORDER or not 0 < s <= P256_HALF_ORDER:
        raise ReleaseBundleError("manifest signature is not canonical P-256 low-S")
    body = _encode_der_integer(r) + _encode_der_integer(s)
    return b"\x30" + _encode_der_length(len(body)) + body


def _require_p256_public_key(public_key: Path) -> None:
    try:
        completed = subprocess.run(
            (
                "openssl",
                "pkey",
                "-pubin",
                "-in",
                os.fspath(public_key),
                "-pubout",
                "-outform",
                "DER",
            ),
            check=False,
            capture_output=True,
            timeout=10,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired) as error:
        raise ReleaseBundleError("OpenSSL public-key validation failed") from error
    if completed.returncode != 0:
        raise ReleaseBundleError("trusted OTA public key is invalid")
    sequence, end = _read_der_tlv(completed.stdout, 0, 0x30)
    if end != len(completed.stdout):
        raise ReleaseBundleError("trusted OTA public key has trailing data")
    algorithm, position = _read_der_tlv(sequence, 0, 0x30)
    point, position = _read_der_tlv(sequence, position, 0x03)
    if (
        algorithm != EC_PUBLIC_KEY_OID + P256_CURVE_OID
        or position != len(sequence)
        or len(point) != 66
        or point[:2] != b"\x00\x04"
    ):
        raise ReleaseBundleError("trusted OTA key must be uncompressed P-256")


def _verify_signature(manifest: bytes, signature: bytes, public_key: Path) -> None:
    _require_p256_public_key(public_key)
    der_signature = _signature_to_der(signature)
    with tempfile.TemporaryDirectory(prefix="m5go-backend-verify-") as directory:
        root = Path(directory)
        manifest_path = root / "manifest.bin"
        signature_path = root / "manifest.der"
        manifest_path.write_bytes(manifest)
        signature_path.write_bytes(der_signature)
        try:
            completed = subprocess.run(
                (
                    "openssl",
                    "dgst",
                    "-sha256",
                    "-verify",
                    os.fspath(public_key),
                    "-signature",
                    os.fspath(signature_path),
                    os.fspath(manifest_path),
                ),
                check=False,
                capture_output=True,
                timeout=10,
            )
        except (FileNotFoundError, subprocess.TimeoutExpired) as error:
            raise ReleaseBundleError("OpenSSL signature verification failed") from error
    if completed.returncode != 0:
        raise ReleaseBundleError("OTA manifest signature is invalid")


def _decode_manifest(data: bytes) -> dict[str, object]:
    if not 8 <= len(data) <= MAX_MANIFEST_SIZE:
        raise ReleaseBundleError("manifest length is outside bounds")
    if data[:4] != MANIFEST_MAGIC:
        raise ReleaseBundleError("manifest magic is invalid")
    if data[4] != MANIFEST_FORMAT_VERSION or data[5] != SIGNATURE_FORMAT_VERSION:
        raise ReleaseBundleError("manifest version is unsupported")
    if int.from_bytes(data[6:8], "big") != len(data):
        raise ReleaseBundleError("manifest total length does not match")
    reader = _Reader(data)
    result: dict[str, object] = {
        "hardware": reader.text("hardware", 48),
        "firmware_version": reader.text("firmware_version", 32),
        "release_counter": reader.integer(8),
        "build_id": reader.text("build_id", 64),
        "signing_key_id": reader.text("signing_key_id", 32),
        "firmware_size": reader.integer(4),
        "firmware_sha256": reader.digest("firmware_sha256"),
        "elf_size": reader.integer(4),
        "elf_sha256": reader.digest("elf_sha256"),
    }
    if reader.position != len(data):
        raise ReleaseBundleError("manifest has trailing data")
    if not 0 < result["release_counter"] <= MAX_SIGNED_SQLITE_INTEGER:
        raise ReleaseBundleError("release counter is outside backend bounds")
    if not 0 < result["firmware_size"] <= MAX_FIRMWARE_SIZE:
        raise ReleaseBundleError("firmware does not fit the application slot")
    if not 0 < result["elf_size"] <= MAX_ELF_SIZE:
        raise ReleaseBundleError("ELF size is outside bounds")
    return result


def verify_release_bundle(
    encoded_bundle: str,
    trusted_keys: dict[str, Path],
) -> VerifiedReleaseBundle:
    try:
        bundle_bytes = base64.b64decode(encoded_bundle, validate=True)
    except (binascii.Error, ValueError) as error:
        raise ReleaseBundleError("bundle_base64 is invalid") from error
    if not bundle_bytes or len(bundle_bytes) > MAX_BUNDLE_SIZE:
        raise ReleaseBundleError("release bundle size is outside bounds")
    try:
        archive = zipfile.ZipFile(io.BytesIO(bundle_bytes), "r")
    except zipfile.BadZipFile as error:
        raise ReleaseBundleError("release bundle is not a valid ZIP") from error
    with archive:
        if tuple(archive.namelist()) != BUNDLE_MEMBERS:
            raise ReleaseBundleError(
                "bundle must contain exactly the canonical members in order"
            )
        infos = {info.filename: info for info in archive.infolist()}
        if any(
            info.compress_type != zipfile.ZIP_STORED
            or info.file_size != info.compress_size
            for info in infos.values()
        ):
            raise ReleaseBundleError(
                "bundle members must be stored without compression"
            )
        if infos["manifest.bin"].file_size > MAX_MANIFEST_SIZE:
            raise ReleaseBundleError("manifest exceeds size limit")
        if infos["manifest.sig"].file_size != SIGNATURE_SIZE:
            raise ReleaseBundleError("manifest signature has the wrong size")
        if infos["firmware.bin"].file_size > MAX_FIRMWARE_SIZE:
            raise ReleaseBundleError("firmware does not fit the application slot")
        if infos["firmware.elf"].file_size > MAX_ELF_SIZE:
            raise ReleaseBundleError("ELF exceeds size limit")
        manifest = archive.read("manifest.bin")
        signature = archive.read("manifest.sig")
        firmware = archive.read("firmware.bin")
        elf = archive.read("firmware.elf")

    decoded = _decode_manifest(manifest)
    key_id = str(decoded["signing_key_id"])
    public_key = trusted_keys.get(key_id)
    if public_key is None:
        raise ReleaseBundleError("manifest signing key is not in the backend trust set")
    _verify_signature(manifest, signature, public_key)
    firmware_digest = hashlib.sha256(firmware).digest()
    elf_digest = hashlib.sha256(elf).digest()
    if (
        len(firmware) != decoded["firmware_size"]
        or firmware_digest != decoded["firmware_sha256"]
    ):
        raise ReleaseBundleError("firmware does not match the signed manifest")
    if len(elf) != decoded["elf_size"] or elf_digest != decoded["elf_sha256"]:
        raise ReleaseBundleError("ELF does not match the signed manifest")
    _require_manifest_artifact_identity(decoded, firmware, elf)
    release_id = "rel-" + hashlib.sha256(manifest + signature).hexdigest()[:32]
    return VerifiedReleaseBundle(
        release_id=release_id,
        hardware=str(decoded["hardware"]),
        firmware_version=str(decoded["firmware_version"]),
        release_counter=int(decoded["release_counter"]),
        build_id=str(decoded["build_id"]),
        signing_key_id=key_id,
        firmware_size=len(firmware),
        firmware_sha256=firmware_digest.hex(),
        elf_size=len(elf),
        elf_sha256=elf_digest.hex(),
        manifest=manifest,
        signature=signature,
        firmware=firmware,
        elf=elf,
    )
