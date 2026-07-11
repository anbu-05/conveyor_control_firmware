from __future__ import annotations

import queue
import re
import threading
import time
from collections import deque
from dataclasses import asdict, dataclass, field
from typing import Any

try:
    import serial
except ImportError as exc:  # pragma: no cover - exercised only without dependency installed
    raise ImportError("pyserial is required. Install it with: python3 -m pip install -r driver/requirements.txt") from exc


DEFAULT_SERIAL_PORT = "/dev/ttyACM0"
DEFAULT_SERIAL_BAUD = 115200
MAX_COMMAND_LENGTH = 160
DEFAULT_MOTOR_ID = "M0"
MIN_COMMAND_INTERVAL_SECONDS = 0.18

CONFIG_LIMITS: dict[str, tuple[int, int]] = {
    "pid_kp_milli": (0, 100000),
    "pid_ki_milli": (0, 100000),
    "pid_kd_milli": (0, 100000),
    "max_pwm": (0, 255),
    "max_speed_counts_per_sec": (0, 100000),
    "position_tolerance_counts": (0, 100000),
}

MOTOR_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_-]*$")
KEY_VALUE_RE = re.compile(r"(?P<key>[A-Za-z0-9_]+)=(?P<value>\S+)")
ANSI_RE = re.compile(r"\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07]*(?:\x07|\x1b\\)|[@-Z\\-_])")


