from __future__ import annotations

import argparse
import json
from pathlib import Path

from presence_api.config import Settings
from presence_api.main import create_app

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
TARGET = REPOSITORY_ROOT / "contracts" / "api-v1.openapi.json"


def rendered_contract() -> str:
    app = create_app(Settings(database_path=Path(":memory:"), api_token="contract"))
    return json.dumps(app.openapi(), indent=2) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--check",
        action="store_true",
        help="fail if the checked-in OpenAPI contract differs from the app",
    )
    args = parser.parse_args()
    rendered = rendered_contract()

    if args.check:
        if not TARGET.is_file() or TARGET.read_text(encoding="utf-8") != rendered:
            parser.error("OpenAPI contract is stale; run make contract-generate")
        print("OpenAPI contract: current")
        return 0

    TARGET.parent.mkdir(parents=True, exist_ok=True)
    TARGET.write_text(rendered, encoding="utf-8")
    print(f"wrote {TARGET.relative_to(REPOSITORY_ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
