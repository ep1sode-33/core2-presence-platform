from __future__ import annotations

from pathlib import Path

import pytest

from presence_api.config import Settings


def clear_token_environment(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("PRESENCE_API_TOKEN", raising=False)
    monkeypatch.delenv("CREDENTIALS_DIRECTORY", raising=False)
    monkeypatch.delenv("PRESENCE_REQUIRE_TOKEN", raising=False)
    monkeypatch.delenv("PRESENCE_OTA_TRUST_KEYS_PATH", raising=False)
    monkeypatch.delenv("PRESENCE_COREDUMP_DECODER", raising=False)
    monkeypatch.delenv("PRESENCE_CONSOLE_HOST", raising=False)


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


def test_console_host_must_be_a_literal_home_lan_address(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    clear_token_environment(monkeypatch)
    monkeypatch.setenv("PRESENCE_CONSOLE_HOST", "devb.example")
    with pytest.raises(RuntimeError, match="literal LAN IP"):
        Settings.from_environment()

    monkeypatch.setenv("PRESENCE_CONSOLE_HOST", "100.117.242.46")
    with pytest.raises(RuntimeError, match="inside 192.168.0.0/24"):
        Settings.from_environment()


def test_ota_trust_set_and_coredump_decoder_load_from_environment(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    clear_token_environment(monkeypatch)
    public_key = tmp_path / "release-current.pem"
    public_key.write_text("test public key", encoding="utf-8")
    trust_set = tmp_path / "ota-trust.json"
    trust_set.write_text(
        '{"release-current":"' + str(public_key) + '"}', encoding="utf-8"
    )
    decoder = tmp_path / "esp-coredump"
    monkeypatch.setenv("PRESENCE_OTA_TRUST_KEYS_PATH", str(trust_set))
    monkeypatch.setenv("PRESENCE_COREDUMP_DECODER", str(decoder))

    settings = Settings.from_environment()

    assert settings.ota_trusted_keys == (("release-current", public_key),)
    assert settings.coredump_decoder == decoder


def test_ota_trust_set_is_limited_to_current_and_next_key(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    clear_token_environment(monkeypatch)
    trust_set = tmp_path / "ota-trust.json"
    trust_set.write_text(
        '{"one":"/one.pem","two":"/two.pem","three":"/three.pem"}',
        encoding="utf-8",
    )
    monkeypatch.setenv("PRESENCE_OTA_TRUST_KEYS_PATH", str(trust_set))

    with pytest.raises(RuntimeError, match="one or two key IDs"):
        Settings.from_environment()