@dataclass
class SerialSnapshot:
    connected: bool = False
    port: str = DEFAULT_SERIAL_PORT
    baud: int = DEFAULT_SERIAL_BAUD
    last_line: str = ""
    last_error: str = ""
    ready: bool = False
    job_state: str = "UNKNOWN"
    motor_id: str = DEFAULT_MOTOR_ID
    motor_ids: list[str] = field(default_factory=lambda: [DEFAULT_MOTOR_ID])
    motor: dict[str, Any] = field(default_factory=dict)
    position: dict[str, Any] = field(default_factory=dict)
    sensors: dict[str, Any] = field(default_factory=dict)
    config: dict[str, str] = field(default_factory=dict)
    status: dict[str, str] = field(default_factory=dict)
    commands: list[str] = field(default_factory=list)
    last_result: dict[str, Any] = field(default_factory=dict)
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
        self._lock = threading.RLock()
        self._write_lock = threading.Lock()
        self._sent_lines: deque[str] = deque(maxlen=20)
        self._last_send_monotonic = 0.0

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
        self._reader = threading.Thread(target=self._read_loop, name="conveyor-driver-serial-reader", daemon=True)
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
            self.snapshot.last_update_monotonic = time.monotonic()
        if emit and was_connected:
            self._emit("status", "rx", "Serial disconnected")

    def command(self, name: str, args: list[str] | None = None) -> str:
        line = self._build_command(name, args or [])
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

        # ESP-IDF console is configured with ESP_LINE_ENDINGS_CR on RX and is
        # served through linenoise. Ctrl-U clears any partial prompt input, then
        # CR submits the command as a terminal would. Sending LF only can leave
        # linenoise with mangled commands such as "tatus" or "us".
        payload = f"\x15{line}\r".encode("utf-8")
        with self._write_lock:
            try:
                elapsed = time.monotonic() - self._last_send_monotonic
                if elapsed < MIN_COMMAND_INTERVAL_SECONDS:
                    time.sleep(MIN_COMMAND_INTERVAL_SECONDS - elapsed)
                serial_port.write(payload)
                serial_port.flush()
                self._last_send_monotonic = time.monotonic()
            except serial.SerialException as exc:
                with self._lock:
                    self.snapshot.last_error = str(exc)
                self._emit("error", "rx", f"Serial write failed: {exc}")
                raise RuntimeError(f"serial write failed: {exc}") from exc
        self._sent_lines.append(line)
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
            buffer = self._answer_terminal_queries(buffer)
            while True:
                newline_index = buffer.find(b"\n")
                carriage_index = buffer.find(b"\r")
                indexes = [index for index in (newline_index, carriage_index) if index >= 0]
                if not indexes:
                    break
                split_index = min(indexes)
                raw_line = buffer[:split_index]
                buffer = buffer[split_index + 1:]
                line = self._clean_console_line(raw_line.decode("utf-8", errors="replace"))
                if line:
                    self._handle_line(line)

    def _answer_terminal_queries(self, buffer: bytearray) -> bytearray:
        # ESP-IDF linenoise can ask the attached terminal for cursor position
        # with DSR (ESC[6n). A normal terminal answers ESC[row;colR. Without an
        # answer, linenoise may consume subsequent command bytes as the pending
        # response, which mangles commands such as stopmotor into UNKNOWN_COMMAND.
        query = b"\x1b[6n"
        response = b"\x1b[1;1R"
        while query in buffer:
            index = buffer.index(query)
            del buffer[index:index + len(query)]
            serial_port = self._serial
            if serial_port is not None and self.snapshot.connected:
                with self._write_lock:
                    try:
                        serial_port.write(response)
                        serial_port.flush()
                    except serial.SerialException as exc:
                        with self._lock:
                            self.snapshot.last_error = str(exc)
                        self._emit("error", "rx", f"Terminal response failed: {exc}")
                        break
        return buffer

    def _handle_line(self, line: str) -> None:
        if self._is_console_noise(line):
            return
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
        with self._lock:
            if token == "OK":
                return self._parse_ok(parts)
            if token == "CONFIG" and len(parts) >= 3:
                value = " ".join(parts[2:])
                self.snapshot.config[parts[1]] = value
                return {"type": "config", "key": parts[1], "value": value}
            if token == "STATUS" and len(parts) >= 2:
                return self._parse_status(parts)
            if token == "COMMAND" and len(parts) >= 2:
                command = parts[1]
                text = " ".join(parts[1:])
                if not any(existing.split(" ", 1)[0] == command for existing in self.snapshot.commands):
                    self.snapshot.commands.append(text)
                return {"type": "command_info", "command": command, "text": text}
            if token == "MOTOR" and len(parts) >= 2:
                self._set_motor_id(parts[1])
                return {"type": "motor_info", "motor_id": parts[1]}
            if token == "ERR":
                return {"type": "error", "code": parts[1] if len(parts) > 1 else "UNKNOWN"}
            if line.startswith("READY"):
                self.snapshot.ready = True
                return {"type": "ready"}
        return None

    def _clean_console_line(self, line: str) -> str:
        line = ANSI_RE.sub("", line)
        line = "".join(ch for ch in line if ch == "\t" or ord(ch) >= 32)
        line = line.strip()
        while line.startswith(">"):
            line = line[1:].strip()
        return line

    def _is_console_noise(self, line: str) -> bool:
        if not line or line in {">", "^U"}:
            return True
        if line in {"R", "1R", ";1R"}:
            return True
        if line in self._sent_lines:
            return True
        if line.startswith("^U") and line[2:].strip() in self._sent_lines:
            return True
        if any(sent.endswith(line) and len(line) < len(sent) for sent in self._sent_lines):
            return True
        return False

    def _parse_ok(self, parts: list[str]) -> dict[str, Any]:
        command = parts[1] if len(parts) > 1 else ""
        values = self._key_values(parts[2:])
        data: dict[str, Any] = {"type": "ok", "command": command, "values": values}

        if command == "STATUS" and "state" in values:
            self.snapshot.job_state = values["state"]
            data["state"] = values["state"]
        elif command == "POSITION":
            self._merge_position(values)
            data.update({"type": "position", **values})
        elif command == "SENSORS":
            self._merge_sensors(values)
            data.update({"type": "sensors", **values})
        elif command == "SETMOTOR":
            self._merge_motor(values)
            data.update({"type": "motor", **values})
        elif command in {"STOP", "STOPMOTOR"}:
            self.snapshot.motor["pwm"] = 0
            data["type"] = "stop"
        elif command == "SETPOSITION":
            self._merge_position(values)
            data.update({"type": "setposition", **values})
        elif command == "POSITIONCONTROL":
            self._merge_position_control(values)
            data.update({"type": "positioncontrol", **values})
        elif command == "SETOFFSET":
            self._merge_position(values)
            data.update({"type": "setoffset", **values})
        elif command in {"JOBRX", "JOBTX"} and "result" in values:
            self.snapshot.job_state = values["result"]
            data.update({"type": command.lower(), "result": values["result"]})
        elif command == "SETCONFIG" and len(parts) >= 4:
            self.snapshot.config[parts[2]] = parts[3]
            data.update({"type": "setconfig", "key": parts[2], "value": parts[3]})
        elif command == "RESETCONFIG" and len(parts) >= 3:
            data.update({"type": "resetconfig", "key": parts[2]})

        self.snapshot.last_error = ""
        self.snapshot.last_result = data
        return data

    def _parse_status(self, parts: list[str]) -> dict[str, Any]:
        values = self._key_values(parts[1:])
        self.snapshot.status.update(values)
        return {"type": "status", "values": values}

    def _merge_motor(self, values: dict[str, str]) -> None:
        if "motor" in values:
            self._set_motor_id(values["motor"])
        for key in ("pwm", "dir"):
            if key in values:
                self.snapshot.motor[key if key != "dir" else "direction"] = self._to_int(values[key])

    def _merge_position(self, values: dict[str, str]) -> None:
        if "motor" in values:
            self._set_motor_id(values["motor"])
        for key in ("pos", "position", "offset"):
            if key in values:
                self.snapshot.position[key] = self._to_int(values[key])

    def _merge_position_control(self, values: dict[str, str]) -> None:
        if "motor" in values:
            self._set_motor_id(values["motor"])
        if "enabled" in values:
            self.snapshot.position["position_control"] = self._to_bool_int(values["enabled"])

    def _merge_sensors(self, values: dict[str, str]) -> None:
        if "motor" in values:
            self._set_motor_id(values["motor"])
        for key in ("upstream", "downstream"):
            if key in values:
                self.snapshot.sensors[key] = self._to_int(values[key])
        upstream = self.snapshot.sensors.get("upstream")
        downstream = self.snapshot.sensors.get("downstream")
        if upstream is not None or downstream is not None:
            self.snapshot.sensors["has_tray"] = upstream == 0 or downstream == 0

    def _set_motor_id(self, motor_id: str) -> None:
        self.snapshot.motor_id = motor_id
        if motor_id not in self.snapshot.motor_ids:
            self.snapshot.motor_ids.append(motor_id)

    @staticmethod
    def _key_values(parts: list[str]) -> dict[str, str]:
        values: dict[str, str] = {}
        for part in parts:
            match = KEY_VALUE_RE.fullmatch(part)
            if match:
                values[match.group("key")] = match.group("value")
        return values

    def _build_command(self, name: str, args: list[str]) -> str:
        name = name.strip().lower()
        if name == "setmotor":
            self._expect_args(name, args, 3)
            motor_id = self._parse_motor_id(args[0])
            pwm = self._parse_int(args[1], "pwm")
            direction = self._parse_int(args[2], "direction")
            if pwm < 0 or pwm > 255:
                raise ValueError("pwm must be between 0 and 255")
            if direction not in {0, 1}:
                raise ValueError("direction must be 0 or 1")
            return f"setmotor {motor_id} {pwm} {direction}"
        if name == "stop":
            self._expect_args(name, args, 0)
            return "stop"
        if name == "stopmotor":
            self._expect_args(name, args, 1)
            return f"stopmotor {self._parse_motor_id(args[0])}"
        if name in {"setposition", "setoffset"}:
            self._expect_args(name, args, 2)
            return f"{name} {self._parse_motor_id(args[0])} {self._parse_int(args[1], name)}"
        if name in {"getposition", "getsensors"}:
            self._expect_args(name, args, 1)
            return f"{name} {self._parse_motor_id(args[0])}"
        if name == "positioncontrol":
            self._expect_args(name, args, 2)
            enabled = self._parse_int(args[1], "enabled")
            if enabled not in {0, 1}:
                raise ValueError("enabled must be 0 or 1")
            return f"positioncontrol {self._parse_motor_id(args[0])} {enabled}"
        if name == "getconfig":
            if len(args) == 0:
                return "getconfig"
            self._expect_args(name, args, 1)
            return f"getconfig {self._parse_config_key(args[0])}"
        if name == "setconfig":
            self._expect_args(name, args, 2)
            key = self._parse_config_key(args[0])
            value = self._parse_int(args[1], key)
            low, high = CONFIG_LIMITS[key]
            if value < low or value > high:
                raise ValueError(f"{key} must be between {low} and {high}")
            return f"setconfig {key} {value}"
        if name == "resetconfig":
            self._expect_args(name, args, 1)
            return f"resetconfig {self._parse_config_key(args[0])}"
        if name in {"jobrx", "jobtx", "getstatus", "status"}:
            self._expect_args(name, args, 0)
            return name
        raise ValueError(f"unsupported serial command: {name}")

    def _emit(self, kind: str, direction: str, message: str, data: dict[str, Any] | None = None) -> None:
        self.events.put(SerialEvent(kind=kind, direction=direction, message=message, data=data, timestamp=time.time()))

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
    def _parse_motor_id(value: str) -> str:
        value = value.strip()
        if not MOTOR_RE.fullmatch(value):
            raise ValueError("motor id must be alphanumeric, starting with a letter")
        return value

    @staticmethod
    def _parse_config_key(value: str) -> str:
        value = value.strip()
        if value not in CONFIG_LIMITS:
            raise ValueError("unknown config key")
        return value

    @staticmethod
    def _parse_int(value: str, label: str) -> int:
        try:
            return int(value, 10)
        except ValueError as exc:
            raise ValueError(f"{label} must be an integer") from exc

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
