# Serial Control Tab Plan

## Goal

Add a separate **Serial** tab to the existing browser controller so the same web app can control and inspect the conveyor over the ESP32-S3 USB Serial/JTAG console, while keeping the current MQTT tab and Claude-updated dark/violet aesthetic intact.

## Current Context

- Web app lives in `tools/conveyor_web/` with FastAPI backend, static HTML/CSS/JS frontend, and WebSocket event streaming.
- Current UI styling uses compact dark cards, violet accent tokens, small uppercase headings, pill statuses, and responsive grids. New UI should extend these classes instead of replacing the visual language.
- Firmware serial command handling is strict and token-based in `main/tasks/command_task.c`.
- Serial console is usually `/dev/ttyACM0` on ESP32-S3 USB Serial/JTAG.
- Serial commands are more powerful than MQTT and include direct motor control, raw speed control, runtime config edits, sensor/encoder watches, state-machine jobs, direction, RSSI, and diagnostics.

## Proposed Architecture

Add a serial backend alongside the existing MQTT backend:

- `tools/conveyor_web/serial_backend.py`
  - Owns a single serial connection.
  - Uses `pyserial`.
  - Starts a reader thread after connect.
  - Writes one command line at a time with `\n` or `\r\n`.
  - Pushes parsed/raw serial output events into a thread-safe queue, mirroring the MQTT event pattern.
  - Maintains a snapshot for UI rendering: connected status, port, baud, last output, motor state, tray state, encoder state, config values, direction, RSSI, active watch flags.

- `tools/conveyor_web/server.py`
  - Add REST endpoints for serial connect/disconnect and commands.
  - Extend WebSocket event pump to broadcast both MQTT and serial events.
  - Keep MQTT endpoints unchanged.

- `tools/conveyor_web/static/index.html`
  - Add top-level tabs: `MQTT` and `Serial`.
  - Keep current MQTT layout inside the MQTT panel.
  - Add separate Serial panel with connection, safe controls, advanced controls, config editor, and serial log.

- `tools/conveyor_web/static/app.js`
  - Add tab switching.
  - Add serial snapshot rendering.
  - Add serial command helpers.
  - Keep MQTT behavior stable.

- `tools/conveyor_web/static/styles.css`
  - Add only incremental classes for tabs, split serial grids, danger zones, terminal-like serial log, and compact command groups.
  - Preserve current token palette and spacing.

## Dependencies

Update `tools/conveyor_web/requirements.txt`:

```text
paho-mqtt
fastapi
uvicorn[standard]
pyserial
```

## Backend API Plan

Add serial-specific models/endpoints:

- `POST /api/serial/connect`
  - Body: `{ "port": "/dev/ttyACM0", "baud": 115200 }`
  - Opens serial connection and starts reader thread.
  - Defaults: `/dev/ttyACM0`, `115200`.

- `POST /api/serial/disconnect`
  - Stops reader thread and closes serial port.

- `GET /api/serial/snapshot`
  - Returns serial backend snapshot.

- `POST /api/serial/command`
  - Body: `{ "command": "getmotor", "args": ["M0"] }` for structured commands.
  - Backend constructs the strict firmware line from an allowlist.
  - Returns immediately after write; output arrives through WebSocket/log.

- `POST /api/serial/raw`
  - Optional advanced endpoint for a raw command line.
  - Disabled by default in UI unless the user expands an Advanced/Raw section.
  - Still rejects embedded newlines and overly long lines.

Extend WebSocket packets:

```json
{
  "type": "serial_event",
  "event": { "direction": "rx", "message": "MOTOR M0 ..." },
  "serial_snapshot": { ... }
}
```

Existing MQTT WebSocket packets stay compatible.

## Serial Backend Behavior

Connection:

- Open serial with `timeout=0.1`, `write_timeout=1`.
- Flush stale input on connect, but do not hide subsequent startup lines like `READY conveyor`.
- Reader thread decodes bytes as UTF-8 with replacement and splits on lines.
- Prefix events as `rx` for firmware output and `tx` for commands sent by the web UI.
- Store bounded log, e.g. latest 1000 lines.

Command sending:

- Require serial connection before sending.
- Serialize writes with a lock.
- Reject empty lines, embedded CR/LF, and commands above a small length limit.
- Append line ending consistently.

Parsing:

- Parse recognized output tokens into snapshot fields:
  - `READY conveyor`
  - `OK ...`
  - `ERR ...`
  - `EVENT SENSOR <sensor> <old> <new>`
  - `EVENT ENCODER M0 <count> <speed>`
  - `EVENT JOB C0 <state>`
  - `ENCODER M0 <count> <gpio17_a> <gpio18_b>`
  - `MOTOR M0 <pwm> <direction> <position> <target_speed> <current_speed> <speed_control>`
  - `TRAY C0 <has_tray> <s0> <s1>`
  - `CONFIG <key> <value>`
  - `DIRECTION C0 <direction>`
  - `RSSI C0 <rssi>`
- Keep raw lines even when parsing fails.

## UI Plan

Top-level navigation:

- Add tabs near the header: `MQTT Control` and `Serial Debug`.
- Header status area should show WebSocket plus the active transport status.
- MQTT tab keeps the current layout and controls.

Serial tab sections:

1. **Serial Connection**
   - Port input default `/dev/ttyACM0`.
   - Baud input default `115200`.
   - Connect/disconnect buttons.
   - Serial connected/disconnected pill.

2. **Quick Diagnostics**
   - Buttons: `getmotor M0`, `getencoder M0`, `gettray`, `getconfig`, `getdirection`, `getrssi`.
   - Render parsed motor/tray/encoder/config values in compact metric cards.

