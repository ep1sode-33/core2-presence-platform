from __future__ import annotations

import argparse
import json
from pathlib import Path

from presence_api.contract import telemetry_contract_schema

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
TARGET = REPOSITORY_ROOT / "contracts" / "telemetry-v1.schema.json"


def rendered_schema() -> str:
    return json.dumps(telemetry_contract_schema(), indent=2) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--check",
        action="store_true",
        help="fail if the checked-in schema differs from the model",
    )
    args = parser.parse_args()
    rendered = rendered_schema()

    if args.check:
        if not TARGET.is_file() or TARGET.read_text(encoding="utf-8") != rendered:
            parser.error("telemetry schema is stale; run make contract-generate")
        print("telemetry contract: current")
        return 0

    TARGET.parent.mkdir(parents=True, exist_ok=True)
    TARGET.write_text(rendered, encoding="utf-8")
    print(f"wrote {TARGET.relative_to(REPOSITORY_ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
