from __future__ import annotations

import ast
import unittest
from pathlib import Path

SCRIPT_PATH = Path(__file__).resolve().parents[1] / "serve_presence_api.py"


class ServePresenceApiConfigurationTests(unittest.TestCase):
    def test_uvicorn_transport_timeouts_and_proxy_boundary(self) -> None:
        tree = ast.parse(SCRIPT_PATH.read_text(encoding="utf-8"))
        config_calls = [
            node
            for node in ast.walk(tree)
            if isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and isinstance(node.func.value, ast.Name)
            and node.func.value.id == "uvicorn"
            and node.func.attr == "Config"
        ]
        self.assertEqual(len(config_calls), 1)

        keywords = {
            keyword.arg: ast.literal_eval(keyword.value)
            for keyword in config_calls[0].keywords
            if keyword.arg in {
                "proxy_headers",
                "timeout_keep_alive",
                "timeout_graceful_shutdown",
            }
        }
        self.assertEqual(
            keywords,
            {
                "proxy_headers": False,
                "timeout_keep_alive": 90,
                "timeout_graceful_shutdown": 30,
            },
        )


if __name__ == "__main__":
    unittest.main()
