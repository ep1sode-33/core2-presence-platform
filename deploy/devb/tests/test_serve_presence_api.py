from __future__ import annotations

import ast
import unittest
from pathlib import Path

SCRIPT_PATH = Path(__file__).resolve().parents[1] / "serve_presence_api.py"
SERVICE_PATH = Path(__file__).resolve().parents[1] / "m5-presence-api.service"


class ServePresenceApiConfigurationTests(unittest.TestCase):
    def test_production_console_tailnet_allowlist_is_exact(self) -> None:
        environment = {}
        for line in SERVICE_PATH.read_text(encoding="utf-8").splitlines():
            if line.startswith('Environment="') and line.endswith('"'):
                key, value = line[len('Environment="') : -1].split("=", 1)
                environment[key] = value

        self.assertEqual(
            environment["PRESENCE_CONSOLE_TAILNET_HOST"], "100.117.242.46"
        )
        self.assertEqual(
            environment["PRESENCE_CONSOLE_TAILNET_CLIENT"], "100.118.9.99"
        )

    def test_listener_boundary_and_explicit_socket_wiring(self) -> None:
        tree = ast.parse(SCRIPT_PATH.read_text(encoding="utf-8"))
        getenv_defaults = {}
        for node in ast.walk(tree):
            if (
                isinstance(node, ast.Call)
                and isinstance(node.func, ast.Attribute)
                and isinstance(node.func.value, ast.Name)
                and node.func.value.id == "os"
                and node.func.attr == "getenv"
                and len(node.args) == 2
            ):
                getenv_defaults[ast.literal_eval(node.args[0])] = ast.literal_eval(
                    node.args[1]
                )
        self.assertEqual(
            getenv_defaults,
            {
                "PRESENCE_LISTEN_ADDRESSES": "100.117.242.46,192.168.0.46",
                "PRESENCE_PORT": "8081",
            },
        )

        run_calls = [
            node
            for node in ast.walk(tree)
            if isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and node.func.attr == "run"
        ]
        self.assertEqual(len(run_calls), 1)
        sockets_keywords = [
            keyword
            for keyword in run_calls[0].keywords
            if keyword.arg == "sockets"
        ]
        self.assertEqual(len(sockets_keywords), 1)
        self.assertIsInstance(sockets_keywords[0].value, ast.Name)
        self.assertEqual(sockets_keywords[0].value.id, "listeners")

    def test_listener_enables_bounded_tcp_keepalive(self) -> None:
        tree = ast.parse(SCRIPT_PATH.read_text(encoding="utf-8"))
        create_listener_functions = [
            node
            for node in tree.body
            if isinstance(node, ast.FunctionDef)
            and node.name == "create_listener"
        ]
        self.assertEqual(len(create_listener_functions), 1)

        socket_options = []
        for node in ast.walk(create_listener_functions[0]):
            if (
                isinstance(node, ast.Call)
                and isinstance(node.func, ast.Attribute)
                and isinstance(node.func.value, ast.Name)
                and node.func.value.id == "listener"
                and node.func.attr == "setsockopt"
            ):
                self.assertEqual(len(node.args), 3)
                socket_options.append(
                    (
                        ast.unparse(node.args[0]),
                        ast.unparse(node.args[1]),
                        ast.literal_eval(node.args[2]),
                    )
                )

        self.assertCountEqual(
            socket_options,
            [
                ("socket.SOL_SOCKET", "socket.SO_REUSEADDR", 1),
                ("socket.SOL_SOCKET", "socket.SO_KEEPALIVE", 1),
                ("socket.IPPROTO_TCP", "socket.TCP_KEEPIDLE", 60),
                ("socket.IPPROTO_TCP", "socket.TCP_KEEPINTVL", 10),
                ("socket.IPPROTO_TCP", "socket.TCP_KEEPCNT", 3),
            ],
        )

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
