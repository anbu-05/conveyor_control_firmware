# Implement Conveyor MQTT Contract

## Goal
Implement the backend-facing MQTT contract documented in `docs/conveyor-mqtt-topic-system.md` without changing the public plan again unless implementation reveals a contract bug.

## Current Gaps
- `main/config/config.h` still uses old central topics: `node-command` and `node-status`.
- `main/tasks/mqtt_task.c` still accepts old command names `transmit`/`receive` and publishes `success` as the acknowledgement instead of `received`.
- `get_commands` still returns a flat command list and uses old command names.
- `node_status` publishes firmware internals (`state`, elapsed ms, raw sensors) instead of simple machine-level `status` and `has_tray`.
- `conveyor_cmd_t` has only `type`, so MQTT optional params cannot yet be applied per command.
- MQTT payload buffer is currently `256`, too small for the documented `get_commands` schema.

## Decisions
- MQTT optional params are per-command only. They must not update NVS or runtime config.
- If an optional param is omitted, use existing runtime config getters as defaults.
- Keep legacy conveyor-specific topics (`conveyor/C0/...`) working unless they interfere with the new factory contract.
- Do not add `cJSON`; use constrained local JSON field extraction helpers unless an ESP-IDF JSON component is already available in this repo.
- Result payloads must include only `command_id`, `command_status`, and `message`, plus `commands` only for the final `get_commands` success result.

## Implementation Steps
1. Update central topic macros in `main/config/config.h`:
   - `CONVEYOR_MQTT_TOPIC_CENTRAL_COMMAND` -> `factory/conveyor/C0/command`
   - `CONVEYOR_MQTT_TOPIC_CENTRAL_STATUS` -> `factory/conveyor/C0/node_status`
   - Increase `CONVEYOR_MQTT_PAYLOAD_MAX` enough for `get_commands`, or add a separate larger command-list buffer. Use a size large enough for the full documented JSON, for example `2048`.

2. Extend `conveyor_cmd_t` in `main/conveyor/conveyor_job.h` with per-command override fields:
   - `int speed_counts_per_sec`
   - `uint32_t tx_detect_timeout_ms`
   - `uint32_t tx_clear_timeout_ms`
   - `uint32_t rx_detect_timeout_ms`
   - `uint32_t rx_done_timeout_ms`
   - `uint32_t done_hold_ms`
   - Use `0` or explicit boolean flags to mean omitted. Prefer helper functions that return override when nonzero, otherwise runtime config. Because timeout minimums are `1`, `0` is safe as omitted for timeouts. For `speed_counts_per_sec`, documented min is `0`, so use a boolean flag or sentinel such as `-1` if preserving valid zero matters.

3. Update `main/conveyor/conveyor_job.c` to use command-scoped settings:
   - Store the active command settings when starting `tray_transmit` or `tray_receive`.
   - `start_tx()` should start motor with the active command speed if supplied, otherwise `runtime_config_run_speed_counts_per_sec()`.
   - `start_rx()` should save settings before waiting for incoming tray; when the tray arrives and motor starts, use the active command speed.
   - Timeout checks should use active TX/RX override values when nonzero, otherwise runtime config getters.
   - DONE hold should use active command `done_hold_ms` when supplied, otherwise `runtime_config_done_hold_ms()`.
   - Reset active command settings when returning to idle, stopping, clearing error, or entering error/ESTOP as appropriate.
   - Keep serial commands behavior unchanged by initializing command settings to omitted/defaults when serial paths construct `conveyor_cmd_t`.

4. Update motor start support:
   - Current `start_motor("M0")` always uses runtime config speed.
   - Add a small helper such as `start_motor_with_speed("M0", speed)` in `app_state.c/.h`, or adapt conveyor job to set target speed directly through existing shared state safely.
   - Keep existing `start_motor()` for serial/debug behavior and call the new helper from conveyor job when a per-command speed override is present.

5. Update central command names in `mqtt_task.c`:
   - Accept `ack_test`, `tray_transmit`, `tray_receive`, `stop`, `clear_error`, `get_commands`.
   - Stop accepting old `transmit`/`receive` on the central factory topic unless there is a deliberate backward-compatibility requirement.
   - Continue supporting legacy `conveyor/C0/cmd` behavior separately.

