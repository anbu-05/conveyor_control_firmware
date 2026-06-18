from __future__ import annotations

import argparse

DEFAULT_MQTT_HOST = "192.168.1.126"
DEFAULT_MQTT_PORT = 1883
DEFAULT_CONVEYOR_ID = "C0"
DEFAULT_SERIAL_PORT = "/dev/ttyACM0"
DEFAULT_SERIAL_BAUD = 115200


def main() -> None:
    parser = argparse.ArgumentParser(description="Conveyor browser MQTT and serial controller")
    parser.add_argument("--host", default="127.0.0.1", help="HTTP server host")
    parser.add_argument("--port", default=8080, type=int, help="HTTP server port")
    parser.add_argument("--mqtt-host", default=DEFAULT_MQTT_HOST, help="Default MQTT broker host shown in the UI")
    parser.add_argument("--mqtt-port", default=DEFAULT_MQTT_PORT, type=int, help="Default MQTT broker port shown in the UI")
    parser.add_argument("--id", default=DEFAULT_CONVEYOR_ID, help="Default conveyor ID shown in the UI")
    parser.add_argument("--serial-port", default=DEFAULT_SERIAL_PORT, help="Default serial port shown in the UI")
    parser.add_argument("--serial-baud", default=DEFAULT_SERIAL_BAUD, type=int, help="Default serial baud shown in the UI")
    args = parser.parse_args()

    import uvicorn

    from . import server

    server.backend.snapshot.mqtt_host = args.mqtt_host
    server.backend.snapshot.mqtt_port = args.mqtt_port
    server.backend.snapshot.conveyor_id = args.id
    server.backend.topics = server.backend.topics.__class__(conveyor_id=args.id)
    server.serial_backend.snapshot.port = args.serial_port
    server.serial_backend.snapshot.baud = args.serial_baud

    uvicorn.run(server.app, host=args.host, port=args.port)


if __name__ == "__main__":
    main()
