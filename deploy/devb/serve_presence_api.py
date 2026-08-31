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
    try:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        # Linux accepted sockets inherit these listener options. Bound stale
        # peers that stop acknowledging while Uvicorn is still waiting for an
        # incomplete request body; this is not an HTTP request-body timeout.
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        listener.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPIDLE, 60)
        listener.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPINTVL, 10)
        listener.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPCNT, 3)
        listener.bind((host, PORT))
        listener.listen(BACKLOG)
    except Exception:
        listener.close()
        raise
    return listener


def main() -> None:
    listeners: list[socket.socket] = []
    try:
        for host in LISTEN_ADDRESSES:
            listeners.append(create_listener(host))
    except Exception:
        for listener in listeners:
            listener.close()
        raise

    uvicorn.Server(
        uvicorn.Config(
            app=app,
            log_level="info",
            proxy_headers=False,
            # The device keeps separate HTTP/1.1 sessions for five-second
            # bounded operations and buffered telemetry batches. The normal
            # telemetry cadence is thirty seconds, so keep both server sides
            # open well beyond that interval rather than racing the idle edge.
            timeout_keep_alive=90,
            # A persistent device connection must not hold a deployment in
            # graceful shutdown until systemd's much longer stop timeout.
            timeout_graceful_shutdown=30,
        )
    ).run(sockets=listeners)


if __name__ == "__main__":
    main()
