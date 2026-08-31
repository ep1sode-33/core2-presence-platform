from __future__ import annotations

import hashlib
import shutil
import subprocess
import tempfile
import unittest
import zipfile
from pathlib import Path

from tools import ota_release


class ManifestTests(unittest.TestCase):
    def manifest(self) -> ota_release.OtaManifest:
        return ota_release.OtaManifest(
            hardware="m5go-classic-esp32-16m",
            firmware_version="0.7.0-rc.1",
            release_counter=7,
            build_id="0123456789abcdef",
            signing_key_id="release-2026-a",
            firmware_size=123456,
            firmware_sha256=bytes(range(32)),
            elf_size=654321,
            elf_sha256=bytes(reversed(range(32))),
        )

    def test_manifest_encoding_is_deterministic_and_round_trips(self) -> None:
        first = ota_release.encode_manifest(self.manifest())
        second = ota_release.encode_manifest(self.manifest())
        self.assertEqual(first, second)
        self.assertEqual(first[:8], b"M5OT\x01\x01" + len(first).to_bytes(2, "big"))
        self.assertEqual(ota_release.decode_manifest(first), self.manifest())

    def test_manifest_rejects_truncation_length_and_trailing_data(self) -> None:
        encoded = ota_release.encode_manifest(self.manifest())
        for broken in (
            encoded[:-1],
            encoded[:6] + b"\x00\x08" + encoded[8:],
            encoded + b"\x00",
        ):
            with (
                self.subTest(broken=broken[-8:]),
                self.assertRaises(ota_release.OtaReleaseError),
            ):
                ota_release.decode_manifest(broken)

    def test_manifest_rejects_noncanonical_or_empty_text(self) -> None:
        for hardware in ("", "m5go/classic", "m5go classic", "mégo"):
            with (
                self.subTest(hardware=hardware),
                self.assertRaises(ota_release.OtaReleaseError),
            ):
                ota_release.encode_manifest(
                    ota_release.OtaManifest(
                        **{**self.manifest().__dict__, "hardware": hardware}
                    )
                )

    def test_manifest_rejects_zero_release_and_wrong_digest_size(self) -> None:
        for release_counter in (0, 1 << 63):
            with (
                self.subTest(release_counter=release_counter),
                self.assertRaises(ota_release.OtaReleaseError),
            ):
                ota_release.encode_manifest(
                    ota_release.OtaManifest(
                        **{
                            **self.manifest().__dict__,
                            "release_counter": release_counter,
                        }
                    )
                )
        with self.assertRaises(ota_release.OtaReleaseError):
            ota_release.encode_manifest(
                ota_release.OtaManifest(
                    **{**self.manifest().__dict__, "firmware_sha256": b"short"}
                )
            )


