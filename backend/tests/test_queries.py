from __future__ import annotations

import base64
import json
from copy import deepcopy

from fastapi.testclient import TestClient

DEVICE = "core2-2cbcbb81eb60"


def encode_test_cursor(payload: dict) -> str:
    encoded = json.dumps(payload, separators=(",", ":")).encode()
    return base64.urlsafe_b64encode(encoded).decode().rstrip("=")


def test_time_window_and_limit(client: TestClient, sample_batch: dict) -> None:
    sample_batch["clock_anchor"] = {
        "utc_ms": 1_800_000_000_000,
        "uptime_ms": 1000,
        "source": "sntp",
    }
    response = client.post(f"/v1/devices/{DEVICE}/batches", json=sample_batch)
    assert response.status_code == 200, response.text
    received_at_ms = client.get(f"/v1/devices/{DEVICE}/samples").json()["items"][0][
        "received_at_ms"
    ]

    included = client.get(
        f"/v1/devices/{DEVICE}/samples",
        params={
            "start_ms": received_at_ms,
            "end_ms": received_at_ms,
            "limit": 1,
        },
    )
    assert included.status_code == 200
    assert len(included.json()["items"]) == 1
    assert included.json()["truncated"] is False
    assert included.json()["next_cursor"] is None

    excluded = client.get(
        f"/v1/devices/{DEVICE}/samples",
        params={"start_ms": received_at_ms + 1},
    )
    assert excluded.status_code == 200
    assert excluded.json() == {
        "items": [],
        "truncated": False,
        "next_cursor": None,
    }

    invalid = client.get(
        f"/v1/devices/{DEVICE}/samples",
        params={"start_ms": 20, "end_ms": 10},
    )
    assert invalid.status_code == 400


def test_range_response_marks_truncation(
    client: TestClient, sample_batch: dict
) -> None:
    second_sample = {
        **sample_batch["records"][0],
        "seq": 2,
        "uptime_ms": 2000,
    }
    sample_batch["records"].append(second_sample)
    response = client.post(f"/v1/devices/{DEVICE}/batches", json=sample_batch)
    assert response.status_code == 200, response.text

    page = client.get(f"/v1/devices/{DEVICE}/samples", params={"limit": 1}).json()

    assert len(page["items"]) == 1
    assert page["truncated"] is True
    assert page["next_cursor"] is not None

    anchored = deepcopy(sample_batch)
    anchored["batch_id"] = "boot000000000001:late-anchor-page"
    anchored["clock_anchor"] = {
        "utc_ms": 1_800_000_000_000,
        "uptime_ms": 3000,
        "source": "sntp",
    }
    anchored["records"] = [
        {
            **anchored["records"][0],
            "seq": 3,
            "uptime_ms": 3000,
        }
    ]
    anchor_response = client.post(f"/v1/devices/{DEVICE}/batches", json=anchored)
    assert anchor_response.status_code == 200, anchor_response.text

    second_page = client.get(
        f"/v1/devices/{DEVICE}/samples",
        params={"limit": 1, "cursor": page["next_cursor"]},
    ).json()
    assert len(second_page["items"]) == 1
    assert second_page["truncated"] is False
    assert second_page["next_cursor"] is None
    assert page["items"][0]["seq"] != second_page["items"][0]["seq"]


def test_invalid_cursor_is_rejected(client: TestClient) -> None:
    response = client.get(
        f"/v1/devices/{DEVICE}/samples", params={"cursor": "not-a-cursor"}
    )

    assert response.status_code == 400


def test_out_of_range_cursors_are_rejected(client: TestClient) -> None:
    huge = 10**100
    telemetry_cursor = encode_test_cursor(
        {"v": 1, "k": "telemetry", "t": huge, "b": "boot000000000001", "s": 0}
    )
    feedback_cursor = encode_test_cursor(
        {"v": 1, "k": "feedback", "t": huge, "i": "feedback-0001"}
    )

    samples = client.get(
        f"/v1/devices/{DEVICE}/samples", params={"cursor": telemetry_cursor}
    )
    feedback = client.get(
        f"/v1/devices/{DEVICE}/feedback", params={"cursor": feedback_cursor}
    )

    assert samples.status_code == 400
    assert feedback.status_code == 400


def test_device_id_must_use_core2_efuse_format(client: TestClient) -> None:
    response = client.get("/v1/devices/2c-bc-bb-81-eb-60/config")

    assert response.status_code == 422
