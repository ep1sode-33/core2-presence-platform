from __future__ import annotations

import base64
import hashlib
import importlib.util
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from presence_api.config import Settings
from presence_api.main import create_app
from presence_api.ota_bundle import ReleaseBundleError, verify_release_bundle

DEVICE = "core2-2cbcbb81eb60"
OTA_TOOL_PATH = Path(__file__).resolve().parents[2] / "tools" / "ota_release.py"
_OTA_SPEC = importlib.util.spec_from_file_location(
    "test_ota_release_tool", OTA_TOOL_PATH
)
assert _OTA_SPEC is not None and _OTA_SPEC.loader is not None
ota_release = importlib.util.module_from_spec(_OTA_SPEC)
sys.modules[_OTA_SPEC.name] = ota_release
_OTA_SPEC.loader.exec_module(ota_release)


@pytest.fixture
def release_fixture(tmp_path: Path) -> dict:
    if shutil.which("openssl") is None:
        pytest.skip("OpenSSL is required")
    private_key = tmp_path / "private.pem"
    public_key = tmp_path / "public.pem"
    subprocess.run(
        (
            "openssl",
            "ecparam",
            "-name",
            "prime256v1",
            "-genkey",
            "-noout",
            "-out",
            str(private_key),
        ),
        check=True,
        capture_output=True,
    )
    subprocess.run(
        (
            "openssl",
            "pkey",
            "-in",
            str(private_key),
            "-pubout",
            "-out",
            str(public_key),
        ),
        check=True,
        capture_output=True,
    )
    firmware = tmp_path / "firmware.bin"
    elf = tmp_path / "firmware.elf"
    identity = ota_release.ArtifactIdentity(
        hardware="m5go-classic-esp32-16m",
        firmware_version="0.7.0",
        build_id="abcdef0123456789",
    )
    marker = ota_release.encode_artifact_identity_marker(identity)
    firmware.write_bytes(b"firmware-v07\0" * 250 + marker + b"app-tail")
    elf.write_bytes(b"\x7fELF-debug-v07\0" * 250 + marker + b"elf-tail")
    bundle = tmp_path / "release.ota.zip"
    ota_release.create_bundle(
        firmware_bin=firmware,
        firmware_elf=elf,
        private_key=private_key,
        output=bundle,
        hardware="m5go-classic-esp32-16m",
        firmware_version="0.7.0",
        release_counter=7,
        build_id="abcdef0123456789",
        signing_key_id="release-2026-a",
    )
    return {
        "private_key": private_key,
        "public_key": public_key,
        "firmware": firmware.read_bytes(),
        "elf": elf.read_bytes(),
        "bundle": bundle.read_bytes(),
    }


