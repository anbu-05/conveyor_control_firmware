# PID Driver Copy Plan

## Goal
Create a new PID-focused browser serial driver as a copy of the existing root-level `driver/` package, named `pid_driver`, while leaving `driver/` intact.

The PID driver should expose only:
- `status`.
- `setmotor`.
- `stopmotor`.
- `stop`.
- `get_pidmode` with one automatic check after connecting and a manual refresh button.
- Position mode UI: `setposition`, polled `getposition`, graphing position samples.
- Speed mode UI: `setspeed`, polled `getspeed`, graphing speed samples.
- `setpid` and `getpid` for per-motor `kp_milli`, `ki_milli`, `kd_milli`.
- `setoffset`.
- An interactive raw console.

## Confirmed Code Facts
- Existing browser driver lives in `driver/` with `server.py`, `serial_backend.py`, `__main__.py`, and static assets.
- Existing serial send/read behavior in `driver/serial_backend.py` already handles ESP-IDF console line endings, linenoise DSR responses, command queue timing on the frontend, and WebSocket event broadcasting.
- Firmware command outputs in `main/tasks/console.c` are exact enough to parse rigidly:
  - `OK PIDMODE motor=<id> mode=position|speed`.
  - `OK POSITION motor=<id> pos=<n>`.
  - `OK SETPOSITION motor=<id> pos=<n>`.
  - `OK SPEED motor=<id> speed=<n>`.
  - `OK SETSPEED motor=<id> speed=<n>`.
  - `OK PID motor=<id> kp_milli=<n> ki_milli=<n> kd_milli=<n>`.
  - `OK SETPID motor=<id> kp_milli=<n> ki_milli=<n> kd_milli=<n>`.
  - `OK SETOFFSET motor=<id> offset=<n>`.
  - `OK SETMOTOR ...`, `OK STOPMOTOR ...`, `OK STOP ...` are already partially handled by the current backend.
- Existing `driver/serial_backend.py` currently lacks command builder support for `setspeed`, `getspeed`, `setpid`, and `getpid`; the PID driver copy must add those there.
- User specifically requested concise code, comments explaining why changed blocks were touched, minimal headers, and rigid communication with no fallback parsing/retries/malformed-data recovery.

## Implementation Tasks
1. Copy the existing package.
- Add `pid_driver/` as a source copy of `driver/`.
- Keep the original `driver/` files unchanged unless an import/package conflict is discovered.
- Rename titles/descriptions in the copy to `PID Driver` / `Conveyor PID Driver` so logs and browser labels make it clear which driver is running.
- In `pid_driver/__main__.py`, keep the same CLI flags but use a different default HTTP port, preferably `8081`, to avoid colliding with the existing driver.
- Do not create extra public APIs beyond the copied FastAPI routes unless needed for this isolated package.

2. Trim and specialize `pid_driver/serial_backend.py`.
- Remove generic runtime config and sensor/job-specific command builder branches from the PID driver copy.
- Keep only accepted command names: `status`, `setmotor`, `stopmotor`, `stop`, `setposition`, `getposition`, `setspeed`, `getspeed`, `get_pidmode`, `setpid`, `getpid`, `setoffset`.
- Keep `raw()` for the interactive console, because the user requested a console.
- Add rigid `_build_command()` cases:
  - `setspeed <motor_id> <speed>` with integer speed.
  - `getspeed <motor_id>`.
  - `setpid <motor_id> <kp_milli> <ki_milli> <kd_milli>` with non-negative integer gains.
  - `getpid <motor_id>`.
- Add parse handling in `_parse_ok()` for `SPEED`, `SETSPEED`, `PID`, and `SETPID`, storing latest speed and gains in the snapshot.
- Keep existing `PIDMODE`, `POSITION`, `SETPOSITION`, `SETOFFSET`, `SETMOTOR`, `STOP`, and `STOPMOTOR` parsing.
- Prefer extending `SerialSnapshot` with `speed: dict[str, Any]` and `pid: dict[str, Any]` rather than overloading `position` further.
- Add concise comments only at changed blocks explaining the reason, for example why the command set is intentionally narrow and why no fallback parser is added.

3. Update `pid_driver/static/index.html` into a focused PID UI.
- Remove UI cards for job control, sensors, generic runtime config, and old PID step-return tuning.
- Keep or add:
  - Serial connection card with `Connect`, `Disconnect`, and `Stop All`.
  - Runtime state/status summary card with a `Status` button.
  - Motor selector/input.
  - Raw motor card for `setmotor` and `stopmotor`.
  - PID mode card showing current mode plus a `Refresh Mode` button that sends `get_pidmode`.
  - Position controls shown/enabled only when mode is `position`: target input, `Set Position`, `Set Offset`, poll interval, start/stop poll button, latest position readout.
  - Speed controls shown/enabled only when mode is `speed`: target input, `Set Speed`, poll interval, start/stop poll button, latest speed readout.
  - PID gains card with KP/KI/KD inputs, `Set PID`, `Get PID`, and readout of latest returned gains.
  - Graph card with a canvas, current plotted signal label, clear button, and sample count.
  - Serial log and raw console.
