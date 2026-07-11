# Serial-Only Python Driver Plan

## Current Status
The new root-level `driver/` package has been implemented as a serial-only FastAPI web driver using copied/adapted static assets. The follow-up work is to add a dedicated PID tuning provision for iterative KP/KI/KD changes and a repeatable standard movement.

## Goal For This Refinement
Add a PID tuning workflow to the existing serial web driver so the operator can repeatedly adjust `pid_kp_milli`, `pid_ki_milli`, and `pid_kd_milli`, run the same movement, observe position behavior, and iterate quickly without using the generic runtime config controls each time.

## Confirmed Decisions
- Keep this as a driver/web UI feature; no MQTT.
- Do not add firmware commands unless implementation discovers an unavoidable limitation.
- Use existing firmware serial commands:
  - `setconfig pid_kp_milli <value>`.
  - `setconfig pid_ki_milli <value>`.
  - `setconfig pid_kd_milli <value>`.
  - `pid_control <motor> 1` / `pid_control <motor> 0`.
  - `getposition <motor>`.
  - `setposition <motor> <position>`.
  - `stopmotor <motor>` and `stop` for safety.
- Current `pid.c` refreshes PID gain config every PID tick, so `setconfig pid_*_milli` should affect the live PID loop immediately.
- Standard movement should be a relative step plus optional return to start, using the current position as the start point.
- Default motor remains `M0`, but use the UI's current motor input so this works for discovered motor IDs later.

## Existing Code Facts To Respect
- Runtime PID config keys are defined in `main/tasks/console.c` and `main/config/runtime_config.c`:
  - `pid_kp_milli` default `500`.
  - `pid_ki_milli` default `0`.
  - `pid_kd_milli` default `50`.
- `setconfig` currently updates RAM runtime config, not flash persistence.
- `pid.c` reads `pid_kp_milli`, `pid_ki_milli`, and `pid_kd_milli` every PID loop iteration and divides by `1000.0`.
- `setposition` fails with `ERR PID_CONTROL_DISABLED` unless `pid_control <motor> 1` has been sent first.
- The driver already parses `OK POSITION motor=<id> pos=<n>`, `OK PID_CONTROL motor=<id> enabled=<0|1>`, and `OK SETCONFIG <key> <value>`.
- The current driver does not have request/response correlation; UI sequencing should be time/poll based unless implementation adds a lightweight command queue.

## Implementation Tasks
1. Update `driver/static/index.html`.
   - Add a new prominent card, preferably near `PID Control`, titled `PID Tuning`.
   - Include numeric inputs for:
     - `KP milli` with id like `pidTuneKpMilli`, default `500`.
     - `KI milli` with id like `pidTuneKiMilli`, default `0`.
     - `KD milli` with id like `pidTuneKdMilli`, default `50`.
     - `Step counts` with id like `pidTuneStepCounts`, default a conservative value such as `500`.
     - `Poll ms` with id like `pidTunePollMs`, default `100`, minimum `50`.
     - `Settle tolerance counts` with id like `pidTuneTolerance`, default from `position_tolerance_counts` if available or `20`.
     - `Timeout ms` with id like `pidTuneTimeoutMs`, default `5000`.
   - Add controls:
     - `Apply Gains`.
     - `Run Step + Return`.
     - `Abort Tune`.
   - Add compact status/readout fields:
     - Start position.
     - Outbound target.
     - Return target.
     - Current position.
     - Last result.
   - Add a small trial history `<pre>` or list for compact entries like: `kp=500 ki=0 kd=50 start=0 target=500 reached=498 return=1 result=ok`.
   - Keep existing visual language: cards, tune rows, pills, danger styling for abort.

2. Update `driver/static/app.js` selectors and state.
   - Add `state.pidTune` object with fields:
     - `running: false`.
     - `abort: false`.
     - `startPosition`, `outboundTarget`, `returnTarget`, `lastPosition`.
     - `history: []`.
   - Add selectors for the new inputs, buttons, status fields, and history.
   - Disable `Run Step + Return` unless serial is connected and no tune is running.
   - Disable or label buttons during an active run to prevent overlapping workflows.

3. Add gain application helpers in `app.js`.
   - Implement `applyPidTuneGains()` that validates the three milli inputs against the same `CONFIG_LIMITS` as runtime config.
   - Send, in order:
     - `setconfig pid_kp_milli <kp>`.
     - `setconfig pid_ki_milli <ki>`.
     - `setconfig pid_kd_milli <kd>`.
   - After applying, update `state.lastSerialConfig` optimistically only after commands are accepted by the REST API.
   - Keep the serial log showing the underlying commands.

4. Add a command helper that awaits REST acceptance.
   - Existing `sendSerialCommand()` logs failures but does not return details.
   - Add `postSerialCommand(command, args, confirmMessage = "")` that returns the JSON response or throws/logs an error.
   - Keep the existing click handlers using `sendSerialCommand()` if minimal changes are preferred, but use `postSerialCommand()` for the tuning sequence.
   - Do not rely on firmware acknowledgement timing for every command unless adding a robust event correlation layer; REST acceptance means the line was written to serial.

