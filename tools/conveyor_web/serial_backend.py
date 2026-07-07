from __future__ import annotations

import queue
import threading
import time
from dataclasses import asdict, dataclass, field
from typing import Any

try:
    import serial
except ImportError as exc:  # pragma: no cover - exercised only without dependency installed
    raise ImportError(
        "pyserial is required. Install it with: python3 -m pip install -r tools/conveyor_web/requirements.txt"
    ) from exc


DEFAULT_SERIAL_PORT = "/dev/ttyACM0"
DEFAULT_SERIAL_BAUD = 115200
MAX_COMMAND_LENGTH = 120

CONFIG_LIMITS: dict[str, tuple[int, int]] = {
    "run_pwm": (0, 255),
    "run_speed_counts_per_sec": (0, 100000),
    "speed_kp_milli": (0, 100000),
    "speed_kd_milli": (0, 100000),
    "done_hold_ms": (0, 60000),
    "tx_detect_timeout_ms": (1, 600000),
    "tx_clear_timeout_ms": (1, 600000),
    "rx_detect_timeout_ms": (1, 600000),
    "rx_done_timeout_ms": (1, 600000),
    "mqtt_status_period_ms": (100, 60000),
}

STRING_CONFIG_LIMITS: dict[str, tuple[int, int]] = {
    "wifi_ssid": (1, 31),
    "wifi_pass": (1, 63),
    "conveyor_id": (1, 31),
    "mqtt_broker_uri": (1, 127),
    "mqtt_topic_cmd": (1, 95),
    "mqtt_topic_emergency": (1, 95),
    "mqtt_topic_feedback": (1, 95),
    "mqtt_topic_all_emergency": (1, 95),
    "mqtt_topic_tray": (1, 95),
}


@dataclass
class SerialSnapshot:
    connected: bool = False
    port: str = DEFAULT_SERIAL_PORT
    baud: int = DEFAULT_SERIAL_BAUD
    last_line: str = ""
    last_error: str = ""
    ready: bool = False
    job_state: str = "UNKNOWN"
    direction: str = "UNKNOWN"
    rssi: int | None = None
    sensor_watch: bool = False
    encoder_watch: bool = False
    motor: dict[str, Any] = field(default_factory=dict)
    encoder: dict[str, Any] = field(default_factory=dict)
    tray: dict[str, Any] = field(default_factory=dict)
    config: dict[str, str] = field(default_factory=dict)
    last_update_monotonic: float | None = None


@dataclass(frozen=True)
class SerialEvent:
    kind: str
    direction: str
    message: str
    data: dict[str, Any] | None = None
    timestamp: float = 0.0

    def as_dict(self) -> dict[str, Any]:
        return asdict(self)


