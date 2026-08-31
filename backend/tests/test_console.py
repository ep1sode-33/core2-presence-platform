from __future__ import annotations

from copy import deepcopy
from pathlib import Path

import pytest
from fastapi import HTTPException, Request
from fastapi.testclient import TestClient

from presence_api.config import Settings
from presence_api.console import ONLINE_THRESHOLD_MS, require_console_access
from presence_api.main import create_app

DEVICE = "core2-2cbcbb81eb60"


def test_console_shell_uses_a_trusted_source_without_a_token_flow(
    tmp_path: Path,
) -> None:
    app = create_app(
        Settings(database_path=tmp_path / "secure.db", api_token="secret-token")
    )
    with TestClient(
        app,
        client=("192.168.0.42", 50_000),
        headers={
            "Host": "192.168.0.46",
            "Origin": "http://192.168.0.46",
        },
    ) as client:
        page = client.get("/console")
        script = client.get("/console/assets/console.js")
        stylesheet = client.get("/console/assets/console.css")

        assert page.status_code == 200
        assert page.headers["content-security-policy"].startswith("default-src 'self'")
        assert page.headers["x-frame-options"] == "DENY"
        assert "secret-token" not in page.text
        assert "https://" not in page.text
        assert script.status_code == 200
        assert "sessionStorage" not in script.text
        assert "localStorage" not in script.text
        assert "Authorization" not in script.text
        assert "Bearer token" not in page.text
        assert "https://" not in script.text
        assert stylesheet.status_code == 200
        assert client.get("/console/assets/unknown.js").status_code == 404


def test_console_data_uses_lan_source_while_existing_api_keeps_bearer_auth(
    tmp_path: Path,
) -> None:
    app = create_app(Settings(database_path=tmp_path / "secure.db", api_token="secret"))
    with TestClient(
        app,
        client=("192.168.0.42", 50_000),
        headers={
            "Host": "192.168.0.46",
            "Origin": "http://192.168.0.46",
        },
    ) as client:
        devices = client.get("/v1/console/devices")
        assert devices.status_code == 200
        assert devices.json()["items"] == []
        assert client.get(f"/v1/console/devices/{DEVICE}/snapshot").status_code == 404

        assert client.get(f"/v1/devices/{DEVICE}/config").status_code == 401
        assert client.get(f"/v1/devices/{DEVICE}/latest").status_code == 401
        authorized = client.get(
            f"/v1/devices/{DEVICE}/config",
            headers={"Authorization": "Bearer secret"},
        )
        assert authorized.status_code == 200

        update = {
            "base_revision": authorized.json()["revision"],
            "created_by": "console-test",
            "config": authorized.json()["config"],
        }
        assert (
            client.put(f"/v1/devices/{DEVICE}/config", json=update).status_code == 401
        )
        console_update = client.put(f"/v1/console/devices/{DEVICE}/config", json=update)
        assert console_update.status_code == 200
        assert console_update.json()["revision"] == 1
        assert (
            client.put(f"/v1/console/devices/{DEVICE}/config", json=update).status_code
            == 409
        )


@pytest.mark.parametrize(
    "client_host",
    [
        "192.168.1.42",
        "100.117.242.46",
        "100.118.9.99",
        "127.0.0.1",
        "not-an-ip",
    ],
)
def test_console_rejects_every_source_outside_the_home_lan(
    tmp_path: Path, client_host: str
) -> None:
    app = create_app(
        Settings(database_path=tmp_path / "blocked.db", api_token="secret")
    )
    with TestClient(app, client=(client_host, 50_000)) as client:
        headers = {
            "Authorization": "Bearer secret",
            "X-Forwarded-For": "192.168.0.42",
        }
        assert client.get("/console", headers=headers).status_code == 403
        assert (
            client.get("/console/assets/console.js", headers=headers).status_code == 403
        )
        assert client.get("/v1/console/devices", headers=headers).status_code == 403
        assert (
            client.get(
                f"/v1/console/devices/{DEVICE}/snapshot", headers=headers
            ).status_code
            == 403
        )
        assert (
            client.put(
                f"/v1/console/devices/{DEVICE}/config",
                headers=headers,
                json={},
            ).status_code
            == 403
        )
        assert (
            client.get(
                f"/v1/console/devices/{DEVICE}/coredumps", headers=headers
            ).status_code
            == 403
        )
        assert client.get("/v1/console/releases", headers=headers).status_code == 403
        assert (
            client.post(
                "/v1/console/releases/import", headers=headers, json={}
            ).status_code
            == 403
        )


