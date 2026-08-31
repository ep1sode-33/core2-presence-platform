from __future__ import annotations

import json
import os
from dataclasses import dataclass
from ipaddress import ip_address, ip_network
from pathlib import Path
from typing import Any

_HOME_LAN_NETWORK = ip_network("192.168.0.0/24")
_TAILSCALE_IPV4_NETWORK = ip_network("100.64.0.0/10")


@dataclass(frozen=True)
class Settings:
    database_path: Path
    api_token: str | None
    require_api_token: bool = False
    ota_trusted_keys: tuple[tuple[str, Path], ...] = ()
    coredump_decoder: Path | None = None
    console_host: str = "192.168.0.46"
    console_tailnet_host: str | None = None
    console_tailnet_client: str | None = None

    @staticmethod
    def _load_console_host(value: str) -> str:
        try:
            address = ip_address(value)
        except ValueError as error:
            raise RuntimeError(
                "Console host must be a literal LAN IP address"
            ) from error
        if address not in _HOME_LAN_NETWORK:
            raise RuntimeError("Console host must be inside 192.168.0.0/24")
        return str(address)

    @staticmethod
    def _load_console_tailnet_access(
        host_value: str | None, client_value: str | None
    ) -> tuple[str | None, str | None]:
        host_value = host_value.strip() if host_value is not None else ""
        client_value = client_value.strip() if client_value is not None else ""
        if not host_value and not client_value:
            return None, None
        if not host_value or not client_value:
            raise RuntimeError(
                "Console Tailnet host and client must be configured together"
            )
        try:
            host_address = ip_address(host_value)
            client_address = ip_address(client_value)
        except ValueError as error:
            raise RuntimeError(
                "Console Tailnet host and client must be literal IPv4 addresses"
            ) from error
        if (
            host_address.version != 4
            or client_address.version != 4
            or host_address not in _TAILSCALE_IPV4_NETWORK
            or client_address not in _TAILSCALE_IPV4_NETWORK
        ):
            raise RuntimeError(
                "Console Tailnet host and client must be inside 100.64.0.0/10"
            )
        return str(host_address), str(client_address)

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
        console_tailnet_host, console_tailnet_client = cls._load_console_tailnet_access(
            os.getenv("PRESENCE_CONSOLE_TAILNET_HOST"),
            os.getenv("PRESENCE_CONSOLE_TAILNET_CLIENT"),
        )
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
            console_tailnet_host=console_tailnet_host,
            console_tailnet_client=console_tailnet_client,
        )