3. **Job Control**
   - Buttons: `jobtx`, `jobrx`, `clearerror`, `estop`.
   - Keep `estop` styled as danger.
   - Include `stop` as a separate immediate-stop danger action with clear labeling because it directly stops all motors and queues emergency stop.

4. **Motor Debug**
   - `setmotor M0 <pwm> <direction>` with PWM constrained to `0..255` and direction select `0/1`.
   - `stopmotor M0` button.
   - `setspeed M0 <speed>` with signed integer range `-100000..100000`.
   - Visually mark this section as advanced/powerful because it bypasses normal job behavior.

5. **Runtime Config**
   - Config key select with editable keys:
     - `run_pwm`
     - `run_speed_counts_per_sec`
     - `speed_kp_milli`
     - `speed_kd_milli`
     - `done_hold_ms`
     - `tx_detect_timeout_ms`
     - `tx_clear_timeout_ms`
     - `rx_detect_timeout_ms`
     - `rx_done_timeout_ms`
     - `mqtt_status_period_ms`
   - Value input with client-side range validation from firmware docs.
   - Buttons: `get selected`, `set selected`, `get all`, `resetconfig`.
   - Keep `setkp`, `setkd`, and `resetk` as convenience controls because they are common tuning operations.

6. **Watchers**
   - Toggles/buttons:
     - `watchsensors on/off`
     - `watchencoder M0 on/off`
   - Show active watch flags from successful `OK WATCH...` responses.
   - Stream watcher events into the serial log and parsed metric cards.

7. **Raw Console**
   - Collapsed by default or visually separated.
   - Single-line input only.
   - Send button.
   - Warning copy: raw serial commands can move the motor or persist config.
   - No multi-line paste execution.

8. **Serial Log**
   - Separate from MQTT log.
   - Terminal-like `<pre>` using current dark card style.
   - Include direction markers, e.g. `>` for sent commands and `<` for received firmware lines.
   - Clear button.

## Command Allowlist

Structured serial commands to support initially:

- `setmotor M0 <pwm> <direction>`
- `stopmotor M0`
- `setspeed M0 <speed>`
- `setkp <decimal>`
- `setkd <decimal>`
- `resetk`
- `stop`
- `watchsensors on|off`
- `watchencoder M0 on|off`
- `getencoder M0`
- `getmotor M0`
- `gettray`
- `getconfig [key]`
- `setconfig <key> <value>`
- `resetconfig`
- `jobtx`
- `jobrx`
- `estop`
- `clearerror`
- `setdirection s0tos1|s1tos0`
- `getdirection`
- `getrssi`

Validation should mirror firmware limits so bad commands are caught before hitting the board where possible.

## Safety Guardrails

- Keep MQTT and serial tabs separate to avoid confusion over which transport is active.
- Disable serial controls unless serial is connected.
- Use danger styling for `estop`, `stop`, `resetconfig`, and raw command send.
- Require a browser `confirm()` for high-risk serial actions:
  - `setmotor`
  - `setspeed` with nonzero speed
  - `stop`
  - `resetconfig`
  - raw command send
- Do not automatically send direct motor commands on page load.
- Do not auto-enable encoder/sensor watchers on connect.
- Do not hide firmware `ERR ...` lines; show them prominently in the serial log.

## Implementation Steps

1. Add `pyserial` to requirements.
2. Implement `SerialBackend` with connect/disconnect, command sending, reader thread, output parsing, event queue, and snapshot.
3. Add serial REST endpoints and integrate serial events into the existing WebSocket pump.
4. Refactor frontend state so MQTT and serial state/logs are separate.
5. Add top-level tab UI while preserving the existing MQTT controls and Claude-updated styling.
6. Add Serial tab connection controls, diagnostics, job control, motor debug, config editor, watcher controls, raw console, and serial log.
7. Add client-side validation and high-risk confirmations.
8. Update README/docs with install/run notes and serial tab usage.
9. Verify syntax and help output.
10. If dependencies are installed, run local API smoke tests for `/health`, serial snapshot, connect failure handling, and command validation.

## Verification Plan

Minimum verification without hardware:

- `python3 -m compileall tools/conveyor_web`
- `python3 -m tools.conveyor_web --help`
- Import dependency check after installing requirements.
- Start FastAPI app locally and fetch `/health`.
- Confirm `/api/serial/snapshot` returns disconnected snapshot.
- Confirm invalid serial command payloads return `400`.
- Confirm serial connect to a missing port returns a clear error.

Hardware verification with ESP32 connected:

- Connect `/dev/ttyACM0` at `115200`.
- Observe `READY conveyor` if board resets or emits it.
- Send `getmotor M0`, `gettray`, `getconfig`, `getdirection`, `getrssi` and confirm parsed cards update.
- Toggle `watchsensors on/off` and confirm sensor events stream.
- Toggle `watchencoder M0 on/off` and confirm encoder events stream.
- Test `jobtx`/`jobrx` only under valid tray conditions.
- Test `estop` and `clearerror`.
- Test `stopmotor M0` after any direct motor test.

## Files Expected To Change

- `tools/conveyor_web/requirements.txt`
- `tools/conveyor_web/serial_backend.py`
- `tools/conveyor_web/server.py`
- `tools/conveyor_web/static/index.html`
- `tools/conveyor_web/static/styles.css`
- `tools/conveyor_web/static/app.js`
- `README.md`
- `docs/serial-debug-commands.md` or `docs/mqtt-control-commands.md` only if cross-linking the web serial tab is useful
- `project.md`

## Open Decisions

Default plan decisions unless overridden:

- Use backend `pyserial`, not browser Web Serial.
- Default port `/dev/ttyACM0`, baud `115200`.
- Keep raw serial command available but collapsed/advanced and confirmation-gated.
- Preserve current aesthetic exactly and add only incremental CSS.