def test_console_rejects_a_missing_client_address() -> None:
    request = Request({"type": "http", "client": None})
    with pytest.raises(HTTPException) as error:
        require_console_access(request)
    assert error.value.status_code == 403


def test_console_allows_only_the_configured_tailnet_client_and_origin(
    tmp_path: Path,
) -> None:
    app = create_app(
        Settings(
            database_path=tmp_path / "tailnet.db",
            api_token=None,
            console_tailnet_host="100.117.242.46",
            console_tailnet_client="100.118.9.99",
        )
    )
    tailnet_headers = {
        "Host": "100.117.242.46:8081",
        "Origin": "http://100.117.242.46:8081",
    }
    with TestClient(app, client=("100.118.9.99", 50_000)) as client:
        assert (
            client.get("/console", headers={"Host": "100.117.242.46:8081"}).status_code
            == 200
        )
        assert client.get("/console", headers=tailnet_headers).status_code == 200
        assert (
            client.get("/v1/console/devices", headers=tailnet_headers).status_code
            == 200
        )
        command = client.post(
            f"/v1/console/devices/{DEVICE}/commands",
            headers=tailnet_headers,
            json={"command": {"action": "diagnostic_snapshot"}},
        )
        assert command.status_code == 200
        assert (
            client.post(
                f"/v1/console/devices/{DEVICE}/commands",
                headers={"Host": "100.117.242.46:8081"},
                json={"command": {"action": "diagnostic_snapshot"}},
            ).status_code
            == 403
        )
        assert (
            client.get(
                "/console",
                headers={"Host": "192.168.0.46:8081"},
            ).status_code
            == 403
        )
        assert (
            client.get(
                "/console",
                headers={
                    "Host": "100.117.242.46:8081",
                    "Origin": "http://100.117.242.46:8082",
                },
            ).status_code
            == 403
        )
        assert (
            client.get(
                "/console",
                headers={
                    "Host": "100.117.242.46:8081",
                    "Origin": "http://192.168.0.46:8081",
                },
            ).status_code
            == 403
        )

    with TestClient(app, client=("100.118.9.98", 50_000)) as client:
        assert (
            client.get(
                "/console",
                headers={
                    **tailnet_headers,
                    "X-Forwarded-For": "100.118.9.99",
                },
            ).status_code
            == 403
        )

    with TestClient(app, client=("192.168.0.42", 50_000)) as client:
        assert client.get("/console", headers=tailnet_headers).status_code == 403


@pytest.mark.parametrize(
    ("headers", "expected_status"),
    [
        ({"Host": "192.168.0.46:8081"}, 200),
        (
            {
                "Host": "192.168.0.46:8081",
                "Origin": "http://192.168.0.46:8081",
            },
            200,
        ),
        ({"Host": "attacker.example"}, 403),
        ({"Host": "testserver"}, 403),
        ({"Host": "localhost"}, 403),
        (
            {
                "Host": "192.168.0.46:8081",
                "Origin": "http://attacker.example",
            },
            403,
        ),
    ],
)
def test_console_rejects_dns_rebinding_origins(
    tmp_path: Path, headers: dict[str, str], expected_status: int
) -> None:
    app = create_app(Settings(database_path=tmp_path / "origin.db", api_token=None))
    with TestClient(app, client=("192.168.0.42", 50_000)) as client:
        assert client.get("/console", headers=headers).status_code == expected_status


def test_console_state_changes_require_an_exact_same_origin(tmp_path: Path) -> None:
    app = create_app(Settings(database_path=tmp_path / "csrf.db", api_token=None))
    with TestClient(app, client=("192.168.0.42", 50_000)) as client:
        path = f"/v1/console/devices/{DEVICE}/commands"
        body = {"command": {"action": "diagnostic_snapshot"}}
        assert client.post(path, json=body).status_code == 403
        assert (
            client.post(
                path,
                json=body,
                headers={"Origin": "http://attacker.example"},
            ).status_code
            == 403
        )
        accepted = client.post(
            path,
            json=body,
            headers={
                "Host": "192.168.0.46",
                "Origin": "http://192.168.0.46",
            },
        )
        assert accepted.status_code == 200, accepted.text