def test_verified_bundle_can_be_selected_and_downloaded_by_target_device(
    tmp_path: Path, release_fixture: dict
) -> None:
    app = create_app(
        Settings(
            database_path=tmp_path / "release.db",
            api_token="device-secret",
            ota_trusted_keys=(("release-2026-a", release_fixture["public_key"]),),
        )
    )
    request = {
        "bundle_base64": base64.b64encode(release_fixture["bundle"]).decode(),
        "imported_by": "release-test",
    }
    with TestClient(
        app,
        client=("192.168.0.42", 50_000),
        headers={
            "Host": "192.168.0.46",
            "Origin": "http://192.168.0.46",
        },
    ) as client:
        imported = client.post("/v1/console/releases/import", json=request)
        duplicate = client.post("/v1/console/releases/import", json=request)
        assert imported.status_code == 200, imported.text
        assert duplicate.status_code == 200
        release_id = imported.json()["release_id"]
        assert imported.json()["verified"] is True
        assert imported.json()["release_counter"] == 7
        assert client.get("/v1/console/releases").json()[0]["release_id"] == release_id

        selected = client.put(
            f"/v1/console/devices/{DEVICE}/release",
            json={"release_id": release_id, "selected_by": "release-test"},
        )
        assert selected.status_code == 200, selected.text

        assert client.get(f"/v1/devices/{DEVICE}/control").status_code == 401
        headers = {"Authorization": "Bearer device-secret"}
        control = client.get(f"/v1/devices/{DEVICE}/control", headers=headers)
        assert control.status_code == 200
        desired = control.json()["desired_release"]
        assert desired["release_id"] == release_id
        assert desired["signature_format"] == "ecdsa-p256-sha256-raw"
        assert "elf_size" not in desired
        assert desired["manifest_url"].endswith("/manifest")
        assert desired["image_url"].endswith("/image")

        manifest_response = client.get(desired["manifest_url"], headers=headers)
        image_response = client.get(desired["image_url"], headers=headers)
        assert manifest_response.status_code == 200
        assert manifest_response.headers["content-type"] == "application/octet-stream"
        manifest_size = int.from_bytes(manifest_response.content[6:8], "big")
        assert len(manifest_response.content) == manifest_size + 64
        assert client.get(desired["manifest_url"]).status_code == 401
        assert image_response.headers["content-type"] == "application/octet-stream"
        assert image_response.content == release_fixture["firmware"]
        assert client.get(desired["image_url"]).status_code == 401

        missing_running_identity = client.post(
            f"/v1/devices/{DEVICE}/control/release-status",
            headers=headers,
            json={
                "schema_version": 1,
                "status_id": "status-running-incomplete-0001",
                "desired_release_id": release_id,
                "running_release_id": None,
                "previous_release_id": None,
                "last_known_good_release_id": release_id,
                "phase": "running",
                "progress_percent": 100,
                "last_error": None,
                "rollback_outcome": "not_needed",
                "firmware_version": "0.7.0",
                "build_id": "abcdef0123456789",
            },
        )
        assert missing_running_identity.status_code == 409

        mismatched_running_identity = client.post(
            f"/v1/devices/{DEVICE}/control/release-status",
            headers=headers,
            json={
                "schema_version": 1,
                "status_id": "status-running-mismatch-0001",
                "desired_release_id": release_id,
                "running_release_id": release_id,
                "previous_release_id": None,
                "last_known_good_release_id": release_id,
                "phase": "running",
                "progress_percent": 100,
                "last_error": None,
                "rollback_outcome": "not_needed",
                "firmware_version": "0.7.0",
                "build_id": "different-build-id",
            },
        )
        assert mismatched_running_identity.status_code == 409

        incomplete_payload = {
            "schema_version": 1,
            "status_id": "status-downloading-incomplete-0001",
            "desired_release_id": release_id,
            "running_release_id": None,
            "previous_release_id": None,
            "last_known_good_release_id": None,
            "phase": "downloading",
            "progress_percent": 50,
            "last_error": None,
            "rollback_outcome": "none",
            "firmware_version": "0.7.0",
            "build_id": "abcdef0123456789",
        }
        incomplete = client.post(
            f"/v1/devices/{DEVICE}/control/release-status",
            headers=headers,
            json=incomplete_payload,
        )
        assert incomplete.status_code == 200, incomplete.text
        assert incomplete.json()["desired_release_completed"] is False
        assert (
            client.get(f"/v1/devices/{DEVICE}/control", headers=headers).json()[
                "desired_release"
            ]["release_id"]
            == release_id
        )

        reported = client.post(
            f"/v1/devices/{DEVICE}/control/release-status",
            headers=headers,
            json={
                "schema_version": 1,
                "status_id": "status-running-0001",
                "desired_release_id": release_id,
                "running_release_id": release_id,
                "previous_release_id": None,
                "last_known_good_release_id": release_id,
                "phase": "running",
                "progress_percent": 100,
                "last_error": None,
                "rollback_outcome": "not_needed",
                "firmware_version": "0.7.0",
                "build_id": "abcdef0123456789",
            },
        )
        assert reported.status_code == 200, reported.text
        assert reported.json()["desired_release_completed"] is True
        assert (
            client.get(f"/v1/devices/{DEVICE}/control", headers=headers).json()[
                "desired_release"
            ]
            is None
        )
        overview = client.get(f"/v1/console/devices/{DEVICE}/release-status").json()
        assert overview["latest_status"]["running_release_id"] == release_id
        assert overview["target"]["completed_at_ms"] is not None

        # The response for an idempotent retry belongs to this exact report;
        # completing a later status must not mutate the earlier receipt.
        duplicate_incomplete = client.post(
            f"/v1/devices/{DEVICE}/control/release-status",
            headers=headers,
            json=incomplete_payload,
        )
        assert duplicate_incomplete.status_code == 200
        assert duplicate_incomplete.json()["duplicate"] is True
        assert duplicate_incomplete.json()["desired_release_completed"] is False

        idle = client.post(
            f"/v1/devices/{DEVICE}/control/release-status",
            headers=headers,
            json={
                "schema_version": 1,
                "status_id": "status-idle-after-confirmed-0001",
                "desired_release_id": release_id,
                "running_release_id": None,
                "previous_release_id": None,
                "last_known_good_release_id": None,
                "phase": "idle",
                "progress_percent": None,
                "last_error": None,
                "rollback_outcome": "none",
                "firmware_version": "0.7.0",
                "build_id": "abcdef0123456789",
            },
        )
        assert idle.status_code == 200, idle.text

        # Even though the newest report has no installed-release IDs, the
        # device's confirmed monotonic counter prevents wireless re-selection.
        downgrade = client.put(
            f"/v1/console/devices/{DEVICE}/release",
            json={"release_id": release_id, "selected_by": "release-test"},
        )
        assert downgrade.status_code == 409

        other = "core2-000000000000"
        forbidden_by_target = client.get(
            desired["image_url"].replace(DEVICE, other), headers=headers
        )
        assert forbidden_by_target.status_code == 404


