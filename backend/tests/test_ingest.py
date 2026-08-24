from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from copy import deepcopy
from pathlib import Path
from threading import Barrier

from fastapi.testclient import TestClient

from presence_api.database import Database
from presence_api.schemas import TelemetryBatch
from presence_api.service import PresenceService

DEVICE = "core2-2cbcbb81eb60"


def test_health(client: TestClient) -> None:
    response = client.get("/v1/healthz")
    assert response.status_code == 200
    assert response.json()["ok"] is True


def test_ingest_and_replay_are_idempotent(
    client: TestClient, sample_batch: dict
) -> None:
    endpoint = f"/v1/devices/{DEVICE}/batches"

    first = client.post(endpoint, json=sample_batch)
    assert first.status_code == 200, first.text
    assert first.json()["stored"] == 2
    assert first.json()["duplicates"] == 0

    replay = client.post(endpoint, json=sample_batch)
    assert replay.status_code == 200, replay.text
    assert replay.json()["stored"] == 0
    assert replay.json()["duplicates"] == 2

    regrouped = deepcopy(sample_batch)
    regrouped["batch_id"] = "boot000000000001:retry"
    replay = client.post(endpoint, json=regrouped)
    assert replay.status_code == 200, replay.text
    assert replay.json()["stored"] == 0
    assert replay.json()["duplicates"] == 2

    samples = client.get(f"/v1/devices/{DEVICE}/samples").json()
    transitions = client.get(f"/v1/devices/{DEVICE}/transitions").json()
    assert len(samples["items"]) == 1
    assert samples["truncated"] is False
    assert samples["next_cursor"] is None
    assert len(transitions["items"]) == 1
    assert transitions["truncated"] is False
    assert transitions["next_cursor"] is None


def test_conflicting_record_rolls_back_batch(
    client: TestClient, sample_batch: dict
) -> None:
    endpoint = f"/v1/devices/{DEVICE}/batches"
    assert client.post(endpoint, json=sample_batch).status_code == 200

    conflict = deepcopy(sample_batch)
    conflict["batch_id"] = "boot000000000001:conflict"
    conflict["records"][0]["mic_rms"] = 999.0
    conflict["records"] = [conflict["records"][0]]
    response = client.post(endpoint, json=conflict)
    assert response.status_code == 409

    original = client.get(f"/v1/devices/{DEVICE}/latest").json()
    assert original["mic_rms"] == 500.0


def test_same_batch_id_with_different_body_conflicts(
    client: TestClient, sample_batch: dict
) -> None:
    endpoint = f"/v1/devices/{DEVICE}/batches"
    assert client.post(endpoint, json=sample_batch).status_code == 200
    changed = deepcopy(sample_batch)
    changed["firmware_version"] = "0.1.1"
    assert client.post(endpoint, json=changed).status_code == 409


def test_clock_anchor_backfills_earlier_records(
    client: TestClient, sample_batch: dict
) -> None:
    endpoint = f"/v1/devices/{DEVICE}/batches"
    first = deepcopy(sample_batch)
    first["records"] = [first["records"][0]]
    assert client.post(endpoint, json=first).status_code == 200
    assert client.get(f"/v1/devices/{DEVICE}/latest").json()["observed_at_ms"] is None

    anchored = deepcopy(sample_batch)
    anchored["batch_id"] = "boot000000000001:2"
    anchored["clock_anchor"] = {
        "utc_ms": 1_800_000_000_000,
        "uptime_ms": 2000,
        "source": "sntp",
    }
    anchored["records"] = [
        {
            **anchored["records"][0],
            "seq": 2,
            "uptime_ms": 2000,
        }
    ]
    assert client.post(endpoint, json=anchored).status_code == 200

    samples = client.get(f"/v1/devices/{DEVICE}/samples").json()["items"]
    by_seq = {sample["seq"]: sample for sample in samples}
    assert by_seq[0]["observed_at_ms"] == 1_799_999_999_000
    assert by_seq[0]["time_quality"] == "anchor_sntp"
    assert by_seq[2]["observed_at_ms"] == 1_800_000_000_000


def test_clock_anchor_is_immutable_within_boot(
    client: TestClient, sample_batch: dict
) -> None:
    endpoint = f"/v1/devices/{DEVICE}/batches"
    anchored = deepcopy(sample_batch)
    anchored["clock_anchor"] = {
        "utc_ms": 1_800_000_000_000,
        "uptime_ms": 1000,
        "source": "sntp",
    }
    anchored["records"] = [anchored["records"][0]]
    assert client.post(endpoint, json=anchored).status_code == 200

    conflicting = deepcopy(anchored)
    conflicting["batch_id"] = "boot000000000001:anchor-conflict"
    conflicting["clock_anchor"]["utc_ms"] += 5000
    conflicting["records"][0]["seq"] = 2
    conflicting["records"][0]["uptime_ms"] = 2000
    response = client.post(endpoint, json=conflicting)

    assert response.status_code == 409
    items = client.get(f"/v1/devices/{DEVICE}/samples").json()["items"]
    assert [item["seq"] for item in items] == [0]


def test_invalid_batch_never_partially_writes(
    client: TestClient, sample_batch: dict
) -> None:
    invalid = deepcopy(sample_batch)
    invalid["records"][1]["kind"] = "mystery"
    response = client.post(f"/v1/devices/{DEVICE}/batches", json=invalid)
    assert response.status_code == 422
    assert client.get(f"/v1/devices/{DEVICE}/latest").status_code == 404


def test_batch_rejects_duplicate_record_sequences(
    client: TestClient, sample_batch: dict
) -> None:
    invalid = deepcopy(sample_batch)
    invalid["records"][1]["seq"] = invalid["records"][0]["seq"]

    response = client.post(f"/v1/devices/{DEVICE}/batches", json=invalid)

    assert response.status_code == 422
    assert client.get(f"/v1/devices/{DEVICE}/latest").status_code == 404


def test_sample_rejects_inverted_microphone_range(
    client: TestClient, sample_batch: dict
) -> None:
    invalid = deepcopy(sample_batch)
    invalid["records"] = [invalid["records"][0]]
    invalid["records"][0]["mic_min"] = 1400
    invalid["records"][0]["mic_max"] = 1300

    response = client.post(f"/v1/devices/{DEVICE}/batches", json=invalid)

    assert response.status_code == 422
    assert client.get(f"/v1/devices/{DEVICE}/latest").status_code == 404


def test_concurrent_batch_retries_are_serialized(
    tmp_path: Path, sample_batch: dict
) -> None:
    database = Database(tmp_path / "concurrent.db")
    database.initialize()
    service = PresenceService(database)
    batch = TelemetryBatch.model_validate(sample_batch)
    barrier = Barrier(2)

    def send_batch():
        barrier.wait()
        return service.ingest(DEVICE, batch)

    try:
        with ThreadPoolExecutor(max_workers=2) as executor:
            results = list(executor.map(lambda _index: send_batch(), range(2)))
    finally:
        database.dispose()

    assert sorted(result.stored for result in results) == [0, 2]
    assert sorted(result.duplicates for result in results) == [0, 2]