def test_config_write_cannot_make_a_device_look_online(
    client: TestClient, sample_batch: dict, monkeypatch
) -> None:
    assert (
        client.post(f"/v1/devices/{DEVICE}/batches", json=sample_batch).status_code
        == 200
    )
    received_at_ms = client.get(f"/v1/devices/{DEVICE}/latest").json()["received_at_ms"]

    monkeypatch.setattr(
        "presence_api.console.utc_now_ms", lambda: received_at_ms + 1_000
    )
    online = client.get("/v1/console/devices").json()["items"][0]
    assert online["online"] is True
    assert online["last_seen_at_ms"] == received_at_ms

    current = client.get(f"/v1/devices/{DEVICE}/config").json()
    update = {
        "base_revision": current["revision"],
        "created_by": "console-test",
        "config": {**current["config"], "pir_hold_ms": 45_000},
    }
    assert client.put(f"/v1/devices/{DEVICE}/config", json=update).status_code == 200

    monkeypatch.setattr(
        "presence_api.console.utc_now_ms",
        lambda: received_at_ms + ONLINE_THRESHOLD_MS + 1,
    )
    offline = client.get("/v1/console/devices").json()["items"][0]
    assert offline["online"] is False
    assert offline["last_seen_at_ms"] == received_at_ms
    assert offline["last_seen_age_ms"] == ONLINE_THRESHOLD_MS + 1
    assert offline["latest_reported_config_revision"] == 0
    assert offline["highest_applied_config_revision"] == 0
    assert offline["config_sync"] == "pending"


def test_config_only_device_has_no_telemetry_last_seen(
    client: TestClient, monkeypatch
) -> None:
    monkeypatch.setattr("presence_api.console.utc_now_ms", lambda: 1_800_000_000_000)
    current = client.get(f"/v1/devices/{DEVICE}/config").json()
    response = client.put(
        f"/v1/devices/{DEVICE}/config",
        json={
            "base_revision": current["revision"],
            "created_by": "console-test",
            "config": current["config"],
        },
    )
    assert response.status_code == 200

    device = client.get("/v1/console/devices").json()["items"][0]
    assert device["online"] is False
    assert device["last_seen_at_ms"] is None
    assert device["last_seen_age_ms"] is None
    assert device["latest_state"] is None
    assert device["latest_reported_config_revision"] is None
    assert device["highest_applied_config_revision"] == 0
    assert device["config_sync"] == "unknown"


def test_console_detects_a_device_config_revision_regression(
    client: TestClient, sample_batch: dict, monkeypatch
) -> None:
    current = client.get(f"/v1/devices/{DEVICE}/config").json()
    issued = client.put(
        f"/v1/devices/{DEVICE}/config",
        json={
            "base_revision": current["revision"],
            "created_by": "console-test",
            "config": current["config"],
        },
    )
    assert issued.status_code == 200
    assert issued.json()["revision"] == 1

    high_revision = deepcopy(sample_batch)
    high_revision["applied_config_revision"] = 1
    monkeypatch.setattr("presence_api.service.utc_now_ms", lambda: 1_800_000_000_000)
    assert (
        client.post(f"/v1/devices/{DEVICE}/batches", json=high_revision).status_code
        == 200
    )

    regressed = deepcopy(sample_batch)
    regressed["batch_id"] = "boot000000000002:0-1"
    regressed["boot_id"] = "boot000000000002"
    monkeypatch.setattr("presence_api.service.utc_now_ms", lambda: 1_800_000_001_000)
    assert (
        client.post(f"/v1/devices/{DEVICE}/batches", json=regressed).status_code == 200
    )

    monkeypatch.setattr("presence_api.console.utc_now_ms", lambda: 1_800_000_002_000)
    device = client.get("/v1/console/devices").json()["items"][0]
    assert device["desired_config_revision"] == 1
    assert device["latest_reported_config_revision"] == 0
    assert device["highest_applied_config_revision"] == 1
    assert device["config_sync"] == "regressed"


