# Autodoor Lean Firmware Rewrite Plan

## Goal

Rewrite the ESP-IDF autodoor firmware in place as a lean, uniform implementation aligned with the conveyor project architecture, while preserving the essential autodoor behavior:

- ESP console control and diagnostics.
- Close-limit homing to position `0`.
- First-open travel learning from the open limit.
- Learned `open` and `close` moves after calibration.
- PCNT-backed encoder position.
- PID-based motor control from the start.
- NVS-backed runtime configuration and learned state.
- A separate safety monitor task, added after basic movement works.
- Minimal MQTT command/result/status control, added after local motion and persistence are working.

This is a firmware-only plan. Dashboard/backend changes are out of scope.

## Fixed Decisions

- Rewrite in place, not side-by-side.
- Use conveyor as the consistency reference for structure and data flow.
- Use `esp_console`, not `microrl`.
- Use PID control from the first motion implementation.
- Use NVS-backed runtime config for PID, speed limits, timeouts, and learned travel/state.
- Preserve the existing autodoor MQTT namespace shape:
  - `factory/<machine_id>/autodoor/command`
  - `factory/<machine_id>/autodoor/result`
  - `factory/<machine_id>/autodoor/node_status`
- Move to a minimal backend-facing MQTT contract after core movement works locally.
- Remove chuck control entirely: no chuck module, relay command, chuck persistence, chuck MQTT, or chuck console command.
- Leave detailed debug data to `esp_console`, not normal MQTT status.
- Keep current calibration flow:
  - `home` seeks close limit and sets encoder position to `0`.
  - first `open` learns travel by seeking open limit.
  - later `open` and `close` use learned targets.

## Architecture Target

Use a conveyor-like module layout, but name the state machine explicitly:

```text
autodoor/main/
  main.c
  config/
    config.h
    runtime_config.h
    runtime_config.c
  shared/
    app_state.h
    app_state.c
  statemachine/
    statemachine.h
    statemachine.c
  tasks/
    console_task.h
    console_task.c
    mqtt_task.h
    mqtt_task.c
    hardware_task.h
    hardware_task.c
    motor_pid_task.h
    motor_pid_task.c
    safety_task.h
    safety_task.c
  store/
    door_store.h
    door_store.c
```

Naming can be adjusted during implementation, but preserve these boundaries and explicit names:

- `statemachine.c` is the door state machine implementation.
- `statemachine_task()` is the FreeRTOS task that consumes state-machine events.
- `statemachine_send_event()` is the main queue entrypoint for MQTT, console, and safety.

Do not use vague names like `door_job` if they hide that this is the actual state machine.

## Core Ownership Rules

- `main.c` only initializes services, config, hardware abstractions, queues, and tasks.
- All task/thread startup sequencing belongs in `main.c`; modules expose init functions and task entrypoints, but `xTaskCreate()` is called only from `main.c` so boot order is visible in one place.
- `statemachine/statemachine.c` owns high-level door states, transitions, command acceptance, calibration flow, and motion completion/failure decisions.
- The state machine is its own FreeRTOS task/thread: `statemachine_task()`.
- MQTT and console are input producers only. They parse external input into typed events and submit those events to the state-machine queue.
- The state machine owns normal door motion intent.
- PID/motor task owns translating target position/speed into direction/PWM hardware output.
- Motor driver only applies direction/PWM and immediate stop.
- Safety task monitors limit switches, encoder, timeouts, direction, stalls, and emergency stop conditions after the base state machine works.
- Safety task normally sends typed fault events to the state-machine queue.
- Safety task may directly cut PWM only for emergency stop or impossible unsafe conditions, and must also notify the state machine.
- Limit-switch GPIO state is stored directly in the motor struct; higher-level code can qualify it later if needed.
- MQTT `node_status` must stay simple and backend-facing; detailed internal state belongs to console diagnostics.

## Event Queue Example

The event queue is the typed boundary for commands and faults going into the state machine. It is not the motor-control queue.

Example `open` command from MQTT:

```text
MQTT receives:
{"command_id":"cmd_42","command":"open"}

mqtt_task.c parses it into:
door_event_t {
  source = DOOR_EVENT_SOURCE_MQTT
  type = DOOR_EVENT_COMMAND
  command = DOOR_COMMAND_OPEN
  command_id = "cmd_42"
}

mqtt_task.c calls:
statemachine_send_event(&event)

statemachine_task() receives the event:
- checks current state
- rejects if busy, unhomed, or in error
- stores active command_id = "cmd_42"
- changes state to OPENING
- requests a motor/PID target:
  motor_control_set_position_target(open_target_counts, open_speed_counts_per_sec)

motor_pid_task() runs independently:
- reads encoder PCNT position
- computes PID output
- calls set_motor(direction, pwm)

when target is reached:
- statemachine_task() changes state to OPEN
- publishes or requests result:
  command_id="cmd_42", command_status="success", message="open complete"
- publishes or requests node_status:
  status="open"
```

