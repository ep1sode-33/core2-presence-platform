from __future__ import annotations

import json
from copy import deepcopy
from pathlib import Path

import pytest
from fastapi.testclient import TestClient
from jsonschema import Draft202012Validator
from pydantic import ValidationError

from presence_api.contract import (
    telemetry_contract_schema,
    validate_telemetry_payload,
)

DEVICE = "core2-2cbcbb81eb60"
SCHEMA_PATH = (
    Path(__file__).resolve().parents[2] / "contracts" / "telemetry-v1.schema.json"
)


def test_published_schema_is_generated_from_the_api_model() -> None:
    published = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))

    assert published == telemetry_contract_schema()


def test_valid_payload_passes_published_and_executable_contracts(
    client: TestClient, sample_batch: dict
) -> None:
    Draft202012Validator(telemetry_contract_schema()).validate(sample_batch)
    validated = validate_telemetry_payload(sample_batch)

    response = client.post(f"/v1/devices/{DEVICE}/batches", json=sample_batch)

    assert validated.batch_id == sample_batch["batch_id"]
    assert response.status_code == 200, response.text


@pytest.mark.parametrize("case", ["duplicate_seq", "inverted_microphone_range"])
def test_semantic_corpus_matches_executable_contract_and_api(
    case: str, client: TestClient, sample_batch: dict
) -> None:
    invalid = deepcopy(sample_batch)
    if case == "duplicate_seq":
        invalid["records"][1]["seq"] = invalid["records"][0]["seq"]
    else:
        invalid["records"] = [invalid["records"][0]]
        invalid["records"][0]["mic_min"] = 1400
        invalid["records"][0]["mic_max"] = 1300

    # These are cross-item/cross-field semantics that Draft 2020-12 cannot
    # express. The shared executable contract and API must reject both.
    Draft202012Validator(telemetry_contract_schema()).validate(invalid)
    with pytest.raises(ValidationError):
        validate_telemetry_payload(invalid)

    response = client.post(f"/v1/devices/{DEVICE}/batches", json=invalid)
    assert response.status_code == 422


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("seq", "0"),
        ("seq", 1 << 63),
        ("mic_rms", "500.0"),
        ("pir", 1),
    ],
)
def test_json_types_match_executable_contract_and_api(
    field: str, value: object, client: TestClient, sample_batch: dict
) -> None:
    invalid = deepcopy(sample_batch)
    invalid["records"][0][field] = value
    validator = Draft202012Validator(telemetry_contract_schema())

    assert list(validator.iter_errors(invalid))
    with pytest.raises(ValidationError):
        validate_telemetry_payload(invalid)

    response = client.post(f"/v1/devices/{DEVICE}/batches", json=invalid)
    assert response.status_code == 422
