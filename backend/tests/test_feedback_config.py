from __future__ import annotations

from copy import deepcopy

from fastapi.testclient import TestClient

from presence_api.config import Settings
from presence_api.main import create_app

DEVICE = "core2-2cbcbb81eb60"


def test_feedback_is_idempotent_and_conflicts_on_mutation(
    client: TestClient,
) -> None:
    endpoint = f"/v1/devices/{DEVICE}/feedback"
    payload = {
        "feedback_id": "feedback-0001",
        "actual_presence": "present",
        "observed_state": "idle",
        "source": "web",
        "note": "screen should have stayed on",
    }
    first = client.post(endpoint, json=payload)
    assert first.status_code == 200, first.text
    assert first.json()["duplicate"] is False

    replay = client.post(endpoint, json=payload)
    assert replay.status_code == 200
    assert replay.json()["duplicate"] is True

    changed = deepcopy(payload)
    changed["actual_presence"] = "absent"
    assert client.post(endpoint, json=changed).status_code == 409
    page = client.get(endpoint).json()
    assert len(page["items"]) == 1
    assert page["truncated"] is False
    assert page["next_cursor"] is None


def test_feedback_record_reference_is_complete(client: TestClient) -> None:
    endpoint = f"/v1/devices/{DEVICE}/feedback"
    response = client.post(
        endpoint,
        json={
            "feedback_id": "feedback-0002",
            "boot_id": "boot000000000001",
            "actual_presence": "present",
            "source": "touch",
        },
    )
    assert response.status_code == 422


def test_feedback_record_reference_must_exist_for_the_same_device(
    client: TestClient, sample_batch: dict
) -> None:
    assert (
        client.post(f"/v1/devices/{DEVICE}/batches", json=sample_batch).status_code
        == 200
    )
    payload = {
        "feedback_id": "feedback-linked-0001",
        "boot_id": sample_batch["boot_id"],
        "seq": 0,
        "actual_presence": "present",
        "source": "touch",
    }

    linked = client.post(f"/v1/devices/{DEVICE}/feedback", json=payload)
    assert linked.status_code == 200, linked.text
    assert linked.json()["occurred_uptime_ms"] == 1000
    assert linked.json()["occurred_at_ms"] is None
    assert linked.json()["time_quality"] == "receive_only"

    missing = {**payload, "feedback_id": "feedback-missing-0001", "seq": 999}
    assert (
        client.post(f"/v1/devices/{DEVICE}/feedback", json=missing).status_code == 404
    )

    other_device = "core2-2cbcbb81eb61"
    cross_device = {**payload, "feedback_id": "feedback-cross-0001"}
    assert (
        client.post(
            f"/v1/devices/{other_device}/feedback", json=cross_device
        ).status_code
        == 404
    )


def test_touch_feedback_requires_acknowledged_telemetry(client: TestClient) -> None:
    payload = {
        "feedback_id": "feedback-touch-0001",
        "actual_presence": "absent",
        "source": "touch",
    }

    response = client.post(f"/v1/devices/{DEVICE}/feedback", json=payload)

    assert response.status_code == 422


def test_late_clock_anchor_backfills_linked_feedback(
    client: TestClient, sample_batch: dict
) -> None:
    batch_endpoint = f"/v1/devices/{DEVICE}/batches"
    initial = deepcopy(sample_batch)
    initial["records"] = [initial["records"][0]]
    assert client.post(batch_endpoint, json=initial).status_code == 200

    feedback_endpoint = f"/v1/devices/{DEVICE}/feedback"
    feedback = {
        "feedback_id": "feedback-anchor-0001",
        "boot_id": initial["boot_id"],
        "seq": 0,
        "actual_presence": "present",
        "source": "touch",
    }
    created = client.post(feedback_endpoint, json=feedback)
    assert created.status_code == 200
    assert created.json()["occurred_at_ms"] is None

    anchored = deepcopy(initial)
    anchored["batch_id"] = "boot000000000001:anchor"
    anchored["clock_anchor"] = {
        "utc_ms": 1_800_000_000_000,
        "uptime_ms": 2000,
        "source": "sntp",
    }
    anchored["records"][0]["seq"] = 2
    anchored["records"][0]["uptime_ms"] = 2000
    assert client.post(batch_endpoint, json=anchored).status_code == 200

    stored = client.get(feedback_endpoint).json()["items"][0]
    assert stored["occurred_at_ms"] == 1_799_999_999_000
    assert stored["occurred_uptime_ms"] == 1000
    assert stored["time_quality"] == "anchor_sntp"


def test_feedback_cursor_handles_identical_receive_times(
    client: TestClient, monkeypatch
) -> None:
    monkeypatch.setattr("presence_api.service.utc_now_ms", lambda: 123456)
    endpoint = f"/v1/devices/{DEVICE}/feedback"
    for suffix in ("a", "b"):
        response = client.post(
            endpoint,
            json={
                "feedback_id": f"feedback-page-{suffix}",
                "actual_presence": "present",
                "source": "web",
            },
        )
        assert response.status_code == 200

    first = client.get(endpoint, params={"limit": 1}).json()
    assert first["truncated"] is True
    assert first["next_cursor"] is not None

    second = client.get(
        endpoint,
        params={"limit": 1, "cursor": first["next_cursor"]},
    ).json()
    assert second["truncated"] is False
    assert second["next_cursor"] is None
    assert first["items"][0]["feedback_id"] != second["items"][0]["feedback_id"]


def test_config_uses_optimistic_revision(client: TestClient) -> None:
    endpoint = f"/v1/devices/{DEVICE}/config"
    initial = client.get(endpoint)
    assert initial.status_code == 200
    assert initial.json()["revision"] == 0

    payload = {
        "base_revision": 0,
        "created_by": "test",
        "config": {
            **initial.json()["config"],
            "pir_hold_ms": 45000,
        },
    }
    updated = client.put(endpoint, json=payload)
    assert updated.status_code == 200, updated.text
    assert updated.json()["revision"] == 1
    assert updated.json()["config"]["pir_hold_ms"] == 45000

    stale = client.put(endpoint, json=payload)
    assert stale.status_code == 409
    assert client.get(endpoint).json()["revision"] == 1


def test_optional_bearer_token(tmp_path) -> None:
    app = create_app(Settings(database_path=tmp_path / "secure.db", api_token="secret"))
    with TestClient(app) as secure_client:
        assert secure_client.get("/v1/healthz").status_code == 200
        endpoint = f"/v1/devices/{DEVICE}/config"
        assert secure_client.get(endpoint).status_code == 401
        response = secure_client.get(
            endpoint, headers={"Authorization": "Bearer secret"}
        )
        assert response.status_code == 200