- Keep mobile responsive behavior using the existing grid/card design.
- Add HTML comments sparingly only around removed/replaced areas if useful; avoid clutter.

4. Update `pid_driver/static/app.js` for mode-aware behavior.
- Rename poll state from position-specific to generic signal polling, for example `pollTimer`, `pollInFlight`, `pollMetric`, and `samples`.
- On successful serial connect, automatically enqueue `get_pidmode <current motor>` once after rendering the connected snapshot. Use the existing frontend command queue; do not add response correlation.
- Add a `Refresh Mode` button that sends `get_pidmode <current motor>`.
- When `pidmode` updates:
  - Store `state.pidMode` as exactly `position`, `speed`, or unknown/empty.
  - Stop current polling if mode changes.
  - Enable position controls only in `position` mode.
  - Enable speed controls only in `speed` mode.
  - Set graph label to the current mode’s metric.
- Position polling sends only `getposition <motor>`.
- Speed polling sends only `getspeed <motor>`.
- Manual commands should still wait while polling is in flight, following the current command queue pattern.
- `setpid` validates non-negative integer gains and sends the firmware command directly, not `setconfig`.
- `getpid` refreshes gain readouts from `OK PID` parsing.
- Keep `raw` console send unchanged except for labels, because it is intentionally unrestricted interactive access.
- Add comments at changed blocks explaining why mode gating exists and why graph samples come only from polled reads.

5. Add graphing with minimal code.
- Use a plain `<canvas>` and browser 2D context; do not add third-party chart dependencies.
- Append a sample only when a parsed polled metric is received:
  - In position mode, sample `position.pos ?? position.position` after `getposition` responses update the snapshot.
  - In speed mode, sample `speed.speed` after `getspeed` responses update the snapshot.
- Store samples as `{t, value}` where `t` is `Date.now()`.
- Cap samples to a fixed count such as 600 to keep rendering cheap.
- Redraw after each sample and on window resize.
- Draw axes, a min/max-scaled line, and a current value label; no smoothing or advanced statistics.
- Add `Clear Graph` to reset samples without stopping polling.
- Ensure graph works when values are negative and when all values are equal.
- Add comments explaining the sample cap and canvas-only choice because the graph can become heavy.

6. Update `pid_driver/static/styles.css`.
- Reuse the existing visual language.
- Add styles for mode-specific hidden/disabled panels, graph canvas sizing, and graph metadata.
- Keep responsive rules simple and compatible with the existing mobile layout.

7. Keep backend API routes simple.
- Existing `/api/serial/connect`, `/api/serial/disconnect`, `/api/serial/command`, `/api/serial/raw`, `/api/serial/snapshot`, `/health`, and `/ws` are sufficient.
- Do not add malformed-data recovery, retries, or fallback parsing.
- Do not add persistence or local storage.

## Safety And Behavior Rules
- `Stop All` must stay visible in the connection area.
- `Stop Motor` must stay visible near raw motor output controls.
- Polling must stop on disconnect.
- Polling should not run unless serial is connected and PID mode is known as `position` or `speed`.
- Mode refresh is explicit after initial connect; do not repeatedly auto-query mode except the single connect-time check.
- Raw console remains available but should be visually marked as advanced/dangerous.

## Validation Plan
1. Run Python syntax checks:
- `python3 -m compileall pid_driver`.

2. Run JavaScript syntax checks:
- `node --check pid_driver/static/app.js`.

3. Exercise command builder directly in a short Python snippet or unit-style shell command:
- `status`.
- `setmotor M0 64 0`.
- `stopmotor M0`.
- `stop`.
- `get_pidmode M0`.
- `setposition M0 100`.
- `getposition M0`.
- `setspeed M0 250`.
- `getspeed M0`.
- `setpid M0 500 0 50`.
- `getpid M0`.
- `setoffset M0 0`.

4. Run the app locally without hardware:
- `python3 -m pid_driver --port 8081`.
- Open the page and confirm it renders, disconnected controls are disabled except connect fields, graph is visible, and raw console is present.

5. With hardware on a safe rig:
- Connect serial and confirm one `get_pidmode <motor>` is sent automatically.
- In position mode, confirm position controls are enabled, speed controls are disabled, polling sends `getposition`, and graph plots position.
- In speed mode, confirm speed controls are enabled, position controls are disabled, polling sends `getspeed`, and graph plots speed.
- Use `Refresh Mode` after changing firmware/controller mode and confirm UI switches.
- Test `setpid`/`getpid` and confirm gain readouts update from `OK SETPID`/`OK PID`.
- Test `setoffset`, `setmotor`, `stopmotor`, and `stop`.

## Out Of Scope
- Modifying firmware commands.
- MQTT, jobs, sensors, generic runtime config, old step-return PID tuning, or automated tuning algorithms.
- Persisting graph history or PID gains.
- Adding a charting library.
- Adding new header functions or firmware public APIs.
