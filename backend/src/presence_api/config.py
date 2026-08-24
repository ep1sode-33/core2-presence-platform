from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Settings:
    database_path: Path
    api_token: str | None
    require_api_token: bool = False

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
        return cls(
            database_path=Path(
                os.getenv("PRESENCE_DB_PATH", "data/presence.db")
            ).expanduser(),
            api_token=api_token,
            require_api_token=require_api_token,
        )
