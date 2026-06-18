from __future__ import annotations

import json
import queue
import time
from dataclasses import asdict, dataclass
from typing import Any

try:
    import paho.mqtt.client as mqtt
except ImportError as exc:  # pragma: no cover - exercised only without dependency installed
    raise ImportError(
        "paho-mqtt is required. Install it with: python3 -m pip install -r tools/conveyor_web/requirements.txt"
    ) from exc


DEFAULT_MQTT_HOST = "192.168.1.126"
DEFAULT_MQTT_PORT = 1883
DEFAULT_CONVEYOR_ID = "C0"


@dataclass(frozen=True)
class ConveyorTopics:
    conveyor_id: str = DEFAULT_CONVEYOR_ID

    @property
    def command(self) -> str:
        return f"conveyor/{self.conveyor_id}/cmd"

    @property
    def emergency(self) -> str:
        return f"conveyor/{self.conveyor_id}/emergency"

    @property
    def all_emergency(self) -> str:
        return "conveyor/all/emergency"

    @property
    def feedback(self) -> str:
        return f"conveyor/{self.conveyor_id}/feedback"

    @property
    def tray(self) -> str:
        return f"conveyor/{self.conveyor_id}/tray"

    def as_dict(self) -> dict[str, str]:
        return {
            "command": self.command,
            "emergency": self.emergency,
            "all_emergency": self.all_emergency,
            "feedback": self.feedback,
            "tray": self.tray,
        }


@dataclass
class ConveyorSnapshot:
    connected: bool = False
    mqtt_host: str = DEFAULT_MQTT_HOST
    mqtt_port: int = DEFAULT_MQTT_PORT
    conveyor_id: str = DEFAULT_CONVEYOR_ID
    state: str = "UNKNOWN"
    state_elapsed_ms: int | None = None
    error: str = ""
    has_tray: bool | None = None
    s0: int | None = None
    s1: int | None = None
    direction: str = "UNKNOWN"
    rssi: int | None = None
    speed_kp: str = ""
    speed_kd: str = ""
    last_topic: str = ""
    last_payload: str = ""
    last_update_monotonic: float | None = None


@dataclass(frozen=True)
class ConveyorEvent:
    kind: str
    message: str
    topic: str = ""
    payload: str = ""
    data: dict[str, Any] | None = None
    timestamp: float = 0.0

    def as_dict(self) -> dict[str, Any]:
        return asdict(self)