Console and safety use the same event queue. Motor/PID control is a separate API because PID needs to run continuously and should not be driven by occasional command events.

## Code Commenting Requirement

The rewrite must include necessary comments because the current code is difficult to follow without explanations of ownership, relationships, and naming.

**Strict rule: comments are part of each checkpoint's implementation. Do not mark a checkpoint complete until its new/changed modules, public APIs, config parameters, ownership boundaries, and non-obvious functions are commented. Prefer short `//` comments; avoid large `/* ... */` blocks unless the file already requires that style.**

Required comments:

- Every module needs a short top-of-file responsibility comment.
- Important public structs, enums, and functions need comments explaining ownership and who is allowed to call them.
- Important internal helper functions need short comments when they centralize validation, persistence, locking, hardware behavior, or cross-task boundaries.
- Config constants and runtime parameters need short comments explaining what they change and their units.
- Function names that encode a design decision or domain meaning need a short comment explaining why that name was chosen.
- Non-obvious hardware choices need comments explaining why the code does it that way, not just what it does.
- Cross-task relationships need comments, especially:
  - MQTT/console/safety produce events.
  - `statemachine_task()` consumes events and owns state transitions.
  - PID task owns repetitive motor output.
  - Motor driver only applies direction/PWM.
  - Safety can cut PWM only for emergency stop.
- Avoid noisy comments that restate simple code.

Example of useful name rationale:

```c
// Named statemachine_send_event() instead of door_open()/door_close() because
// external producers must not call state transitions directly. All outside
// requests enter through the same queue and are serialized by statemachine_task().
bool statemachine_send_event(const door_event_t *event);
```

## Minimal MQTT Contract

MQTT is implemented after local movement, safety, and persistence restore.

Command topic:

```text
factory/<machine_id>/autodoor/command
```

Command envelope:

```json
{
  "command_id": "cmd_123",
  "command": "open"
}
```

Supported MQTT commands:

```text
ack_test
home
open
close
stop
clear_error
get_commands
```

Result topic:

```text
factory/<machine_id>/autodoor/result
```

Every valid command publishes `received` first:

```json
{
  "command_id": "cmd_123",
  "command_status": "received",
  "message": "command received"
}
```

Final result example:

```json
{
  "command_id": "cmd_123",
  "command_status": "success",
  "message": "open complete"
}
```

Allowed `command_status` values:

```text
received
success
failure
```

Node status topic:

```text
factory/<machine_id>/autodoor/node_status
```

Simple status shape:

```json
{
  "id": "cnc2",
  "status": "opening"
}
```

Initial statuses:

```text
unknown
idle
unhomed
homing
closed
opening
open
closing
stopped
error
```

`node_status` should publish on state changes and important fault/stop transitions, not continuously.

## Console Contract

Use `esp_console` over the ESP-IDF monitor console.

Initial high-level commands:

```text
home
open
close
stop
clearerror
status
get_limit_switches
getencoder
getmotor
getconfig
setconfig <key> <value>
resetconfig
setkp <value>
setki <value>
setkd <value>
resetpid
```

Guarded raw motor commands may exist for commissioning only:

```text
unlockraw <token-or-confirmation>
setmotor <direction> <pwm>
stopmotor
lockraw
```

Raw motor rules:

- Disabled by default after boot.
- Rejected while the door is moving.
- Rejected unless explicitly unlocked.
- `stop` must always be accepted and must relock raw control.

Console output style should follow conveyor-style literal lines where practical:

```text
READY autodoor
OK HOME
ERR JOB_BUSY
ERR BAD_ARGS
STATUS cnc2 OPEN pos=1234
LIMITS open=0 close=1 raw_open=0 raw_close=1
ENCODER pos=1234 speed=56
CONFIG pid_kp_milli 500
```

## Runtime Config

Follow conveyor's runtime config pattern:

- Compile-time defaults in `config/config.h`.
- NVS-backed active values in `config/runtime_config.c`.
- `getconfig`, `setconfig`, and `resetconfig` through console.
- Reject config changes while moving, except read-only queries.
- Store integer fixed-point PID gains, for example milli-units.

Initial editable keys should include:

```text
pid_kp_milli
pid_ki_milli
pid_kd_milli
max_pwm
min_start_pwm
home_speed_counts_per_sec
open_speed_counts_per_sec
close_speed_counts_per_sec
seek_speed_counts_per_sec
position_tolerance_counts
home_timeout_ms
open_timeout_ms
close_timeout_ms
stall_check_ms
stall_min_delta_counts
direction_check_delay_ms
limit_switch_qualify_ms
mqtt_status_period_ms
```

Learned/calibrated values should be stored separately from editable config:

```text
travel_counts
open_target_counts
close_target_counts
last_door_state
last_position_counts
homed flag
travel_learned flag
```

## Implementation Checkpoints

Each checkpoint should leave the firmware buildable and testable before continuing.

### 1. Baseline Snapshot And Contract Notes

- Record current branch and working tree state before deleting firmware code.
- Keep current code as git history/reference only.
- Confirm target chip remains ESP32-S3 from `sdkconfig`.
- Confirm current machine defaults for `cnc1` and `cnc2` before replacing files.

Validation:

- `git status --short --branch`
- Note current machine config values for pins, direction levels, WiFi, MQTT URI, PWM frequency, and control period.

### 2. Replace File Structure And Build Skeleton

- Replace the large firmware structure with the new module layout.
- Update `main/CMakeLists.txt` to list new files and include dirs.
- Create `main.c` with minimal startup and `READY autodoor` console print.
- Add empty/stub module APIs for config, shared state, state machine, console, limit switches, encoder, motor, PID, safety, MQTT, and store.
- Use `statemachine.*` names from the start.

Validation:

- `idf.py build` succeeds.
- Serial monitor prints `READY autodoor`.

### 3. Config And Runtime Config

- Implement compile-time config defaults for machine identity, pins, WiFi, MQTT, control periods, PID defaults, speed defaults, and safety timeouts.
- Implement machine config validation outside the state machine.
- Implement runtime config load/save/reset through NVS.
- Implement console `getconfig`, `setconfig`, `resetconfig`, PID helper commands.

Validation:

- Bad config values are rejected at boot or by `setconfig`.
- `getconfig` prints all editable values.
- `setconfig` persists across reboot.
- Build remains clean.

### 4. Shared State And Central Event Queue

- Define typed door commands/events:
  - `DOOR_CMD_ACK_TEST`
  - `DOOR_CMD_HOME`
  - `DOOR_CMD_OPEN`
  - `DOOR_CMD_CLOSE`
  - `DOOR_CMD_STOP`
  - `DOOR_CMD_CLEAR_ERROR`
  - `DOOR_CMD_GET_COMMANDS`
  - `DOOR_EVT_SAFETY_FAULT`
  - `DOOR_EVT_EMERGENCY_STOP`
- Include optional `command_id` for future MQTT-originated commands.
- Create the central FreeRTOS queue consumed by `statemachine_task()`.
- Add `statemachine_send_event()` used by console first, then safety and MQTT later.
- Keep shared mutable state minimal and protected. Tasks may directly edit `door_motor`, but must hold `motor_mutex` for multi-field reads/writes or any consistency-sensitive access.

Validation:

- Console can queue `ack_test` or equivalent and receive an `OK` response.
- Queue full behavior returns a clear error.
- Comments explain why external producers submit events instead of calling transition functions directly.

### 5. Hardware Access And Verification

Implement and verify the basic hardware access code together. The encoder and
physical limit switches belong to the door motor axis, so they are owned by the
combined hardware task instead of separate encoder/limit-switch threads:

- Motor GPIO/LEDC output:
  - direction GPIO
  - PWM output
  - immediate stop
  - safe direction-change handling
- Physical limit-switch sampling:
  - GPIO input setup
  - direct open/close GPIO levels stored in the motor struct
- PCNT encoder:
  - ESP32-S3 PCNT-backed quadrature counting
  - store latest count in the motor struct
  - set/reset count later if needed by homing
  - speed estimate later if needed by PID
- Guarded raw console verification commands.

Target motor API shape:

```c
esp_err_t hardware_motor_init(void);
esp_err_t hardware_encoder_init(void);
esp_err_t hardware_limit_switch_init(void);
esp_err_t hardware_init(void);
void hardware_task(void *arg);
esp_err_t set_motor(int direction, int pwm);
void stop_motor(void);
```

Target `door_motor_t` shape includes `int direction`, `int pwm`, encoder count,
current/target position, current/target speed, `bool PID_control`,
`int open_limit_switch`, and `int close_limit_switch`.
The struct is exposed from `app_state`; tasks edit it directly while holding
`motor_mutex` instead of adding one app-state setter per field.

