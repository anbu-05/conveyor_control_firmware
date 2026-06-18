# Plan: Python MQTT GUI Conveyor Controller

## Goal

Create a laptop-side Python controller/driver with a GUI for the existing conveyor MQTT backend.

The controller should:

- Connect to the configured MQTT broker at `192.168.1.126` by default.
- Publish only the compact literal payloads that the current ESP32 firmware accepts.
- Subscribe to conveyor feedback and tray topics.
- Show live state, sensor, tray, direction, RSSI, errors, and command acknowledgements.
- Provide safe GUI controls for TX, RX, emergency stop, clear error, direction, RSSI query, and PID gain tuning.

## Current Code Findings

- Firmware MQTT settings live in `main/config/config.h`:
  - Broker URI: `mqtt://192.168.1.126`
  - Conveyor ID: `C0`
  - Command topic: `conveyor/C0/cmd`
  - Emergency topic: `conveyor/C0/emergency`
  - All-emergency topic: `conveyor/all/emergency`
  - Feedback topic: `conveyor/C0/feedback`
  - Tray topic: `conveyor/C0/tray`
- MQTT parsing is intentionally strict in `main/tasks/mqtt_task.c`; payloads must be compact exact JSON strings.
- Normal job control payloads on `conveyor/C0/cmd`:
  - `{"type":"tx"}`
  - `{"type":"rx"}`
  - `{"type":"clear_error"}`
  - `{"type":"setdirection","value":"s0tos1"}`
  - `{"type":"setdirection","value":"s1tos0"}`
  - `{"type":"getdirection"}`
  - `{"type":"getrssi"}`
  - `{"type":"setkp","value":"0.010"}`
  - `{"type":"setkd","value":"0.010"}`
  - `{"type":"resetk"}`
- Emergency payloads can be sent to `conveyor/C0/emergency`:
  - `{"type":"emergency_stop"}`
  - `STOP`
- Feedback arrives as JSON on `conveyor/C0/feedback` and may include:
  - Job status: `id`, `state`, `state_elapsed_ms`, `s0`, `s1`, `direction`
  - Errors: same fields plus `error`
  - Direction response: `id`, `direction`
  - RSSI response: `id`, `rssi`
  - PID acknowledgements: `config`, `value`, or `speed_kp`/`speed_kd`
- Tray status arrives on `conveyor/C0/tray` as `id`, `has_tray`, `s0`, and `s1`.
- Raw sensor values use `0 = tray detected`, `1 = no tray`.
- There is no existing Python packaging in this repo: no `pyproject.toml` and no `requirements.txt`.

## Design

Use a small Python app with a separated driver/backend and GUI layer:

- `tools/conveyor_gui/` as a self-contained laptop utility folder.
- `paho-mqtt` for MQTT client support.
- Tkinter for the GUI because it ships with most Python installations and avoids heavy GUI dependencies.
- A background MQTT network thread via Paho, with messages passed into Tkinter using a `queue.Queue` and `root.after(...)` polling.
- A driver class that owns MQTT connection state, topic names, publishing, subscription callbacks, JSON parsing, and a current status snapshot.
- A GUI class that renders controls and status, never directly touching Paho callbacks.

Suggested files:

- `tools/conveyor_gui/conveyor_mqtt.py`
  - `ConveyorTopics` dataclass.
  - `ConveyorSnapshot` dataclass for current GUI state.
  - `ConveyorMqttClient` class for connect/disconnect/subscribe/publish.
  - Command methods: `tx()`, `rx()`, `clear_error()`, `emergency_stop()`, `all_stop()`, `set_direction(value)`, `get_direction()`, `get_rssi()`, `set_kp(value)`, `set_kd(value)`, `reset_gains()`.
  - Payload construction should use fixed compact strings or `json.dumps(..., separators=(",", ":"))` to preserve firmware compatibility.
- `tools/conveyor_gui/app.py`
  - Tkinter application.
  - Connection controls for broker host, port, conveyor ID, connect/disconnect.
  - Status panel for MQTT connection, state, elapsed time, error, tray presence, raw S0/S1, direction, RSSI, and latest message timestamp.
  - Command buttons for TX, RX, clear error, emergency stop, all stop, get direction, get RSSI, set direction.
  - PID gain entries/buttons for set KP, set KD, reset gains.
  - Scrolling log area for incoming messages and published commands.
  - Disable normal job/direction buttons while disconnected; leave emergency disabled when disconnected because MQTT cannot send without a broker connection.
- `tools/conveyor_gui/__main__.py`
  - Allows running with `python -m tools.conveyor_gui`.
- `requirements.txt`
  - Add `paho-mqtt>=2,<3` if the repo should have one global dependency file.
  - Alternative: add `tools/conveyor_gui/requirements.txt` if keeping laptop utility dependencies isolated is preferred.
