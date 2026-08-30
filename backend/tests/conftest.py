from __future__ import annotations

from collections.abc import Iterator
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from presence_api.config import Settings
from presence_api.main import create_app


@pytest.fixture
def client(tmp_path: Path) -> Iterator[TestClient]:
    app = create_app(Settings(database_path=tmp_path / "presence.db", api_token=None))
    with TestClient(app, client=("192.168.0.42", 50_000)) as test_client:
        yield test_client


@pytest.fixture
def sample_batch() -> dict:
    return {
        "schema_version": 1,
        "batch_id": "boot000000000001:0-1",
        "boot_id": "boot000000000001",
        "firmware_version": "0.1.0",
        "applied_config_revision": 0,
        "records": [
            {
                "seq": 0,
                "kind": "sample",
                "uptime_ms": 1000,
                "pir": True,
                "mic_rms": 500.0,
                "mic_envelope": 550.0,
                "mic_min": -1200,
                "mic_max": 1300,
                "noise_floor": 400.0,
                "sound_threshold": 750.0,
                "sound_active": False,
                "state": "present",
                "brightness": 255,
            },
            {
                "seq": 1,
                "kind": "transition",
                "uptime_ms": 1020,
                "from_state": "idle",
                "to_state": "present",
                "reason": "pir_motion",
            },
        ],
    }
