from __future__ import annotations

import socket

import uvicorn

from env_api import app


LISTEN_ADDRESSES = ("100.117.242.46", "192.168.0.46")
PORT = 8080
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

    config = uvicorn.Config(app=app, log_level="info")
    uvicorn.Server(config).run(sockets=listeners)


if __name__ == "__main__":
    main()
