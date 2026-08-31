from __future__ import annotations

import json
import os
from dataclasses import dataclass
from ipaddress import ip_address, ip_network
from pathlib import Path
from typing import Any


@dataclass(frozen=True)
class Settings:
    database_path: Path
    api_token: str | None
    require_api_token: bool = False
    ota_trusted_keys: tuple[tuple[str, Path], ...] = ()
    coredump_decoder: Path | None = None
    console_host: str = "192.168.0.46"

    @staticmethod
    def _load_console_host(value: str) -> str:
        try:
            address = ip_address(value)
        except ValueError as error:
            raise RuntimeError(
                "Console host must be a literal LAN IP address"
            ) from error
        if address not in ip_network("192.168.0.0/24"):
            raise RuntimeError("Console host must be inside 192.168.0.0/24")
        return str(address)

    @staticmethod
    def _load_ota_trusted_keys(path_value: str | None) -> tuple[tuple[str, Path], ...]:
        if path_value is None:
            return ()
        trust_path = Path(path_value).expanduser()
        try:
            payload: Any = json.loads(trust_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            raise RuntimeError("OTA trust-set file is unreadable or invalid") from error
        if not isinstance(payload, dict) or not 1 <= len(payload) <= 2:
            raise RuntimeError("OTA trust set must map one or two key IDs to PEM files")
        result: list[tuple[str, Path]] = []
        for key_id, public_key_value in payload.items():
            if (
                not isinstance(key_id, str)
                or not key_id
                or len(key_id) > 32
                or not all(
                    character.isalnum() or character in "._-" for character in key_id
                )
                or not isinstance(public_key_value, str)
            ):
                raise RuntimeError("OTA trust set contains an invalid key entry")
            public_key = Path(public_key_value).expanduser()
            if not public_key.is_file():
                raise RuntimeError(f"OTA public key is unavailable: {public_key}")
            result.append((key_id, public_key))
        return tuple(sorted(result))

    @classmethod
    def from_environment(cls) -> Settings:
        token_from_environment = os.getenv("PRESENCE_API_TOKEN")
        environment_token = (
            token_from_environment.strip() or None
            if token_from_environment is not None
            else None
        )
        credentials_directory = os.getenv("CREDENTIALS_DIRECTORY")
        credential_token = None
        if credentials_directory:
            credential_path = Path(credentials_directory) / "api_token"
            if credential_path.is_file():
                credential_token = (
                    credential_path.read_text(encoding="utf-8").strip() or None
                )
        # A systemd credential is intentionally authoritative in production;
        # a stale environment value must not silently override a rotated key.
        api_token = credential_token or environment_token
        require_api_token = os.getenv("PRESENCE_REQUIRE_TOKEN", "0") == "1"
        if require_api_token and api_token is None:
            raise RuntimeError("presence API token is required but unavailable")
        decoder_value = os.getenv("PRESENCE_COREDUMP_DECODER")
        return cls(
            database_path=Path(
                os.getenv("PRESENCE_DB_PATH", "data/presence.db")
            ).expanduser(),
            api_token=api_token,
            require_api_token=require_api_token,
            ota_trusted_keys=cls._load_ota_trusted_keys(
                os.getenv("PRESENCE_OTA_TRUST_KEYS_PATH")
            ),
            coredump_decoder=(
                Path(decoder_value).expanduser() if decoder_value else None
            ),
            console_host=cls._load_console_host(
                os.getenv("PRESENCE_CONSOLE_HOST", "192.168.0.46")
            ),
        )