@unittest.skipUnless(shutil.which("openssl"), "OpenSSL is required")
class SignatureAndBundleTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="m5go-ota-test-")
        self.root = Path(self.temporary.name)
        self.private_key = self.root / "private.pem"
        self.public_key = self.root / "public.pem"
        subprocess.run(
            (
                "openssl",
                "ecparam",
                "-name",
                "prime256v1",
                "-genkey",
                "-noout",
                "-out",
                str(self.private_key),
            ),
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        subprocess.run(
            (
                "openssl",
                "pkey",
                "-in",
                str(self.private_key),
                "-pubout",
                "-out",
                str(self.public_key),
            ),
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        self.firmware = self.root / "input.bin"
        self.elf = self.root / "input.elf"
        self.identity = ota_release.ArtifactIdentity(
            hardware="m5go-classic-esp32-16m",
            firmware_version="0.7.0",
            build_id="abcdef0123456789",
        )
        marker = ota_release.encode_artifact_identity_marker(self.identity)
        self.firmware.write_bytes(b"firmware-image\x00" * 127 + marker + b"app-tail")
        self.elf.write_bytes(b"\x7fELF-debug-symbols\x00" * 53 + marker + b"elf-tail")
        self.bundle = self.root / "release.ota.zip"

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def create_bundle(self) -> ota_release.OtaManifest:
        return ota_release.create_bundle(
            firmware_bin=self.firmware,
            firmware_elf=self.elf,
            private_key=self.private_key,
            output=self.bundle,
            hardware="m5go-classic-esp32-16m",
            firmware_version="0.7.0",
            release_counter=11,
            build_id="abcdef0123456789",
            signing_key_id="release-2026-a",
        )

    def write_signed_bundle(
        self, path: Path, firmware: bytes, elf: bytes
    ) -> ota_release.OtaManifest:
        manifest = ota_release.OtaManifest(
            hardware=self.identity.hardware,
            firmware_version=self.identity.firmware_version,
            release_counter=12,
            build_id=self.identity.build_id,
            signing_key_id="release-2026-a",
            firmware_size=len(firmware),
            firmware_sha256=hashlib.sha256(firmware).digest(),
            elf_size=len(elf),
            elf_sha256=hashlib.sha256(elf).digest(),
        )
        encoded = ota_release.encode_manifest(manifest)
        signature = ota_release.sign_manifest(encoded, self.private_key)
        with zipfile.ZipFile(path, "w") as bundle:
            bundle.writestr(ota_release._zip_info("firmware.bin"), firmware)
            bundle.writestr(ota_release._zip_info("firmware.elf"), elf)
            bundle.writestr(ota_release._zip_info("manifest.bin"), encoded)
            bundle.writestr(ota_release._zip_info("manifest.sig"), signature)
        return manifest

    def test_signature_is_fixed_raw_low_s_and_verifies(self) -> None:
        manifest = ota_release.encode_manifest(
            ota_release.OtaManifest(
                hardware="m5go-classic-esp32-16m",
                firmware_version="0.7.0",
                release_counter=1,
                build_id="abc123",
                signing_key_id="release-2026-a",
                firmware_size=1,
                firmware_sha256=hashlib.sha256(b"x").digest(),
                elf_size=1,
                elf_sha256=hashlib.sha256(b"y").digest(),
            )
        )
        signature = ota_release.sign_manifest(manifest, self.private_key)
        self.assertEqual(len(signature), 64)
        self.assertLessEqual(
            int.from_bytes(signature[32:], "big"), ota_release.P256_HALF_ORDER
        )
        ota_release.verify_manifest_signature(manifest, signature, self.public_key)

    def test_bundle_contains_exact_artifacts_and_verifies(self) -> None:
        expected = self.create_bundle()
        with zipfile.ZipFile(self.bundle, "r") as bundle:
            self.assertEqual(tuple(bundle.namelist()), ota_release.BUNDLE_MEMBERS)
            self.assertEqual(bundle.read("firmware.bin"), self.firmware.read_bytes())
            self.assertEqual(bundle.read("firmware.elf"), self.elf.read_bytes())
            self.assertEqual(len(bundle.read("manifest.sig")), 64)
        actual = ota_release.verify_bundle(
            bundle_path=self.bundle,
            public_key=self.public_key,
            expected_hardware="m5go-classic-esp32-16m",
            current_release_counter=10,
        )
        self.assertEqual(actual, expected)

    def test_identity_marker_is_stable_and_unambiguous(self) -> None:
        marker = ota_release.encode_artifact_identity_marker(self.identity)
        self.assertEqual(len(marker), ota_release.ARTIFACT_IDENTITY_SIZE)
        self.assertEqual(
            ota_release.extract_artifact_identity(
                b"prefix" + marker + b"suffix", "bin"
            ),
            self.identity,
        )
        for payload in (b"no marker", marker + marker):
            with (
                self.subTest(payload_length=len(payload)),
                self.assertRaises(ota_release.OtaReleaseError),
            ):
                ota_release.extract_artifact_identity(payload, "bin")

    def test_create_rejects_missing_mismatched_or_wrong_claimed_identity(self) -> None:
        valid_firmware = self.firmware.read_bytes()
        valid_elf = self.elf.read_bytes()
        other = ota_release.encode_artifact_identity_marker(
            ota_release.ArtifactIdentity(
                hardware=self.identity.hardware,
                firmware_version=self.identity.firmware_version,
                build_id="other-build",
            )
        )

        cases = (
            (b"firmware-without-marker", valid_elf, self.identity.build_id),
            (valid_firmware, b"\x7fELF" + other, self.identity.build_id),
            (valid_firmware + valid_firmware, valid_elf, self.identity.build_id),
            (valid_firmware, valid_elf, "wrong-manifest-build"),
        )
        for index, (firmware, elf, build_id) in enumerate(cases):
            self.firmware.write_bytes(firmware)
            self.elf.write_bytes(elf)
            with (
                self.subTest(index=index),
                self.assertRaises(ota_release.OtaReleaseError),
            ):
                ota_release.create_bundle(
                    firmware_bin=self.firmware,
                    firmware_elf=self.elf,
                    private_key=self.private_key,
                    output=self.root / f"rejected-{index}.zip",
                    hardware=self.identity.hardware,
                    firmware_version=self.identity.firmware_version,
                    release_counter=11,
                    build_id=build_id,
                    signing_key_id="release-2026-a",
                )

    def test_verify_rejects_signed_missing_mismatched_or_ambiguous_identity(
        self,
    ) -> None:
        marker = ota_release.encode_artifact_identity_marker(self.identity)
        other_marker = ota_release.encode_artifact_identity_marker(
            ota_release.ArtifactIdentity(
                hardware=self.identity.hardware,
                firmware_version=self.identity.firmware_version,
                build_id="other-build",
            )
        )
        valid_firmware = b"app" + marker + b"tail"
        valid_elf = b"\x7fELF" + marker + b"tail"
        cases = (
            (b"firmware-without-marker", valid_elf),
            (valid_firmware, b"\x7fELF" + other_marker),
            (valid_firmware + marker, valid_elf),
        )
        for index, (firmware, elf) in enumerate(cases):
            bundle = self.root / f"signed-invalid-identity-{index}.zip"
            self.write_signed_bundle(bundle, firmware, elf)
            with (
                self.subTest(index=index),
                self.assertRaises(ota_release.OtaReleaseError),
            ):
                ota_release.verify_bundle(
                    bundle_path=bundle,
                    public_key=self.public_key,
                    expected_hardware=self.identity.hardware,
                )

    def test_bundle_rejects_downgrade_wrong_model_and_existing_output(self) -> None:
        self.create_bundle()
        with self.assertRaises(ota_release.OtaReleaseError):
            self.create_bundle()
        with self.assertRaises(ota_release.OtaReleaseError):
            ota_release.verify_bundle(
                bundle_path=self.bundle,
                public_key=self.public_key,
                current_release_counter=11,
            )
        with self.assertRaises(ota_release.OtaReleaseError):
            ota_release.verify_bundle(
                bundle_path=self.bundle,
                public_key=self.public_key,
                expected_hardware="core2",
            )

    def test_bundle_rejects_tampered_firmware_and_signature(self) -> None:
        self.create_bundle()
        original: dict[str, bytes] = {}
        with zipfile.ZipFile(self.bundle, "r") as bundle:
            for name in ota_release.BUNDLE_MEMBERS:
                original[name] = bundle.read(name)

        for member in ("firmware.bin", "manifest.sig"):
            tampered = self.root / f"tampered-{member}.zip"
            with zipfile.ZipFile(tampered, "w") as bundle:
                for name in ota_release.BUNDLE_MEMBERS:
                    value = original[name]
                    if name == member:
                        value = bytes((value[0] ^ 1,)) + value[1:]
                    bundle.writestr(name, value)
            with (
                self.subTest(member=member),
                self.assertRaises(ota_release.OtaReleaseError),
            ):
                ota_release.verify_bundle(
                    bundle_path=tampered, public_key=self.public_key
                )

    def test_bundle_rejects_noncanonical_compression_and_counter_bounds(self) -> None:
        self.create_bundle()
        original: dict[str, bytes] = {}
        with zipfile.ZipFile(self.bundle, "r") as bundle:
            for name in ota_release.BUNDLE_MEMBERS:
                original[name] = bundle.read(name)

        compressed = self.root / "compressed.zip"
        with zipfile.ZipFile(
            compressed, "w", compression=zipfile.ZIP_DEFLATED
        ) as bundle:
            for name in ota_release.BUNDLE_MEMBERS:
                bundle.writestr(name, original[name])
        with self.assertRaises(ota_release.OtaReleaseError):
            ota_release.verify_bundle(
                bundle_path=compressed, public_key=self.public_key
            )
        with self.assertRaises(ota_release.OtaReleaseError):
            ota_release.verify_bundle(
                bundle_path=self.bundle,
                public_key=self.public_key,
                current_release_counter=1 << 63,
            )

    def test_non_p256_signing_key_is_rejected(self) -> None:
        p384 = self.root / "p384.pem"
        subprocess.run(
            (
                "openssl",
                "ecparam",
                "-name",
                "secp384r1",
                "-genkey",
                "-noout",
                "-out",
                str(p384),
            ),
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        with self.assertRaises(ota_release.OtaReleaseError):
            ota_release.sign_manifest(b"manifest", p384)

    def test_public_trust_header_contains_only_fixed_public_points(self) -> None:
        header = self.root / "ota_trust_keys.generated.h"
        ota_release.write_trust_header(
            keys=[("release-2026-a", self.public_key)], output=header
        )
        text = header.read_text(encoding="ascii")
        public_point = ota_release.public_point_from_public_key(self.public_key)
        self.assertIn('{"release-2026-a"', text)
        self.assertIn(f"0x{public_point[0]:02x}", text)
        self.assertEqual(text.count("0x"), 65)
        self.assertNotIn("PRIVATE", text)
        self.assertNotIn(self.private_key.read_text(encoding="ascii"), text)
        with self.assertRaises(ota_release.OtaReleaseError):
            ota_release.write_trust_header(
                keys=[("release-2026-a", self.public_key)], output=header
            )

    def test_public_trust_header_rejects_unsafe_or_duplicate_ids(self) -> None:
        for keys in (
            [("bad key", self.public_key)],
            [("same", self.public_key), ("same", self.public_key)],
            [],
        ):
            with (
                self.subTest(keys=keys),
                self.assertRaises(ota_release.OtaReleaseError),
            ):
                ota_release.write_trust_header(keys=keys, output=self.root / "trust.h")


if __name__ == "__main__":
    unittest.main()
