from __future__ import annotations

import json
import queue
import threading
import time
from dataclasses import asdict, dataclass, field
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
DEFAULT_CONVEYOR_IDS = ("C0", "C1")


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
class ConveyorRuntime:
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


@dataclass
class ConveyorSnapshot:
    connected: bool = False
    mqtt_host: str = DEFAULT_MQTT_HOST
    mqtt_port: int = DEFAULT_MQTT_PORT
    conveyor_ids: list[str] = field(default_factory=lambda: list(DEFAULT_CONVEYOR_IDS))
    conveyors: dict[str, ConveyorRuntime] = field(
        default_factory=lambda: {conveyor_id: ConveyorRuntime(conveyor_id=conveyor_id) for conveyor_id in DEFAULT_CONVEYOR_IDS}
    )


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
        self.topics: dict[str, ConveyorTopics] = {
            conveyor_id: ConveyorTopics(conveyor_id=conveyor_id) for conveyor_id in DEFAULT_CONVEYOR_IDS
        }
        self._client: mqtt.Client | None = None
        self._lock = threading.RLock()

    def snapshot_dict(self) -> dict[str, Any]:
        with self._lock:
            data = asdict(self.snapshot)
            data["topics"] = {conveyor_id: topics.as_dict() for conveyor_id, topics in self.topics.items()}
        return data

    def connect(self, host: str, port: int, conveyor_ids: list[str]) -> None:
        self.disconnect(emit=False)
        conveyor_ids = self._normalize_conveyor_ids(conveyor_ids)
        with self._lock:
            self.topics = {conveyor_id: ConveyorTopics(conveyor_id=conveyor_id) for conveyor_id in conveyor_ids}
            self.snapshot.mqtt_host = host
            self.snapshot.mqtt_port = port
            self.snapshot.conveyor_ids = conveyor_ids
            self.snapshot.conveyors = {conveyor_id: ConveyorRuntime(conveyor_id=conveyor_id) for conveyor_id in conveyor_ids}

        client_id = f"conveyor_web_{'_'.join(conveyor_ids)}_{int(time.time())}"
        client = self._make_client(client_id)
        client.on_connect = self._on_connect
        client.on_disconnect = self._on_disconnect
        client.on_message = self._on_message
        with self._lock:
            self._client = client
        client.connect_async(host, port, keepalive=30)
        client.loop_start()
        self._emit("log", f"Connecting to MQTT {host}:{port} as {client_id}")

    def disconnect(self, emit: bool = True) -> None:
        if self._client is None:
            with self._lock:
                self.snapshot.connected = False
            return

        with self._lock:
            client = self._client
            self._client = None
        try:
            client.disconnect()
            client.loop_stop()
        finally:
            with self._lock:
                self.snapshot.connected = False
            if emit:
                self._emit("status", "MQTT disconnected")

    def command(self, name: str, value: str | None = None, conveyor_id: str | None = None) -> None:
        topics = self._topics_for_command(name, conveyor_id)
        if name == "tx":
            self._publish_command(topics, {"type": "tx"})
        elif name == "rx":
            self._publish_command(topics, {"type": "rx"})
        elif name == "clear_error":
            self._publish_command(topics, {"type": "clear_error"})
        elif name == "emergency_stop":
            self._publish(topics.emergency, self._compact_json({"type": "emergency_stop"}))
        elif name == "all_stop":
            self._publish(ConveyorTopics().all_emergency, "STOP")
        elif name == "get_direction":
            self._publish_command(topics, {"type": "getdirection"})
        elif name == "get_rssi":
            self._publish_command(topics, {"type": "getrssi"})
        elif name == "reset_gains":
            self._publish_command(topics, {"type": "resetk"})
        elif name == "set_direction":
            self.set_direction(value, topics)
        elif name == "set_kp":
            self._publish_command(topics, {"type": "setkp", "value": self._format_gain(value)})
        elif name == "set_kd":
            self._publish_command(topics, {"type": "setkd", "value": self._format_gain(value)})
        else:
            raise ValueError(f"unsupported command: {name}")

    def set_direction(self, value: str | None, topics: ConveyorTopics) -> None:
        if value not in {"s0tos1", "s1tos0"}:
            raise ValueError("direction must be s0tos1 or s1tos0")
        self._publish_command(topics, {"type": "setdirection", "value": value})

    def _topics_for_command(self, name: str, conveyor_id: str | None) -> ConveyorTopics:
        if name == "all_stop":
            return ConveyorTopics()
        if conveyor_id is None:
            raise ValueError("conveyor_id is required")
        with self._lock:
            topics = self.topics.get(conveyor_id)
        if topics is None:
            raise ValueError(f"unknown conveyor_id: {conveyor_id}")
        return topics

    def _make_client(self, client_id: str) -> mqtt.Client:
        callback_api_version = getattr(mqtt, "CallbackAPIVersion", None)
        if callback_api_version is not None:
            return mqtt.Client(callback_api_version.VERSION1, client_id=client_id)
        return mqtt.Client(client_id=client_id)

    def _on_connect(self, client: mqtt.Client, _userdata: Any, _flags: Any, rc: int) -> None:
        if rc != 0:
            with self._lock:
                self.snapshot.connected = False
            self._emit("error", f"MQTT connect failed with rc={rc}")
            return

        with self._lock:
            self.snapshot.connected = True
            topics_list = list(self.topics.values())
            conveyor_ids = list(self.snapshot.conveyor_ids)
            mqtt_host = self.snapshot.mqtt_host
            mqtt_port = self.snapshot.mqtt_port

        for topics in topics_list:
            client.subscribe(topics.feedback, qos=0)
            client.subscribe(topics.tray, qos=0)
        self._emit("status", f"MQTT connected to {mqtt_host}:{mqtt_port}")
        for conveyor_id in conveyor_ids:
            self.command("get_direction", conveyor_id=conveyor_id)
            self.command("get_rssi", conveyor_id=conveyor_id)

    def _on_disconnect(self, _client: mqtt.Client, _userdata: Any, rc: int) -> None:
        with self._lock:
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

        with self._lock:
            conveyor_id = self._conveyor_id_for_topic(message.topic)
            runtime = self.snapshot.conveyors.get(conveyor_id) if conveyor_id is not None else None
            if runtime is not None:
                runtime.last_topic = message.topic
                runtime.last_payload = payload
                runtime.last_update_monotonic = time.monotonic()
            if data is not None:
                self._update_snapshot(data, runtime)

        self._emit("message", f"RX {message.topic} {payload}", message.topic, payload, data)

    def _update_snapshot(self, data: dict[str, Any], runtime: ConveyorRuntime | None) -> None:
        if runtime is None:
            return
        if "state" in data:
            runtime.state = str(data["state"])
        if "state_elapsed_ms" in data:
            runtime.state_elapsed_ms = self._as_int(data["state_elapsed_ms"])
        if "error" in data:
            runtime.error = str(data["error"])
        elif "state" in data and str(data["state"]) not in {"ERROR", "ESTOP"}:
            runtime.error = ""
        if "has_tray" in data:
            runtime.has_tray = bool(data["has_tray"])
        if "s0" in data:
            runtime.s0 = self._as_int(data["s0"])
        if "s1" in data:
            runtime.s1 = self._as_int(data["s1"])
        if "direction" in data:
            runtime.direction = str(data["direction"])
        if "rssi" in data:
            runtime.rssi = self._as_int(data["rssi"])
        if data.get("config") == "speed_kp" and "value" in data:
            runtime.speed_kp = str(data["value"])
        if data.get("config") == "speed_kd" and "value" in data:
            runtime.speed_kd = str(data["value"])
        if data.get("config") == "speed_gains":
            runtime.speed_kp = str(data.get("speed_kp", runtime.speed_kp))
            runtime.speed_kd = str(data.get("speed_kd", runtime.speed_kd))

    def _publish_command(self, topics: ConveyorTopics, payload: dict[str, str]) -> None:
        self._publish(topics.command, self._compact_json(payload))

    def _conveyor_id_for_topic(self, topic: str) -> str | None:
        # Caller must hold self._lock.
        for conveyor_id, topics in self.topics.items():
            if topic in {topics.feedback, topics.tray}:
                return conveyor_id
        return None

    def _publish(self, topic: str, payload: str) -> None:
        with self._lock:
            client = self._client
            connected = self.snapshot.connected

        if client is None or not connected:
            self._emit("error", "MQTT is not connected; command was not sent")
            raise RuntimeError("MQTT is not connected")

        result = client.publish(topic, payload, qos=0, retain=False)
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
    def _normalize_conveyor_ids(conveyor_ids: list[str]) -> list[str]:
        normalized: list[str] = []
        for conveyor_id in conveyor_ids:
            conveyor_id = conveyor_id.strip()
            if conveyor_id and conveyor_id not in normalized:
                normalized.append(conveyor_id)
        if not normalized:
            raise ValueError("at least one conveyor ID is required")
        if len(normalized) > 2:
            raise ValueError("only two conveyor IDs are supported")
        return normalized

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