def test_console_snapshot_aligns_replayed_samples_transitions_and_feedback(
    client: TestClient, sample_batch: dict, monkeypatch
) -> None:
    observed_ms = 1_800_000_000_000
    received_ms = observed_ms + 600_000
    anchored = deepcopy(sample_batch)
    anchored["clock_anchor"] = {
        "utc_ms": observed_ms,
        "uptime_ms": 1_000,
        "source": "sntp",
    }
    monkeypatch.setattr("presence_api.service.utc_now_ms", lambda: received_ms)
    assert (
        client.post(f"/v1/devices/{DEVICE}/batches", json=anchored).status_code == 200
    )
    feedback = client.post(
        f"/v1/devices/{DEVICE}/feedback",
        json={
            "feedback_id": "console-feedback-0001",
            "boot_id": anchored["boot_id"],
            "seq": 0,
            "actual_presence": "present",
            "observed_state": "present",
            "source": "touch",
        },
    )
    assert feedback.status_code == 200, feedback.text

    monkeypatch.setattr("presence_api.console.utc_now_ms", lambda: received_ms + 1_000)
    response = client.get(
        f"/v1/console/devices/{DEVICE}/snapshot",
        params={"hours": 1, "max_points": 60},
    )
    assert response.status_code == 200, response.text
    body = response.json()

    assert body["window"]["time_basis"] == "observed_at_ms_with_receive_fallback"
    assert body["window"]["sample_count"] == 1
    assert body["window"]["transition_count"] == 1
    assert body["window"]["feedback_count"] == 1
    assert body["series"][0]["start_ms"] == observed_ms
    assert body["series"][0]["end_ms"] == observed_ms
    assert body["transitions"][0]["marker_ms"] == observed_ms + 20
    assert body["feedback"][0]["marker_ms"] == observed_ms
    assert body["calibration"]["present_fraction"] == 1.0
    assert body["calibration"]["feedback"] == {
        "present": 1,
        "absent": 0,
        "mismatch": 0,
    }


def test_unanchored_feedback_uses_its_telemetry_receive_time(
    client: TestClient, sample_batch: dict, monkeypatch
) -> None:
    received_ms = 1_800_000_000_000
    monkeypatch.setattr("presence_api.service.utc_now_ms", lambda: received_ms)
    assert (
        client.post(f"/v1/devices/{DEVICE}/batches", json=sample_batch).status_code
        == 200
    )

    monkeypatch.setattr("presence_api.service.utc_now_ms", lambda: received_ms + 30_000)
    response = client.post(
        f"/v1/devices/{DEVICE}/feedback",
        json={
            "feedback_id": "console-feedback-unanchored",
            "boot_id": sample_batch["boot_id"],
            "seq": 0,
            "actual_presence": "present",
            "observed_state": "present",
            "source": "touch",
        },
    )
    assert response.status_code == 200
    assert response.json()["occurred_at_ms"] is None

    monkeypatch.setattr("presence_api.console.utc_now_ms", lambda: received_ms + 31_000)
    body = client.get(
        f"/v1/console/devices/{DEVICE}/snapshot",
        params={"hours": 1, "max_points": 60},
    ).json()

    assert body["series"][0]["start_ms"] == received_ms
    assert body["feedback"][0]["marker_ms"] == received_ms


