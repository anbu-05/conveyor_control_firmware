from __future__ import annotations

import argparse

from .serial_backend import DEFAULT_SERIAL_BAUD, DEFAULT_SERIAL_PORT


def main() -> None:
    parser = argparse.ArgumentParser(description="Conveyor browser serial debug driver")
    parser.add_argument("--host", default="127.0.0.1", help="HTTP server host")
    parser.add_argument("--port", default=8080, type=int, help="HTTP server port")
    parser.add_argument("--serial-port", default=DEFAULT_SERIAL_PORT, help="Default serial port shown in the UI")
    parser.add_argument("--serial-baud", default=DEFAULT_SERIAL_BAUD, type=int, help="Default serial baud shown in the UI")
    args = parser.parse_args()

    import uvicorn

    from . import server

    server.serial_backend.snapshot.port = args.serial_port
    server.serial_backend.snapshot.baud = args.serial_baud

    uvicorn.run(server.app, host=args.host, port=args.port)


if __name__ == "__main__":
    main()