Validation:

- `get_limit_switches` shows direct open/close GPIO levels.
- `getencoder` shows count changes while manually moving or low-power jogging.
- Guarded raw console commands can set low PWM and stop.
- Direction changes force PWM to zero before switching direction.
- `stop_motor()` always cuts PWM.
- No GPIO ISR quadrature counting remains.
- No semantic open/close/seek/backoff functions exist in the motor driver.

### 6. PID Motor Control Task

- Implement simple position PID control from the start.
- Inputs:
  - current offset-corrected position
  - target position
  - runtime PID gains and PWM limits
- Outputs:
  - direction/PWM through motor hardware layer
- Include integral clamping and output clamping.
- Include safe direction reversal behavior.
- Keep PID math local to `motor_pid_task.c`; the loop is simple enough that a
  PID dependency is unnecessary.
- Keep PID gain defaults/NVS ownership in `runtime_config`; PID loads the live
  startup gains from runtime config and `setk()` only updates live float gains.
- Add `position_offset` so `current_position = encoder_count + position_offset`.
- Expose `set_position()`, `get_position()`, `set_offset()`, and `setk()`.
- Treat `PID_control` as user-owned. PID and helper functions read it but
  never change it.
- Treat speed as encoder counts/sec and PWM as the final hardware duty value.
- When `PID_control` is false, pass signed `target_speed` through the
  speed-to-PWM mapper instead of running PID.
- When `PID_control` is true, PID output is signed speed in counts/sec;
  only the final motor-output helper maps speed linearly to PWM.
- Use runtime `RUNTIME_CONFIG_MAX_SPEED_COUNTS_PER_SEC` as the full-scale speed
  that maps to the configured max PWM; the default is 20000 counts/sec.
- The PID task must use `set_motor()` only; `stop_motor()` remains a direct user
  or safety control path, not part of normal PID output.
- Expose read-only motor/PID diagnostics through console.

Validation:

- With motor disabled or low power, target changes produce bounded PWM.
- `stop` clears target and output.
- PID gains can be changed through console while idle.
- Comments explain why PID task owns repetitive motor output instead of the state machine directly writing PWM each tick.

### 7. State Machine And Movement Behavior

This is one topic, implemented step by step. Do not implement all subtasks at once. Build and test after each subtask.

Subtasks:

1. State-machine skeleton:
   - implement `statemachine_task()`
   - implement states:
     - `UNHOMED`
     - `HOMING`
     - `CLOSED`
     - `OPENING`
     - `OPEN`
     - `CLOSING`
     - `STOPPED`
     - `ERROR`
   - implement command acceptance/rejection rules
   - emit console state events

2. `stop` and `clear_error`:
   - `stop` works from every state and clears motor/PID output
   - `clear_error` only clears valid error/stopped conditions
   - busy and invalid-state commands are rejected clearly

3. `home`:
   - allowed from safe states
   - move toward close limit using PID seek target/speed mode
   - stop when close qualified limit activates
   - set encoder count to `0`
   - mark `homed=true`
   - persist state
   - enter `CLOSED`

4. First-open travel learning:
   - require homed unless a deliberate config says otherwise
   - move toward open limit
   - stop when open qualified limit activates
   - record `travel_counts` from current position
   - set `travel_learned=true`
   - persist learned travel
   - enter `OPEN`

5. Learned `open`:
   - target learned open position or configurable open lead if needed
   - use PID target move
   - finish when within `position_tolerance_counts`
   - enter `OPEN`
   - if open limit activates before target, treat it as a clearly reported fault unless a later correction rule is explicitly added

6. `close`:
   - require homed
   - move toward position `0` or configurable pre-limit close target if needed
   - finish within tolerance
   - if close limit activates, stop, zero position, enter `CLOSED`
   - persist final state

7. Safety task and safety events:
   - add after the base state machine and movement behavior work locally
   - monitor emergency stop, impossible limit combinations, encoder direction mismatch, stall/jam, motion timeouts, and unsafe limit activation
   - send typed events into the state-machine queue
   - directly cut PWM only for emergency stop or impossible unsafe conditions, and notify the state machine

Validation:

- State-machine comments explain why each transition is allowed or rejected.
- `stop` works from every state.
- `home` reaches close limit and sets position `0`.
- First `open` learns positive travel and persists it.
- Learned `open` reaches target without needing limit contact.
- `close` reaches closed state safely and zeroes on close-limit contact.
- Timeout/stall/fault paths stop and report failure after safety is added.
- Safety task records fault reason in the state machine.