class ConveyorMqttBackend:
    def __init__(self) -> None:
        self.events: queue.Queue[ConveyorEvent] = queue.Queue()
        self.snapshot = ConveyorSnapshot()
        self.topics = ConveyorTopics()
        self._client: mqtt.Client | None = None

    def snapshot_dict(self) -> dict[str, Any]:
        data = asdict(self.snapshot)
        data["topics"] = self.topics.as_dict()
        return data

    def connect(self, host: str, port: int, conveyor_id: str) -> None:
        self.disconnect(emit=False)
        self.topics = ConveyorTopics(conveyor_id=conveyor_id)
        self.snapshot.mqtt_host = host
        self.snapshot.mqtt_port = port
        self.snapshot.conveyor_id = conveyor_id

        client_id = f"conveyor_web_{conveyor_id}_{int(time.time())}"
        client = self._make_client(client_id)
        client.on_connect = self._on_connect
        client.on_disconnect = self._on_disconnect
        client.on_message = self._on_message
        self._client = client
        client.connect_async(host, port, keepalive=30)
        client.loop_start()
        self._emit("log", f"Connecting to MQTT {host}:{port} as {client_id}")

    def disconnect(self, emit: bool = True) -> None:
        if self._client is None:
            self.snapshot.connected = False
            return

        client = self._client
        self._client = None
        try:
            client.disconnect()
            client.loop_stop()
        finally:
            self.snapshot.connected = False
            if emit:
                self._emit("status", "MQTT disconnected")

    def command(self, name: str, value: str | None = None) -> None:
        if name == "tx":
            self._publish_command({"type": "tx"})
        elif name == "rx":
            self._publish_command({"type": "rx"})
        elif name == "clear_error":
            self._publish_command({"type": "clear_error"})
        elif name == "emergency_stop":
            self._publish(self.topics.emergency, self._compact_json({"type": "emergency_stop"}))
        elif name == "all_stop":
            self._publish(self.topics.all_emergency, "STOP")
        elif name == "get_direction":
            self._publish_command({"type": "getdirection"})
        elif name == "get_rssi":
            self._publish_command({"type": "getrssi"})
        elif name == "reset_gains":
            self._publish_command({"type": "resetk"})
        elif name == "set_direction":
            self.set_direction(value)
        elif name == "set_kp":
            self._publish_command({"type": "setkp", "value": self._format_gain(value)})
        elif name == "set_kd":
            self._publish_command({"type": "setkd", "value": self._format_gain(value)})
        else:
            raise ValueError(f"unsupported command: {name}")

    def set_direction(self, value: str | None) -> None:
        if value not in {"s0tos1", "s1tos0"}:
            raise ValueError("direction must be s0tos1 or s1tos0")
        self._publish_command({"type": "setdirection", "value": value})

    def _make_client(self, client_id: str) -> mqtt.Client:
        callback_api_version = getattr(mqtt, "CallbackAPIVersion", None)
        if callback_api_version is not None:
            return mqtt.Client(callback_api_version.VERSION1, client_id=client_id)
        return mqtt.Client(client_id=client_id)

    def _on_connect(self, client: mqtt.Client, _userdata: Any, _flags: Any, rc: int) -> None:
        if rc != 0:
            self.snapshot.connected = False
            self._emit("error", f"MQTT connect failed with rc={rc}")
            return

        self.snapshot.connected = True
        client.subscribe(self.topics.feedback, qos=0)
        client.subscribe(self.topics.tray, qos=0)
        self._emit("status", f"MQTT connected to {self.snapshot.mqtt_host}:{self.snapshot.mqtt_port}")
        self.command("get_direction")
        self.command("get_rssi")

    def _on_disconnect(self, _client: mqtt.Client, _userdata: Any, rc: int) -> None:
        self.snapshot.connected = False
        if rc == 0:
            self._emit("status", "MQTT disconnected")
        else:
            self._emit("error", f"MQTT disconnected unexpectedly with rc={rc}")

    def _on_message(self, _client: mqtt.Client, _userdata: Any, message: mqtt.MQTTMessage) -> None:
        payload = message.payload.decode("utf-8", errors="replace")
        data: dict[str, Any] | None = None
        try:
            parsed = json.loads(payload)
            if isinstance(parsed, dict):
                data = parsed
        except json.JSONDecodeError:
            data = None

        self.snapshot.last_topic = message.topic
        self.snapshot.last_payload = payload
        self.snapshot.last_update_monotonic = time.monotonic()
        if data is not None:
            self._update_snapshot(data)

        self._emit("message", f"RX {message.topic} {payload}", message.topic, payload, data)

    def _update_snapshot(self, data: dict[str, Any]) -> None:
        if "state" in data:
            self.snapshot.state = str(data["state"])
        if "state_elapsed_ms" in data:
            self.snapshot.state_elapsed_ms = self._as_int(data["state_elapsed_ms"])
        if "error" in data:
            self.snapshot.error = str(data["error"])
        elif "state" in data and str(data["state"]) not in {"ERROR", "ESTOP"}:
            self.snapshot.error = ""
        if "has_tray" in data:
            self.snapshot.has_tray = bool(data["has_tray"])
        if "s0" in data:
            self.snapshot.s0 = self._as_int(data["s0"])
        if "s1" in data:
            self.snapshot.s1 = self._as_int(data["s1"])
        if "direction" in data:
            self.snapshot.direction = str(data["direction"])
        if "rssi" in data:
            self.snapshot.rssi = self._as_int(data["rssi"])
        if data.get("config") == "speed_kp" and "value" in data:
            self.snapshot.speed_kp = str(data["value"])
        if data.get("config") == "speed_kd" and "value" in data:
            self.snapshot.speed_kd = str(data["value"])
        if data.get("config") == "speed_gains":
            self.snapshot.speed_kp = str(data.get("speed_kp", self.snapshot.speed_kp))
            self.snapshot.speed_kd = str(data.get("speed_kd", self.snapshot.speed_kd))

    def _publish_command(self, payload: dict[str, str]) -> None:
        self._publish(self.topics.command, self._compact_json(payload))

    def _publish(self, topic: str, payload: str) -> None:
        if self._client is None or not self.snapshot.connected:
            self._emit("error", "MQTT is not connected; command was not sent")
            raise RuntimeError("MQTT is not connected")

        result = self._client.publish(topic, payload, qos=0, retain=False)
        if result.rc != mqtt.MQTT_ERR_SUCCESS:
            self._emit("error", f"Publish failed rc={result.rc}: {topic} {payload}")
            raise RuntimeError(f"MQTT publish failed with rc={result.rc}")
        self._emit("publish", f"TX {topic} {payload}", topic, payload)

    def _emit(
        self,
        kind: str,
        message: str,
        topic: str = "",
        payload: str = "",
        data: dict[str, Any] | None = None,
    ) -> None:
        self.events.put(
            ConveyorEvent(
                kind=kind,
                message=message,
                topic=topic,
                payload=payload,
                data=data,
                timestamp=time.time(),
            )
        )

    @staticmethod
    def _compact_json(payload: dict[str, str]) -> str:
        return json.dumps(payload, separators=(",", ":"))

    @staticmethod
    def _format_gain(value: str | None) -> str:
        if value is None:
            raise ValueError("gain value is required")
        try:
            parsed = float(value)
        except ValueError as exc:
            raise ValueError("gain must be a decimal number") from exc

        if parsed < 0.0 or parsed > 100.0:
            raise ValueError("gain must be between 0.000 and 100.000")
        return f"{parsed:.3f}"

    @staticmethod
    def _as_int(value: Any) -> int | None:
        try:
            return int(value)
        except (TypeError, ValueError):
            return None
