from __future__ import annotations

from pathlib import Path

import pytest

from presence_api.config import Settings


def clear_token_environment(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("PRESENCE_API_TOKEN", raising=False)
    monkeypatch.delenv("CREDENTIALS_DIRECTORY", raising=False)
    monkeypatch.delenv("PRESENCE_REQUIRE_TOKEN", raising=False)


def test_required_token_fails_closed(monkeypatch: pytest.MonkeyPatch) -> None:
    clear_token_environment(monkeypatch)
    monkeypatch.setenv("PRESENCE_REQUIRE_TOKEN", "1")

    with pytest.raises(RuntimeError, match="required but unavailable"):
        Settings.from_environment()


def test_systemd_credential_supplies_required_token(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    clear_token_environment(monkeypatch)
    (tmp_path / "api_token").write_text("credential-secret\n", encoding="utf-8")
    monkeypatch.setenv("CREDENTIALS_DIRECTORY", str(tmp_path))
    monkeypatch.setenv("PRESENCE_REQUIRE_TOKEN", "1")

    settings = Settings.from_environment()

    assert settings.api_token == "credential-secret"
    assert settings.require_api_token is True


def test_systemd_credential_overrides_environment_token(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    clear_token_environment(monkeypatch)
    (tmp_path / "api_token").write_text("credential-secret\n", encoding="utf-8")
    monkeypatch.setenv("CREDENTIALS_DIRECTORY", str(tmp_path))
    monkeypatch.setenv("PRESENCE_API_TOKEN", "stale-environment-secret")
    monkeypatch.setenv("PRESENCE_REQUIRE_TOKEN", "1")

    settings = Settings.from_environment()

    assert settings.api_token == "credential-secret"


def test_whitespace_environment_token_is_not_accepted(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    clear_token_environment(monkeypatch)
    monkeypatch.setenv("PRESENCE_API_TOKEN", "   ")
    monkeypatch.setenv("PRESENCE_REQUIRE_TOKEN", "1")

    with pytest.raises(RuntimeError, match="required but unavailable"):
        Settings.from_environment()
