# Plan: Web MQTT Conveyor Controller

## Goal

Replace the separate Tkinter desktop GUI with a browser-based webapp for controlling the conveyor through the existing MQTT backend.

The webapp should:

- Run locally as a small Python web server.
- Open in a browser at a local URL, e.g. `http://127.0.0.1:8080`.
- Connect to the existing plain MQTT broker at `192.168.1.126:1883` from the Python backend.
- Stream live conveyor status to the browser over WebSocket.
- Publish only the compact MQTT payloads that the ESP32 firmware currently accepts.
- Preserve the same safety boundary: no raw PWM, no arbitrary MQTT publishing by default.

## Current State

The previous implementation added a desktop GUI package under `tools/conveyor_gui`:

- `conveyor_mqtt.py`: useful MQTT driver with topic construction, compact payload publishing, snapshot updates, and event queue.
- `app.py`: Tkinter GUI that the user no longer wants.
- `__main__.py`: starts the Tkinter GUI.
- `requirements.txt`: currently only `paho-mqtt>=2,<3`.

Firmware protocol remains unchanged:

- Broker: `192.168.1.126:1883`
- Command topic: `conveyor/C0/cmd`
- Emergency topic: `conveyor/C0/emergency`
- All-emergency topic: `conveyor/all/emergency`
- Feedback topic: `conveyor/C0/feedback`
- Tray topic: `conveyor/C0/tray`

Accepted normal command payloads:

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

Accepted emergency payloads:

- `{"type":"emergency_stop"}` on `conveyor/C0/emergency`
- `STOP` on `conveyor/all/emergency`

## Design Decision

Use a Python backend web server rather than direct browser-to-MQTT.

Reason:

- The configured MQTT broker is plain MQTT on port `1883`.
- Browsers cannot speak raw MQTT over TCP.
- Browser MQTT requires MQTT-over-WebSocket support on the broker, which is not documented in this project.
- A small Python backend can reuse `paho-mqtt`, keep the firmware protocol unchanged, and expose a normal browser UI over HTTP/WebSocket.

## Proposed Structure

Replace `tools/conveyor_gui` with `tools/conveyor_web`.

Files:

- `tools/conveyor_web/__init__.py`
  - Package marker.
- `tools/conveyor_web/mqtt_backend.py`
  - Reuse and adapt the current `ConveyorMqttClient` logic from `tools/conveyor_gui/conveyor_mqtt.py`.
  - Keep `ConveyorTopics`, `ConveyorSnapshot`, and compact payload command methods.
  - Replace Tkinter/event-queue assumptions with callback/event broadcasting suitable for WebSocket clients.
  - Keep `paho-mqtt` network loop in the backend.
- `tools/conveyor_web/server.py`
  - FastAPI app.
  - Serves static frontend files.
  - Owns a shared `ConveyorMqttClient` instance.
  - Provides REST endpoints for connection and commands.
  - Provides a WebSocket endpoint for live events and snapshots.
- `tools/conveyor_web/__main__.py`
  - CLI entry point:
    - `python3 -m tools.conveyor_web --host 127.0.0.1 --port 8080 --mqtt-host 192.168.1.126 --mqtt-port 1883 --id C0`
  - Starts `uvicorn`.
- `tools/conveyor_web/static/index.html`
  - Single-page browser UI.
- `tools/conveyor_web/static/styles.css`
  - Responsive industrial-control style layout.
- `tools/conveyor_web/static/app.js`
  - Browser-side WebSocket handling, status rendering, command requests, form validation, and bounded log.
- `tools/conveyor_web/requirements.txt`
  - `paho-mqtt>=2,<3`
  - `fastapi>=0.110,<1`
  - `uvicorn[standard]>=0.27,<1`

Remove or supersede:

- Delete `tools/conveyor_gui/app.py` because the Tkinter GUI is no longer wanted.
- Delete or replace `tools/conveyor_gui/__main__.py` so the documented entry point is no longer a desktop app.
- Prefer deleting the full `tools/conveyor_gui/` directory after migrating reusable logic to `tools/conveyor_web/`.
- Update docs to refer only to the webapp.

## Backend API

REST endpoints:

- `GET /api/snapshot`
  - Returns current MQTT connection and conveyor snapshot.
- `POST /api/connect`
  - Body: `{ "mqtt_host": "192.168.1.126", "mqtt_port": 1883, "conveyor_id": "C0" }`
  - Connects/reconnects MQTT backend and subscribes to feedback/tray.
- `POST /api/disconnect`
  - Disconnects MQTT backend.
- `POST /api/command`
  - Body: `{ "command": "tx" }`, `{ "command": "rx" }`, etc.
  - Supported commands:
    - `tx`
    - `rx`
    - `clear_error`
    - `emergency_stop`
    - `all_stop`
    - `get_direction`
    - `get_rssi`
    - `reset_gains`
    - `set_direction` with `value` of `s0tos1` or `s1tos0`
    - `set_kp` with `value`
    - `set_kd` with `value`
  - Validates command names and values server-side.
  - Calls the MQTT backend command methods.
- `GET /health`
  - Simple health/status endpoint for quick checks.

WebSocket endpoint:

- `GET /ws`
  - Sends initial snapshot on connect.
  - Broadcasts MQTT events, publish events, errors, and snapshot updates to all connected browser clients.
  - Browser reconnects automatically if the socket drops.