6. Add constrained JSON numeric extraction helpers in `mqtt_task.c`:
   - Reuse the existing string extractor style.
   - Add an integer extractor for optional fields by exact field name.
   - Validate ranges before queuing commands:
   - `speed_counts_per_sec`: 0 to 100000
   - timeouts: 1 to 600000
   - `done_hold_ms`: 0 to 60000
   - Reject invalid params with `command_status: "failure"` and a clear `message`, without queuing the job.

7. Implement central command result behavior:
   - `ack_test`: publish `received` immediately, then no final success unless the contract is updated. The current doc flow says `ack_test -> received`.
   - `get_commands`: publish `received` first, then final `success` with the full `commands` schema.
   - `tray_transmit`, `tray_receive`, `stop`, `clear_error`: publish `received` after validation and successful queueing, then final `success`/`failure` when state machine reaches terminal state.
   - Do not use `success` as acknowledgement.
   - For pre-queue validation failures such as missing command_id, unknown command, invalid param, busy, no tray, tray present, no error to clear, or queue full, publish one `failure` result and do not also publish `received`.

8. Update `get_commands` publisher:
   - Replace the flat string array with the documented command objects.
   - Include optional params for `tray_transmit` and `tray_receive` with defaults and min/max values.
   - Populate defaults from compile-time constants or runtime config getters. Prefer runtime config getters so the backend sees the currently active defaults if they were changed over serial/NVS.
   - Ensure the payload fits in the configured buffer.

9. Simplify central `node_status` payload:
   - Map firmware states to backend statuses:
   - `CONVEYOR_STATE_IDLE`, `TX_DONE`, `RX_DONE` -> `idle`
   - `TX_WAIT_FOR_TX1_DETECT`, `TX_WAIT_FOR_TX1_CLEAR` -> `transmitting`
   - `RX_WAIT_FOR_RX0`, `RX_WAIT_FOR_RX1` -> `receiving`
   - `CONVEYOR_STATE_ESTOP` -> `stopped`
   - `CONVEYOR_STATE_ERROR` -> `error`
   - fallback -> `unknown`
   - Publish JSON exactly like: `{"id":"C0","status":"transmitting","has_tray":true}`.
   - Do not include raw sensor values, internal state names, elapsed time, or error detail in `node_status`.
   - Keep internal debug details on serial and legacy feedback topics if needed.

10. Review central command tracking:
   - `central_command_tracker_t` currently tracks only one active central command.
   - Preserve this model for motion commands.
   - For `stop`, if it interrupts an active command, publish failure for the interrupted command, then publish `received` for stop once stop is queued, then final `success` when ESTOP state is reached.
   - For `ack_test` and `get_commands`, do not mark `central_command.active` because they are immediate/non-motion commands.

11. Build and validate:
   - Build with ESP-IDF: `source "/home/anbu/.espressif/v6.0.1/esp-idf/export.sh" >/tmp/kilo/esp-idf-export.log && idf.py build`.
   - Confirm no new dependency on unavailable JSON components.
   - Confirm payload buffers are large enough and `snprintf` truncation is avoided or detected.

## MQTT Test Scenarios
- `ack_test`: command receives one `received` result.
- `get_commands`: command receives `received`, then `success` with command schemas and param defaults.
- `tray_transmit` with no params: uses runtime config defaults and publishes `received -> success/failure`.
- `tray_transmit` with valid params: uses one-shot overrides and does not persist them.
- `tray_receive` with valid params: uses one-shot overrides and does not persist them.
- Any invalid param range: publishes `failure` only and does not queue job.
- `stop` while motion active: publishes failure for interrupted motion, then `received -> success/failure` for stop.
- `clear_error` when not error/stopped: publishes `failure` only.
- `node_status` publishes only `id`, `status`, `has_tray` on state changes.

## Files To Edit
- `main/config/config.h`
- `main/tasks/mqtt_task.c`
- `main/conveyor/conveyor_job.h`
- `main/conveyor/conveyor_job.c`
- `main/shared/app_state.h`
- `main/shared/app_state.c`
- Possibly `docs/conveyor-mqtt-topic-system.md` only if implementation reveals a mismatch that must be corrected.

## Risks
- The documented `get_commands` payload is much larger than current MQTT payload size.
- Manual JSON parsing is fragile; keep accepted payloads simple and reject malformed fields rather than guessing.
- Per-command speed override requires careful synchronization with motor state.
- `ack_test -> received` only may look odd compared with other commands but matches the current document.
