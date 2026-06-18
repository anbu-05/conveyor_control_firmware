# Plan: Reverse Direction And RSSI Commands

## Goal

Add runtime commands over serial and MQTT to:

- Flip the conveyor’s logical travel direction between `S0_TO_S1` and `S1_TO_S0`.
- Query the ESP32 station RSSI.

## Current Code Findings

- High-level serial commands are parsed in `main/tasks/command_task.c` and queue `conveyor_cmd_t` commands for jobs.
- MQTT commands are parsed literally in `main/tasks/mqtt_task.c` from `conveyor/C0/cmd`; feedback is published to `conveyor/C0/feedback`.
- The conveyor job state machine in `main/conveyor/conveyor_job.c` currently hardcodes sensor mapping:
  - TX watches `S1` via `tx1_sensor()`.
  - RX first waits for `S0` via `rx0_sensor()`, then watches `S1` via `rx1_sensor()`.
- `start_motor()` in `main/shared/app_state.c` always uses compile-time `CONVEYOR_MOTOR_FORWARD_DIRECTION`, so jobs always drive the physical motor in one direction.
- WiFi is initialized in `mqtt_task.c`; ESP-IDF RSSI can be read with `esp_wifi_sta_get_ap_info()` and `wifi_ap_record_t.rssi`.

## Direction Design

Introduce one shared runtime travel-direction setting with two values:

- `S0_TO_S1`: existing/default behavior.
- `S1_TO_S0`: reversed behavior.

Implementation approach:

- Add a small enum and helpers in `main/shared/app_state.h` / `main/shared/app_state.c`:
  - `conveyor_travel_direction_t`
  - `conveyor_get_travel_direction()`
  - `conveyor_set_travel_direction()`
  - `conveyor_travel_direction_name()`
  - `conveyor_get_rssi()` or equivalent RSSI helper, if shared by serial and MQTT.
- Initialize direction to `S0_TO_S1` so current behavior is preserved after boot.
- Update `start_motor()` to choose the sign of `target_speed` from the current travel direction:
  - `S0_TO_S1` uses `CONVEYOR_MOTOR_FORWARD_DIRECTION`.
  - `S1_TO_S0` uses the opposite motor direction.
- Update `conveyor_job.c` sensor mapping helpers:
  - `tx1_sensor()` returns `S1` for `S0_TO_S1`, `S0` for `S1_TO_S0`.
  - `rx0_sensor()` returns `S0` for `S0_TO_S1`, `S1` for `S1_TO_S0`.
  - `rx1_sensor()` returns `S1` for `S0_TO_S1`, `S0` for `S1_TO_S0`.
- Add direction to job/tray/status feedback where practical, so remote clients can confirm the current direction.
- Reject direction changes while the job state is not `IDLE` to avoid changing sensor/motor semantics mid-transfer.

## Serial Commands

Add strict literal commands in `execute_command()`:

- `setdirection s0tos1`
  - Requires `argc == 2`.
  - Requires conveyor idle; otherwise `ERR JOB_BUSY` or `ERR CONFIG_BUSY`.
  - Sets travel direction to `S0_TO_S1`.
  - Prints `OK SETDIRECTION S0_TO_S1`.
- `setdirection s1tos0`
  - Same behavior, reversed.
  - Prints `OK SETDIRECTION S1_TO_S0`.
- `getdirection`
  - Requires `argc == 1`.
  - Prints `DIRECTION C0 S0_TO_S1` or `DIRECTION C0 S1_TO_S0`.
- `getrssi`
  - Requires `argc == 1`.
  - On success prints `RSSI C0 <dbm>`.
  - If WiFi is not connected or RSSI cannot be read, prints `ERR RSSI_UNAVAILABLE`.

Reuse existing error style:

- `ERR BAD_ARGS` for wrong command shape or unknown direction text.
- `ERR JOB_BUSY` or `ERR CONFIG_BUSY` for non-idle direction changes. Prefer `ERR JOB_BUSY` because it is already used for job-level state conflicts.

