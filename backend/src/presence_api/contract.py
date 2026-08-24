from __future__ import annotations

from typing import Any

from .schemas import TelemetryBatch

SCHEMA_DIALECT = "https://json-schema.org/draft/2020-12/schema"
SCHEMA_ID = "https://m5-presence.local/schemas/telemetry-v1.schema.json"
SEMANTIC_CONSTRAINTS = (
    "records[*].seq values must be unique within one batch",
    "each sample mic_min must be less than or equal to mic_max",
)


def telemetry_contract_schema() -> dict[str, Any]:
    """Return the generated structural projection of the executable contract."""
    generated = TelemetryBatch.model_json_schema(mode="validation")
    generated["title"] = "Core2 presence telemetry batch v1"
    return {
        "$schema": SCHEMA_DIALECT,
        "$id": SCHEMA_ID,
        "$comment": (
            "Generated from presence_api.schemas.TelemetryBatch. The executable "
            "contract also enforces every rule in x-semantic-constraints."
        ),
        "x-semantic-constraints": list(SEMANTIC_CONSTRAINTS),
        **generated,
    }


def validate_telemetry_payload(payload: object) -> TelemetryBatch:
    """Apply the same structural and semantic validation used by the API."""
    return TelemetryBatch.model_validate(payload)