def test_bundle_import_uses_backend_trust_set_not_request_material(
    tmp_path: Path, release_fixture: dict
) -> None:
    app = create_app(
        Settings(
            database_path=tmp_path / "untrusted.db",
            api_token=None,
            ota_trusted_keys=(("next-key", release_fixture["public_key"]),),
        )
    )
    with TestClient(
        app,
        client=("192.168.0.42", 50_000),
        headers={
            "Host": "192.168.0.46",
            "Origin": "http://192.168.0.46",
        },
    ) as client:
        response = client.post(
            "/v1/console/releases/import",
            json={
                "bundle_base64": base64.b64encode(release_fixture["bundle"]).decode(),
                "imported_by": "release-test",
            },
        )
    assert response.status_code == 400
    assert "trust set" in response.json()["detail"]


def test_bundle_import_rejects_noncanonical_archive(
    tmp_path: Path, release_fixture: dict
) -> None:
    source = tmp_path / "source.zip"
    source.write_bytes(release_fixture["bundle"])
    bad = tmp_path / "compressed.zip"
    with (
        zipfile.ZipFile(source, "r") as original,
        zipfile.ZipFile(bad, "w", compression=zipfile.ZIP_DEFLATED) as rewritten,
    ):
        for name in original.namelist():
            rewritten.writestr(name, original.read(name))

    app = create_app(
        Settings(
            database_path=tmp_path / "bad.db",
            api_token=None,
            ota_trusted_keys=(("release-2026-a", release_fixture["public_key"]),),
        )
    )
    with TestClient(
        app,
        client=("192.168.0.42", 50_000),
        headers={
            "Host": "192.168.0.46",
            "Origin": "http://192.168.0.46",
        },
    ) as client:
        response = client.post(
            "/v1/console/releases/import",
            json={"bundle_base64": base64.b64encode(bad.read_bytes()).decode()},
        )
    assert response.status_code == 400
    assert "without compression" in response.json()["detail"]


def test_bundle_import_rejects_operator_text_that_firmware_cannot_parse(
    tmp_path: Path, release_fixture: dict
) -> None:
    app = create_app(
        Settings(
            database_path=tmp_path / "operator.db",
            api_token=None,
            ota_trusted_keys=(("release-2026-a", release_fixture["public_key"]),),
        )
    )
    with TestClient(
        app,
        client=("192.168.0.42", 50_000),
        headers={
            "Host": "192.168.0.46",
            "Origin": "http://192.168.0.46",
        },
    ) as client:
        response = client.post(
            "/v1/console/releases/import",
            json={
                "bundle_base64": base64.b64encode(release_fixture["bundle"]).decode(),
                "imported_by": 'operator\\"with-escape',
            },
        )
    assert response.status_code == 422


def test_backend_rejects_cryptographically_valid_artifact_identity_failures(
    tmp_path: Path, release_fixture: dict
) -> None:
    identity = ota_release.ArtifactIdentity(
        hardware="m5go-classic-esp32-16m",
        firmware_version="0.7.0",
        build_id="abcdef0123456789",
    )
    marker = ota_release.encode_artifact_identity_marker(identity)
    other_marker = ota_release.encode_artifact_identity_marker(
        ota_release.ArtifactIdentity(
            hardware=identity.hardware,
            firmware_version=identity.firmware_version,
            build_id="other-build",
        )
    )

    def signed_bundle(
        firmware: bytes, elf: bytes, manifest_build_id: str = identity.build_id
    ) -> str:
        manifest = ota_release.OtaManifest(
            hardware=identity.hardware,
            firmware_version=identity.firmware_version,
            release_counter=8,
            build_id=manifest_build_id,
            signing_key_id="release-2026-a",
            firmware_size=len(firmware),
            firmware_sha256=hashlib.sha256(firmware).digest(),
            elf_size=len(elf),
            elf_sha256=hashlib.sha256(elf).digest(),
        )
        encoded = ota_release.encode_manifest(manifest)
        signature = ota_release.sign_manifest(encoded, release_fixture["private_key"])
        path = tmp_path / f"invalid-identity-{len(list(tmp_path.glob('*.zip')))}.zip"
        with zipfile.ZipFile(path, "w") as bundle:
            bundle.writestr(ota_release._zip_info("firmware.bin"), firmware)
            bundle.writestr(ota_release._zip_info("firmware.elf"), elf)
            bundle.writestr(ota_release._zip_info("manifest.bin"), encoded)
            bundle.writestr(ota_release._zip_info("manifest.sig"), signature)
        return base64.b64encode(path.read_bytes()).decode()

    firmware = release_fixture["firmware"]
    elf = release_fixture["elf"]
    cases = (
        signed_bundle(b"firmware-without-marker", elf),
        signed_bundle(firmware, elf.replace(marker, other_marker)),
        signed_bundle(firmware + marker, elf),
        signed_bundle(firmware, elf, "wrong-manifest-build"),
    )
    for encoded_bundle in cases:
        with pytest.raises(ReleaseBundleError):
            verify_release_bundle(
                encoded_bundle,
                {"release-2026-a": release_fixture["public_key"]},
            )