## MQTT Commands

Add literal JSON payloads on `conveyor/C0/cmd`:

- `{"type":"setdirection","value":"s0tos1"}`
- `{"type":"setdirection","value":"s1tos0"}`
- `{"type":"getdirection"}`
- `{"type":"getrssi"}`

MQTT behavior:

- Direction changes reject with existing `publish_bad_command("JOB_BUSY")` if not idle.
- Bad direction value returns `BAD_VALUE`.
- Successful direction set publishes feedback like:
  - `{"id":"C0","direction":"S0_TO_S1"}`
  - `{"id":"C0","direction":"S1_TO_S0"}`
- `getdirection` publishes the same direction feedback.
- `getrssi` publishes feedback like:
  - `{"id":"C0","rssi":-55}`
- RSSI read failure publishes `RSSI_UNAVAILABLE` via `publish_bad_command()`.

## Files To Change

- `main/shared/app_state.h`
  - Add travel-direction enum and helper declarations.
  - Add RSSI helper declaration if implemented centrally.
- `main/shared/app_state.c`
  - Store runtime travel direction.
  - Implement direction helpers.
  - Update `start_motor()` to use current travel direction.
  - Optionally implement `get_rssi` helper with `esp_wifi_sta_get_ap_info()`.
- `main/conveyor/conveyor_job.h`
  - Optionally extend `conveyor_status_t` and `conveyor_tray_status_t` with direction.
- `main/conveyor/conveyor_job.c`
  - Add current direction to status/tray status if struct is extended.
  - Update `tx1_sensor()`, `rx0_sensor()`, and `rx1_sensor()` to map based on current direction.
- `main/tasks/command_task.c`
  - Add serial parsing for `setdirection`, `getdirection`, and `getrssi`.
  - Include `esp_wifi.h` if RSSI is read directly here, or call a shared helper.
- `main/tasks/mqtt_task.c`
  - Add MQTT payload handling for `setdirection`, `getdirection`, and `getrssi`.
  - Add small publish helpers for direction/RSSI JSON.
- Docs:
  - `docs/serial-debug-commands.md`: document new serial commands and outputs.
  - `docs/mqtt-control-commands.md`: document new MQTT payloads and feedback.
  - `docs/conveyor-state-machine.md`: update sensor mapping to mention runtime direction.
  - `project.md`: add a short status note and update quick command/output examples.

## Verification

- Build with `idf.py build`.
- Static behavior checks from source:
  - Default `S0_TO_S1` keeps existing TX/RX mapping and motor direction.
  - `S1_TO_S0` swaps TX/RX terminal sensors and reverses motor speed sign.
- Manual serial tests on hardware:
  - `getdirection`
  - `setdirection s1tos0`
  - `jobtx` with a tray present should move toward `S0` and stop/complete using `S0`.
  - `jobrx` with no tray should wait at `S1`, start when `S1` detects, then stop at `S0`.
  - `getrssi` should print `RSSI C0 <dbm>` when connected.
- Manual MQTT tests:
  - `mosquitto_pub -h 192.168.1.126 -t conveyor/C0/cmd -m '{"type":"setdirection","value":"s1tos0"}'`
  - `mosquitto_pub -h 192.168.1.126 -t conveyor/C0/cmd -m '{"type":"getdirection"}'`
  - `mosquitto_pub -h 192.168.1.126 -t conveyor/C0/cmd -m '{"type":"getrssi"}'`
  - Confirm feedback appears on `conveyor/C0/feedback`.

## Open Decisions

- Direction persistence is not included in this plan. Direction resets to `S0_TO_S1` after reboot unless persistence is explicitly required.
- The planned command names are `setdirection`, `getdirection`, and `getrssi`; this matches the existing strict lowercase serial style and compact MQTT payload style.