## Frontend UI

Single-page layout:

- Header:
  - App title.
  - WebSocket status.
  - MQTT broker connection status.
- Connection card:
  - MQTT host input, default `192.168.1.126`.
  - MQTT port input, default `1883`.
  - Conveyor ID input, default `C0`.
  - Connect and Disconnect buttons.
- Status cards:
  - State.
  - State elapsed ms.
  - Error.
  - Tray presence.
  - Raw S0/S1 with reminder: `0 = tray detected`, `1 = clear`.
  - Direction.
  - RSSI.
  - KP/KD acknowledgements if known.
- Control card:
  - `TX`.
  - `RX`.
  - `Clear Error`.
  - `Emergency Stop` with prominent red styling.
  - `All Stop` with prominent red styling.
  - `Get Direction`.
  - `Get RSSI`.
- Direction card:
  - Selector for `s0tos1` and `s1tos0`.
  - Set button.
- PID tuning card:
  - KP input and set button.
  - KD input and set button.
  - Reset gains button.
  - Browser validation for `0.000` through `100.000`, with server-side validation still authoritative.
- Log panel:
  - Shows received MQTT messages and published commands.
  - Bounded to the latest 500 entries.
  - Clear-log button.

Frontend implementation style:

- Plain HTML/CSS/JavaScript, no Node build step.
- Use `fetch()` for commands and connection actions.
- Use browser `WebSocket` for live updates.
- Keep the UI responsive for laptop/tablet/mobile.
- Disable command buttons when MQTT is disconnected.
- Do not hide firmware errors; display feedback `error` fields directly.

## Safety Behavior

- The webapp must not expose arbitrary topic/payload publishing by default.
- The backend command endpoint must reject unknown command names.
- Gain values must be validated and formatted to exactly three decimal places before publishing.
- Emergency stop uses `conveyor/<id>/emergency`.
- All stop uses `conveyor/all/emergency` with `STOP`.
- Firmware remains the authority for `JOB_BUSY`, `NO_TRAY`, `TRAY_PRESENT`, and timeout errors.

## Documentation Updates

Update:

- `README.md`
  - Replace the "Python MQTT GUI" section with "Python Web GUI".
  - New run commands:
    - `python3 -m pip install -r tools/conveyor_web/requirements.txt`
    - `python3 -m tools.conveyor_web --mqtt-host 192.168.1.126 --mqtt-port 1883 --id C0`
    - Open `http://127.0.0.1:8080`.
- `docs/mqtt-control-commands.md`
  - Replace the Tkinter GUI section with the webapp section.
- `project.md`
  - Update status note from Tkinter GUI to webapp.
  - Update file list entry from `tools/conveyor_gui/` to `tools/conveyor_web/`.
- Optional: replace `.kilo/plans/python-mqtt-gui-conveyor-controller.md` references only if needed; otherwise leave historical plan intact.

## Implementation Steps

1. Create `tools/conveyor_web/` package and static folder.
2. Migrate reusable MQTT logic from `tools/conveyor_gui/conveyor_mqtt.py` into `tools/conveyor_web/mqtt_backend.py`.
3. Add event broadcasting hooks to the MQTT backend:
   - Maintain the current snapshot.
   - Push events into an async-safe broadcaster or thread-safe queue consumed by FastAPI.
   - Ensure Paho callbacks do not directly mutate asyncio WebSocket objects.
4. Implement `server.py` with FastAPI:
   - Static file mounting.
   - REST command endpoints.
   - WebSocket manager with connected client set.
   - Background task that drains MQTT backend events and broadcasts them.
5. Implement `static/index.html`, `static/styles.css`, and `static/app.js`.
6. Implement `__main__.py` to run `uvicorn` with CLI options.
7. Delete the Tkinter-specific files/package after migration.
8. Add `tools/conveyor_web/requirements.txt`.
9. Update docs.
10. Run verification.

## Verification

Static checks:

- `python3 -m compileall tools/conveyor_web`
- `python3 -m tools.conveyor_web --help`

Local web-server smoke test:

- Start the server with a test port, e.g. `python3 -m tools.conveyor_web --host 127.0.0.1 --port 8080 --mqtt-host 192.168.1.126 --mqtt-port 1883 --id C0`.
- Fetch `http://127.0.0.1:8080/health` and confirm JSON response.
- Fetch `http://127.0.0.1:8080/` and confirm HTML is served.

Manual broker/hardware checks:

- Open `http://127.0.0.1:8080`.
- Connect to MQTT broker.
- Confirm the UI shows MQTT connected.
- Confirm initial `getdirection` and `getrssi` publish after MQTT connect.
- Confirm feedback/tray updates stream into the UI log and status cards.
- Test `TX`, `RX`, `Clear Error`, `Emergency Stop`, and `All Stop` against hardware when safe.
- Test KP/KD set and reset acknowledgements.

## Open Notes

- This plan avoids relying on MQTT-over-WebSocket broker support. If the broker later enables WebSocket MQTT, the browser could connect directly with an MQTT JS client, but that would expose broker details and would still need careful command validation.
- The backend web server is still a local Python process, but the control surface is browser-based rather than a separate desktop GUI window.