5. Implement position polling helpers.
   - Add `sleep(ms)`.
   - Add `requestPosition(motorId)` that sends `getposition <motor>` and then waits briefly for WebSocket snapshot updates.
   - Add `waitForPositionNear(target, tolerance, timeoutMs, pollMs)`:
     - Loop until timeout or abort.
     - Send `getposition` each poll.
     - Read latest `snapshot.position.pos` rendered through `state`.
     - Return `{ok: true, position}` when `abs(position - target) <= tolerance`.
     - Return `{ok: false, reason: "timeout", position}` on timeout.
     - Return `{ok: false, reason: "aborted", position}` on abort.
   - Store latest position in `state.pidTune.lastPosition` whenever `renderSerialSnapshot()` receives `snapshot.position.pos` or `snapshot.position.position`.

6. Implement `runPidTuneStepReturn()`.
   - Validate inputs.
   - Confirm before starting because the sequence moves hardware.
   - Set `state.pidTune.running = true`, `abort = false`.
   - Apply gains using `applyPidTuneGains()`.
   - Send `pid_control <motor> 1`.
   - Get current position by polling `getposition` until a numeric position is available or fail with a clear message.
   - Set `startPosition` from the current position.
   - Compute `outboundTarget = startPosition + stepCounts`.
   - Send `setposition <motor> <outboundTarget>`.
   - Poll until outbound target is reached within tolerance or timeout/abort.
   - If outbound succeeds and not aborted, send `setposition <motor> <startPosition>`.
   - Poll until start position is reached within tolerance or timeout/abort.
   - On finish, append a trial history row with gains, start, target, final positions, elapsed-ish result text, and success/failure reason.
    - On abort or timeout, send `stopmotor <motor>` and `pid_control <motor> 0` for safety.
    - On normal successful completion, leave `pid_control` enabled by default unless implementation chooses a checkbox `Disable PID after trial`. If adding that checkbox, default it on for safety.

7. Implement abort behavior.
   - `Abort Tune` sets `state.pidTune.abort = true`.
   - Immediately send `stopmotor <motor>` and `pid_control <motor> 0`.
   - Update status to `aborted`.
   - Ensure `finally` path clears `running` and refreshes button disabled state.

8. Add UI synchronization with config values.
   - When `renderSerialSnapshot()` sees `snapshot.config.pid_kp_milli`, `pid_ki_milli`, or `pid_kd_milli`, populate the PID tuning inputs if the user is not actively editing those inputs.
   - Keep the existing generic runtime config dropdown working.

9. Optional but useful: add presets.
   - Add small buttons for `Load Current`, `Default`, and `Half KP` only if implementation remains simple.
   - `Load Current` pulls values from `state.lastSerialConfig`.
   - `Default` fills `500`, `0`, `50`.
   - Do not add localStorage persistence unless explicitly requested later.

10. Update `README.md` after implementation.
   - Add a short subsection under `Serial Web Driver` explaining PID tuning:
     - KP/KI/KD are milli-unit runtime configs.
     - `500` means `0.500` in `pid.c`.
      - The tuning run applies gains, enables PID control, moves by a relative count step, and returns to start.
     - Use `Abort Tune` or `Stop All` if movement is unsafe.

## Safety Requirements
- Require a confirmation before `Run Step + Return` starts.
- `Abort Tune` must remain enabled during a tuning run.
- Any timeout, failed command, or abort should send `stopmotor <motor>` and `pid_control <motor> 0`.
- Keep `Stop All` available in the main connection area.
- Use conservative defaults: low step count and finite timeout.
- Do not auto-repeat trials. The operator should explicitly start each trial.

## Validation Plan
1. Run syntax checks:
   - `python3 -m compileall driver`.
   - `node --check driver/static/app.js`.
2. Run backend command-builder checks for the commands used by tuning:
   - `setconfig pid_kp_milli 500`.
   - `setconfig pid_ki_milli 0`.
   - `setconfig pid_kd_milli 50`.
    - `pid_control M0 1`.
   - `getposition M0`.
   - `setposition M0 500`.
   - `stopmotor M0`.
3. Without hardware:
   - Open the page and verify controls render and are disabled while disconnected except connection controls.
   - Simulate bad connect and confirm errors still show.
4. With hardware on a safe rig:
   - Connect serial.
   - Click `Get All Config` and confirm PID tuning inputs fill from config.
   - Run `Apply Gains` and confirm serial log shows three `setconfig` commands.
    - Run a very small `Step counts` value and confirm the sequence sends `pid_control`, `getposition`, `setposition`, periodic `getposition`, return `setposition`, and final polling.
    - Test `Abort Tune` during movement and confirm `stopmotor` plus `pid_control 0` are sent.
   - Test timeout by using an unreachable target or very short timeout and confirm safe stop behavior.

## Risks And Notes
- The driver cannot perfectly know when a written command has been acknowledged without a request/response correlation layer. Polling `getposition` and observing snapshot changes is acceptable for this UI workflow.
- Current firmware does not emit continuous position updates. The tuning workflow must poll `getposition`.
- `setconfig` changes are RAM-only in current source; this is good for iterative tuning because reboot restores defaults.
- If position direction is reversed or step sign is wrong for the rig, the operator can use a negative `Step counts` value. The UI should allow negative step counts.
- If implementation wants stronger safety, add a checkbox for `Disable PID after trial`, default checked.

## Out Of Scope
- MQTT support.
- Firmware persistence for tuned gains.
- Automated tuning/optimization algorithms.
- Graph plotting of response curves, unless requested later.
- Local storage or export/import of tuning profiles, unless requested later.