### 8. Persistence Restore

- Restore learned travel, homed/travel flags, last persistable state, and last position.
- Do not restore transient moving states as moving.
- If persisted state is inconsistent, boot as `UNHOMED` or `ERROR` with a clear console diagnostic.
- Do not move at boot from restored state. Boot restore is informational/stateful only.

Validation:

- Reboot after home/open/close restores expected state.
- Corrupt or invalid saved values do not cause motion at boot.
- Console status clearly shows restored state and whether travel was learned.

### 9. MQTT Minimal Contract

Implement MQTT only after safety, homing, open travel learning, learned open, close, and persistence restore are working through console/local control.

- Implement WiFi/MQTT setup in `tasks/mqtt_task.c`.
- Subscribe only to `factory/<machine_id>/autodoor/command`.
- Publish to `result` and `node_status`.
- Parse JSON using cJSON.
- Convert MQTT payloads into the same typed events used by console.
- Support `ack_test`, `home`, `open`, `close`, `stop`, `clear_error`, `get_commands`.
- Keep command parsing strict enough to avoid ambiguous behavior, but not exact-string-only.

Validation:

- `ack_test` publishes `received`.
- Motion commands publish `received` then final `success` or `failure`.
- `stop` publishes `received` then final `success` or `failure`.
- Bad JSON publishes `failure`.
- `get_commands` returns the supported command list.
- MQTT commands use the same state-machine event path as console commands.

### 10. Final Cleanup

- Remove old oversized files and obsolete headers not used by the rewrite.
- Remove chuck persistence and command remnants.
- Remove old debug MQTT/get_topics code unless intentionally retained.
- Ensure `CMakeLists.txt` references only new modules.
- Keep comments that explain ownership, naming, relationships, and hardware rationale.
- Update firmware docs only in a later documentation pass, not during this implementation unless explicitly requested.

Validation:

- `idf.py build` succeeds from clean checkout.
- No references remain to chuck commands, chuck state, or `microrl`.
- No semantic motor mode helpers remain in the motor driver.
- Source comments are useful and not just restatements of simple code.

### 11. End-To-End Hardware Validation

Run with the motor mechanically safe or disabled first.

Validation sequence:

1. Boot and verify `READY autodoor`.
2. Verify `getconfig`, `get_limit_switches`, `getencoder`, `getmotor`.
3. Verify limit-switch polarity before motion.
4. Verify PCNT count direction by manually moving or low-power jogging.
5. Verify guarded raw motor stop and direction behavior.
6. Tune low PID gains with motor unloaded or mechanically safe.
7. Test `stop` and emergency stop before homing.
8. Run `home` at low speed.
9. Run first `open` travel learn at low speed.
10. Run learned `close`.
11. Run learned `open`.
12. Reboot and verify NVS restore.
13. Test induced faults: timeout, stall/no encoder movement, wrong direction, limit activation.
14. Test MQTT `ack_test`, `home`, `open`, `close`, `stop`, `clear_error`.

## Risks And Watchpoints

- PID from start is more tuning-sensitive than the old S-profile. Start conservative and require console tuning before normal operation.
- Replacing in place removes side-by-side comparison during implementation. Use git history as reference and keep each checkpoint buildable.
- Safety task and PID task can fight over motor output if ownership is unclear. Enforce the emergency-stop-only direct cut rule.
- MQTT compatibility intentionally changes: no chuck/debug/get_topics topics in the minimal rewrite. Dashboard/backend may need later changes, but those are out of scope now.
- Runtime config must reject unsafe edits while moving.
- Treat direct limit-switch GPIO levels carefully; any later motion decision should document whether it uses raw GPIO state or a qualified state.
- Do not move at boot from restored state.

## Definition Of Done

- Firmware builds cleanly.
- New code structure matches the planned module boundaries.
- State-machine code is explicitly named `statemachine.*` and runs in `statemachine_task()`.
- The event queue is the single command/fault entrypoint into the state machine.
- Source comments explain module responsibilities, function naming rationale where needed, cross-task relationships, and non-obvious hardware choices.
- `door_controller.c` is gone or reduced to no meaningful old state-machine code.
- `esp_console` supports control, diagnostics, runtime config, and guarded raw motor commissioning.
- PCNT is used for encoder counting.
- PID controls door motion.
- Home, first-open learn, learned open, close, stop, clear_error, safety faults, and persistence restore work through console/local control.
- MQTT supports the minimal command/result/node_status contract after local behavior works.
- Chuck control is absent.