def test_console_window_prefers_observed_time_and_falls_back_to_receive_time(
    client: TestClient, sample_batch: dict, monkeypatch
) -> None:
    received_ms = 1_800_000_000_000
    old_observed_ms = received_ms - 2 * 3_600_000
    replayed = deepcopy(sample_batch)
    replayed["clock_anchor"] = {
        "utc_ms": old_observed_ms,
        "uptime_ms": 1_000,
        "source": "sntp",
    }
    monkeypatch.setattr("presence_api.service.utc_now_ms", lambda: received_ms)
    assert (
        client.post(f"/v1/devices/{DEVICE}/batches", json=replayed).status_code == 200
    )

    live = deepcopy(sample_batch)
    live["batch_id"] = "boot000000000002:0-1"
    live["boot_id"] = "boot000000000002"
    monkeypatch.setattr("presence_api.service.utc_now_ms", lambda: received_ms + 100)
    assert client.post(f"/v1/devices/{DEVICE}/batches", json=live).status_code == 200

    monkeypatch.setattr("presence_api.console.utc_now_ms", lambda: received_ms + 1_000)
    body = client.get(
        f"/v1/console/devices/{DEVICE}/snapshot",
        params={"hours": 1, "max_points": 60},
    ).json()

    assert body["window"]["sample_count"] == 1
    assert body["window"]["transition_count"] == 1
    assert body["series"][0]["start_ms"] == received_ms + 100
    assert body["transitions"][0]["marker_ms"] == received_ms + 100


def test_console_downsampling_respects_max_points_at_inclusive_endpoints(
    client: TestClient, sample_batch: dict, monkeypatch
) -> None:
    now_ms = 1_800_000_900_000
    window_ms = 15 * 60 * 1_000
    start_ms = now_ms - window_ms
    batch = deepcopy(sample_batch)
    batch["batch_id"] = "boot000000000003:0-60"
    batch["boot_id"] = "boot000000000003"
    batch["clock_anchor"] = {
        "utc_ms": start_ms,
        "uptime_ms": 0,
        "source": "sntp",
    }
    template = sample_batch["records"][0]
    batch["records"] = [
        {
            **template,
            "seq": index,
            "uptime_ms": index * 15_000,
        }
        for index in range(61)
    ]

    monkeypatch.setattr("presence_api.service.utc_now_ms", lambda: now_ms)
    assert client.post(f"/v1/devices/{DEVICE}/batches", json=batch).status_code == 200

    monkeypatch.setattr("presence_api.console.utc_now_ms", lambda: now_ms)
    response = client.get(
        f"/v1/console/devices/{DEVICE}/snapshot",
        params={"hours": 0.25, "max_points": 60},
    )
    assert response.status_code == 200, response.text
    body = response.json()

    assert body["window"]["sample_count"] == 61
    assert len(body["series"]) == 60
    assert body["series"][0]["start_ms"] == start_ms
    assert body["series"][-1]["end_ms"] == now_ms


def test_feedback_counts_are_not_limited_by_visible_markers(
    client: TestClient, monkeypatch
) -> None:
    event_ms = 1_800_000_000_000
    monkeypatch.setattr("presence_api.service.utc_now_ms", lambda: event_ms)
    current = client.get(f"/v1/devices/{DEVICE}/config").json()
    assert (
        client.put(
            f"/v1/devices/{DEVICE}/config",
            json={
                "base_revision": current["revision"],
                "created_by": "console-test",
                "config": current["config"],
            },
        ).status_code
        == 200
    )
    for index, actual in enumerate(("present", "absent", "present")):
        response = client.post(
            f"/v1/devices/{DEVICE}/feedback",
            json={
                "feedback_id": f"console-page-{index:04d}",
                "actual_presence": actual,
                "observed_state": "idle",
                "source": "web",
            },
        )
        assert response.status_code == 200

    monkeypatch.setattr("presence_api.console.MAX_EVENT_MARKERS", 2)
    monkeypatch.setattr("presence_api.console.utc_now_ms", lambda: event_ms + 1_000)
    body = client.get(
        f"/v1/console/devices/{DEVICE}/snapshot", params={"hours": 1}
    ).json()

    assert len(body["feedback"]) == 2
    assert body["window"]["feedback_truncated"] is True
    assert body["window"]["feedback_count"] == 3
    assert body["calibration"]["feedback"] == {
        "present": 2,
        "absent": 1,
        "mismatch": 2,
    }


def test_console_snapshot_validates_window_and_device(client: TestClient) -> None:
    assert client.get(f"/v1/console/devices/{DEVICE}/snapshot").status_code == 404
    assert (
        client.get(
            f"/v1/console/devices/{DEVICE}/snapshot", params={"hours": 25}
        ).status_code
        == 422
    )
    assert (
        client.get(
            f"/v1/console/devices/{DEVICE}/snapshot", params={"max_points": 59}
        ).status_code
        == 422
    )