class SerialBackend:
    def __init__(self) -> None:
        self.events: queue.Queue[SerialEvent] = queue.Queue()
        self.snapshot = SerialSnapshot()
        self._serial: serial.Serial | None = None
        self._reader: threading.Thread | None = None
        self._stop_reader = threading.Event()
        self._lock = threading.Lock()
        self._write_lock = threading.Lock()

    def snapshot_dict(self) -> dict[str, Any]:
        with self._lock:
            return asdict(self.snapshot)

    def connect(self, port: str, baud: int) -> None:
        port = port.strip() or DEFAULT_SERIAL_PORT
        if baud < 1 or baud > 3000000:
            raise ValueError("baud must be between 1 and 3000000")

        self.disconnect(emit=False)
        try:
            serial_port = serial.Serial(port=port, baudrate=baud, timeout=0.1, write_timeout=1)
        except serial.SerialException as exc:
            with self._lock:
                self.snapshot.port = port
                self.snapshot.baud = baud
                self.snapshot.connected = False
                self.snapshot.last_error = str(exc)
            raise RuntimeError(f"serial connect failed: {exc}") from exc

        try:
            serial_port.reset_input_buffer()
        except serial.SerialException:
            pass

        self._serial = serial_port
        self._stop_reader.clear()
        with self._lock:
            self.snapshot.connected = True
            self.snapshot.port = port
            self.snapshot.baud = baud
            self.snapshot.last_error = ""
            self.snapshot.last_update_monotonic = time.monotonic()
        self._reader = threading.Thread(target=self._read_loop, name="conveyor-serial-reader", daemon=True)
        self._reader.start()
        self._emit("status", "rx", f"Serial connected to {port} at {baud}")

    def disconnect(self, emit: bool = True) -> None:
        serial_port = self._serial
        self._serial = None
        self._stop_reader.set()

        if serial_port is not None:
            try:
                serial_port.close()
            except serial.SerialException:
                pass

        if self._reader is not None and self._reader.is_alive():
            self._reader.join(timeout=0.5)
        self._reader = None

        with self._lock:
            was_connected = self.snapshot.connected
            self.snapshot.connected = False
            self.snapshot.sensor_watch = False
            self.snapshot.encoder_watch = False
            self.snapshot.last_update_monotonic = time.monotonic()
        if emit and was_connected:
            self._emit("status", "rx", "Serial disconnected")

    def command(self, name: str, args: list[str] | None = None) -> str:
        args = args or []
        line = self._build_command(name, args)
        self._send_line(line)
        return line

    def raw(self, line: str) -> str:
        line = line.strip()
        self._validate_line(line)
        self._send_line(line)
        return line

    def _send_line(self, line: str) -> None:
        serial_port = self._serial
        if serial_port is None or not self.snapshot.connected:
            self._emit("error", "rx", "Serial is not connected; command was not sent")
            raise RuntimeError("serial is not connected")

        payload = f"{line}\n".encode("utf-8")
        with self._write_lock:
            try:
                serial_port.write(payload)
                serial_port.flush()
            except serial.SerialException as exc:
                with self._lock:
                    self.snapshot.last_error = str(exc)
                self._emit("error", "rx", f"Serial write failed: {exc}")
                raise RuntimeError(f"serial write failed: {exc}") from exc
        self._emit("command", "tx", line)

    def _read_loop(self) -> None:
        buffer = bytearray()
        while not self._stop_reader.is_set():
            serial_port = self._serial
            if serial_port is None:
                return
            try:
                chunk = serial_port.read(128)
            except serial.SerialException as exc:
                with self._lock:
                    self.snapshot.connected = False
                    self.snapshot.last_error = str(exc)
                self._emit("error", "rx", f"Serial read failed: {exc}")
                return

            if not chunk:
                continue
            buffer.extend(chunk)
            while b"\n" in buffer:
                raw_line, _, buffer = buffer.partition(b"\n")
                line = raw_line.decode("utf-8", errors="replace").strip("\r")
                if line:
                    self._handle_line(line)

    def _handle_line(self, line: str) -> None:
        data = self._parse_line(line)
        kind = "error" if line.startswith("ERR ") else "message"
        with self._lock:
            self.snapshot.last_line = line
            self.snapshot.last_update_monotonic = time.monotonic()
            if line.startswith("ERR "):
                self.snapshot.last_error = line
        self._emit(kind, "rx", line, data)

    def _parse_line(self, line: str) -> dict[str, Any] | None:
        parts = line.split()
        if not parts:
            return None

        token = parts[0]
        data: dict[str, Any] | None = None
        with self._lock:
            if line == "READY conveyor":
                self.snapshot.ready = True
                data = {"type": "ready"}
            elif token == "OK":
                self._parse_ok(parts)
                data = {"type": "ok", "command": " ".join(parts[1:])}
            elif token == "ERR":
                data = {"type": "error", "code": parts[1] if len(parts) > 1 else "UNKNOWN"}
            elif token == "EVENT" and len(parts) >= 2:
                data = self._parse_event(parts)
            elif token == "ENCODER" and len(parts) == 5:
                data = self._parse_encoder(parts)
            elif token == "MOTOR" and len(parts) == 8:
                data = self._parse_motor(parts)
            elif token == "TRAY" and len(parts) == 5:
                data = self._parse_tray(parts)
            elif token == "CONFIG" and len(parts) == 3:
                self.snapshot.config[parts[1]] = parts[2]
                data = {"type": "config", "key": parts[1], "value": parts[2]}
            elif token == "DIRECTION" and len(parts) == 3:
                self.snapshot.direction = parts[2]
                data = {"type": "direction", "conveyor_id": parts[1], "direction": parts[2]}
            elif token == "RSSI" and len(parts) == 3:
                rssi = self._to_int(parts[2])
                self.snapshot.rssi = rssi
                data = {"type": "rssi", "conveyor_id": parts[1], "rssi": rssi}
        return data

    def _parse_ok(self, parts: list[str]) -> None:
        if len(parts) >= 3 and parts[1] == "WATCHSENSORS":
            self.snapshot.sensor_watch = parts[2] == "ON"
        if len(parts) >= 4 and parts[1] == "WATCHENCODER":
            self.snapshot.encoder_watch = parts[3] == "ON"
        if len(parts) >= 3 and parts[1] in {"SETKP", "SETKD"}:
            key = "speed_kp" if parts[1] == "SETKP" else "speed_kd"
            self.snapshot.config[key] = parts[2]
        if len(parts) >= 4 and parts[1] == "SETCONFIG":
            self.snapshot.config[parts[2]] = parts[3]

    def _parse_event(self, parts: list[str]) -> dict[str, Any] | None:
        if len(parts) == 5 and parts[1] == "SENSOR":
            sensor = parts[2]
            old = self._to_int(parts[3])
            new = self._to_int(parts[4])
            if sensor == "S0":
                self.snapshot.tray["s0"] = new
            if sensor == "S1":
                self.snapshot.tray["s1"] = new
            s0 = self.snapshot.tray.get("s0")
            s1 = self.snapshot.tray.get("s1")
            if s0 is not None or s1 is not None:
                self.snapshot.tray["has_tray"] = s0 == 0 or s1 == 0
            return {"type": "sensor", "sensor": sensor, "old": old, "new": new}
        if len(parts) == 5 and parts[1] == "ENCODER":
            count = self._to_int(parts[3])
            speed = self._to_int(parts[4])
            self.snapshot.encoder = {"motor": parts[2], "count": count, "speed": speed}
            return {"type": "encoder_event", "motor": parts[2], "count": count, "speed": speed}
        if len(parts) == 4 and parts[1] == "JOB":
            self.snapshot.job_state = parts[3]
            return {"type": "job", "conveyor_id": parts[2], "state": parts[3]}
        return {"type": "event", "parts": parts[1:]}

    def _parse_encoder(self, parts: list[str]) -> dict[str, Any]:
        self.snapshot.encoder = {
            "motor": parts[1],
            "count": self._to_int(parts[2]),
            "gpio17_a": self._to_int(parts[3]),
            "gpio18_b": self._to_int(parts[4]),
        }
        return {"type": "encoder", **self.snapshot.encoder}

    def _parse_motor(self, parts: list[str]) -> dict[str, Any]:
        self.snapshot.motor = {
            "motor": parts[1],
            "pwm": self._to_int(parts[2]),
            "direction": self._to_int(parts[3]),
            "position": self._to_int(parts[4]),
            "target_speed": self._to_int(parts[5]),
            "current_speed": self._to_int(parts[6]),
            "speed_control": self._to_int(parts[7]),
        }
        return {"type": "motor", **self.snapshot.motor}

    def _parse_tray(self, parts: list[str]) -> dict[str, Any]:
        self.snapshot.tray = {
            "conveyor_id": parts[1],
            "has_tray": self._to_bool_int(parts[2]),
            "s0": self._to_int(parts[3]),
            "s1": self._to_int(parts[4]),
        }
        return {"type": "tray", **self.snapshot.tray}

    def _build_command(self, name: str, args: list[str]) -> str:
        if name == "setmotor":
            self._expect_args(name, args, 3)
            self._expect_motor(args[0])
            pwm = self._parse_int(args[1], "pwm")
            direction = self._parse_int(args[2], "direction")
            if pwm < 0 or pwm > 255:
                raise ValueError("pwm must be between 0 and 255")
            if direction not in {0, 1}:
                raise ValueError("direction must be 0 or 1")
            return f"setmotor {args[0]} {pwm} {direction}"
        if name == "stopmotor":
            self._expect_args(name, args, 1)
            self._expect_motor(args[0])
            return f"stopmotor {args[0]}"
        if name == "setspeed":
            self._expect_args(name, args, 2)
            self._expect_motor(args[0])
            speed = self._parse_int(args[1], "speed")
            if speed < -100000 or speed > 100000:
                raise ValueError("speed must be between -100000 and 100000")
            return f"setspeed {args[0]} {speed}"
        if name in {"setkp", "setkd"}:
            self._expect_args(name, args, 1)
            return f"{name} {self._format_gain(args[0])}"
        if name == "resetk":
            self._expect_args(name, args, 0)
            return "resetk"
        if name == "stop":
            self._expect_args(name, args, 0)
            return "stop"
        if name == "watchsensors":
            self._expect_args(name, args, 1)
            if args[0] not in {"on", "off"}:
                raise ValueError("watchsensors mode must be on or off")
            return f"watchsensors {args[0]}"
        if name == "watchencoder":
            self._expect_args(name, args, 2)
            self._expect_motor(args[0])
            if args[1] not in {"on", "off"}:
                raise ValueError("watchencoder mode must be on or off")
            return f"watchencoder {args[0]} {args[1]}"
        if name in {"getencoder", "getmotor"}:
            self._expect_args(name, args, 1)
            self._expect_motor(args[0])
            return f"{name} {args[0]}"
        if name in {"gettray", "getconfig", "resetconfig", "jobtx", "jobrx", "estop", "clearerror", "getdirection", "getrssi"}:
            if name == "getconfig" and len(args) == 1:
                self._expect_config_key(args[0])
                return f"getconfig {args[0]}"
            self._expect_args(name, args, 0)
            return name
        if name == "setconfig":
            self._expect_args(name, args, 2)
            self._expect_config_key(args[0])
            if args[0] in STRING_CONFIG_LIMITS:
                value = args[1].strip()
                low, high = STRING_CONFIG_LIMITS[args[0]]
                if len(value) < low or len(value) > high:
                    raise ValueError(f"{args[0]} must be {low} to {high} characters")
                if any(ch.isspace() for ch in value):
                    raise ValueError(f"{args[0]} must not contain spaces")
                return f"setconfig {args[0]} {value}"

            value = self._parse_int(args[1], "config value")
            low, high = CONFIG_LIMITS[args[0]]
            if value < low or value > high:
                raise ValueError(f"{args[0]} must be between {low} and {high}")
            return f"setconfig {args[0]} {value}"
        if name == "setdirection":
            self._expect_args(name, args, 1)
            if args[0] not in {"s0tos1", "s1tos0"}:
                raise ValueError("direction must be s0tos1 or s1tos0")
            return f"setdirection {args[0]}"
        raise ValueError(f"unsupported serial command: {name}")

    def _emit(self, kind: str, direction: str, message: str, data: dict[str, Any] | None = None) -> None:
        self.events.put(
            SerialEvent(kind=kind, direction=direction, message=message, data=data, timestamp=time.time())
        )

    @staticmethod
    def _validate_line(line: str) -> None:
        if not line:
            raise ValueError("command line is required")
        if "\n" in line or "\r" in line:
            raise ValueError("command line must be a single line")
        if len(line) > MAX_COMMAND_LENGTH:
            raise ValueError(f"command line must be {MAX_COMMAND_LENGTH} characters or less")

    @staticmethod
    def _expect_args(name: str, args: list[str], count: int) -> None:
        if len(args) != count:
            raise ValueError(f"{name} expects {count} argument(s)")

    @staticmethod
    def _expect_motor(value: str) -> None:
        if value != "M0":
            raise ValueError("motor must be M0")

    @staticmethod
    def _expect_config_key(value: str) -> None:
        if value not in CONFIG_LIMITS and value not in STRING_CONFIG_LIMITS:
            raise ValueError("unknown config key")

    @staticmethod
    def _parse_int(value: str, label: str) -> int:
        try:
            return int(value, 10)
        except ValueError as exc:
            raise ValueError(f"{label} must be an integer") from exc

    @staticmethod
    def _format_gain(value: str) -> str:
        try:
            parsed = float(value)
        except ValueError as exc:
            raise ValueError("gain must be a decimal number") from exc
        if parsed < 0.0 or parsed > 100.0:
            raise ValueError("gain must be between 0.000 and 100.000")
        return f"{parsed:.3f}"

    @staticmethod
    def _to_int(value: str) -> int | None:
        try:
            return int(value, 10)
        except ValueError:
            return None

    @staticmethod
    def _to_bool_int(value: str) -> bool | None:
        parsed = SerialBackend._to_int(value)
        if parsed is None:
            return None
        return bool(parsed)