- Documentation update:
  - `README.md`: add a short "Python GUI" section with install/run commands.
  - `docs/mqtt-control-commands.md`: optionally link to the GUI utility as a laptop alternative to Mosquitto commands.
  - `project.md`: add a short status note and file list entries.

## GUI Behavior

Connection panel:

- Defaults:
  - Host: `192.168.1.126`
  - Port: `1883`
  - Conveyor ID: `C0`
- On connect:
  - Subscribe to `conveyor/<id>/feedback`.
  - Subscribe to `conveyor/<id>/tray`.
  - Optionally subscribe to `conveyor/<id>/#` for future visibility, but parse only known topics.
  - Publish `getdirection` and `getrssi` once after connection to populate initial fields.

Status panel:

- State text with color hints:
  - `IDLE`, `TX_DONE`, `RX_DONE`: normal/green-ish.
  - `TX_*`, `RX_*`: active/blue-ish.
  - `ERROR`, feedback with `error`: warning/red-ish.
  - `ESTOP`: prominent red.
- Tray display:
  - `has_tray=true`: tray present.
  - Raw `s0`/`s1` displayed with labels that explain `0 = detected`.
- Error field should retain the last error until a later feedback message has no `error`, or clear when `clear_error` is sent and a non-error state is received.

Controls:

- `TX` publishes `{"type":"tx"}` to `conveyor/C0/cmd`.
- `RX` publishes `{"type":"rx"}` to `conveyor/C0/cmd`.
- `Emergency Stop` publishes `{"type":"emergency_stop"}` to `conveyor/C0/emergency`.
- `All Stop` publishes `STOP` to `conveyor/all/emergency`.
- `Clear Error` publishes `{"type":"clear_error"}` to `conveyor/C0/cmd`.
- Direction selector publishes exact `setdirection` payloads with `s0tos1` or `s1tos0`.
- Gain setters validate decimals to `0.000` through `100.000`, format to three fractional digits, and publish exact quoted values.

Log panel:

- Append received topic and decoded payload.
- Append published topic and payload.
- Keep a bounded log, e.g. last 500 lines, to avoid unbounded memory growth.

## Safety Notes

- The GUI must not expose raw `setmotor`, direct PWM, or arbitrary MQTT payload entry by default because firmware MQTT intentionally supports only high-level safe commands.
- Emergency stop should use the dedicated emergency topic, not only the command topic.
- The GUI should display firmware rejections from feedback `error` fields instead of trying to infer all preconditions locally.
- The GUI can optionally disable `TX` when `has_tray` is false and disable `RX` when `has_tray` is true, but should still rely on firmware as the authority.

## Implementation Steps

1. Create `tools/conveyor_gui/` package.
2. Implement `ConveyorTopics` and `ConveyorMqttClient` using `paho.mqtt.client.Client`.
3. Implement robust message handling:
   - Decode UTF-8 payloads.
   - Parse JSON when possible.
   - Treat `STOP` or other non-JSON messages as plain text.
   - Update a snapshot from known fields.
   - Push structured events into a thread-safe queue for the GUI.
4. Implement the Tkinter app:
   - Layout connection, status, command, tuning, and log sections.
   - Poll the driver event queue with `after(...)`.
   - Keep GUI updates on the Tk main thread only.
5. Add run entry point in `__main__.py`.
6. Add dependency file for `paho-mqtt`.
7. Update docs with install/run commands and the default broker/topic assumptions.

## Verification

Static/local checks:

- Run Python syntax checks with `python -m compileall tools/conveyor_gui`.
- Run import smoke test with `python -m tools.conveyor_gui --help` if CLI args are added, or instantiate the app module enough to ensure imports work.
- Verify emitted payload strings exactly match firmware parser expectations from `main/tasks/mqtt_task.c`.

Manual MQTT checks with broker/hardware:

- Start the GUI and connect to `192.168.1.126:1883`.
- Confirm connection state changes and subscriptions are active.
- Press `Get Direction`; confirm `direction` feedback appears.
- Press `Get RSSI`; confirm RSSI or `RSSI_UNAVAILABLE` appears.
- Confirm tray panel updates from `conveyor/C0/tray` when sensors change.
- With tray present, press `TX`; confirm state transitions and completion feedback.
- With conveyor empty, press `RX`; confirm waiting/active/done feedback.
- Press `Emergency Stop`; confirm `ESTOP` feedback and motor stop.
- Press `Clear Error`; confirm return toward `IDLE`.
- Set KP/KD and reset gains; confirm config acknowledgement payloads.

## Open Decisions

- Toolkit choice: this plan uses Tkinter to minimize dependencies. If a more polished GUI is required, replace the GUI layer with PySide6 or customtkinter while keeping the MQTT driver unchanged.
- Dependency placement: prefer `tools/conveyor_gui/requirements.txt` for an isolated utility, unless the project wants one root `requirements.txt`.
- Packaging: a full `pyproject.toml` is not necessary for a simple internal tool, but can be added later if this GUI should be installed as a command-line app.
