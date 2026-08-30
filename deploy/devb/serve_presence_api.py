from __future__ import annotations

import os
import socket

import uvicorn
from presence_api.main import app

LISTEN_ADDRESSES = tuple(
    address.strip()
    for address in os.getenv(
        "PRESENCE_LISTEN_ADDRESSES",
        "100.117.242.46,192.168.0.46",
    ).split(",")
    if address.strip()
)
PORT = int(os.getenv("PRESENCE_PORT", "8081"))
BACKLOG = 2048


def create_listener(host: str) -> socket.socket:
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        listener.bind((host, PORT))
        listener.listen(BACKLOG)
    except Exception:
        listener.close()
        raise
    return listener


def main() -> None:
    listeners: list[socket.socket] = []
    try:
        listeners = [create_listener(host) for host in LISTEN_ADDRESSES]
    except Exception:
        for listener in listeners:
            listener.close()
        raise

    uvicorn.Server(uvicorn.Config(app=app, log_level="info", proxy_headers=False)).run(
        sockets=listeners
    )


if __name__ == "__main__":
    main()
