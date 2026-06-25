# Gantry MQTT Factory Contract Plan

## Base Topic
```text
factory/gantry
```

## Topics
```text
factory/gantry/command
factory/gantry/result
factory/gantry/node_status
factory/gantry/feedback
```

## Direction Summary
```text
Master server publishes:
factory/gantry/command

Master server subscribes:
factory/gantry/result
factory/gantry/node_status
factory/gantry/feedback

ESP subscribes:
factory/gantry/command

ESP publishes:
factory/gantry/result
factory/gantry/node_status
factory/gantry/feedback
```

## Command Topic
```text
factory/gantry/command
```

Supported commands:
```text
ack_test
home
move_abs
move_rel
jog
stop
ramp_stop
halt
disable_voltage
fault_reset
```

## Command Examples

### 1. ack_test
Connectivity check only.

Publish to:
```text
factory/gantry/command
```

Payload:
```json
{
  "command_id": "cmd_ack_001",
  "command": "ack_test"
}
```

Expected result:
```json
{
  "command_id": "cmd_ack_001",
  "command": "ack_test",
  "command_status": "received",
  "message": "command received"
}
```

### 2. home
Starts gantry homing.

Publish to:
```text
factory/gantry/command
```

Payload:
```json
{
  "command_id": "cmd_home_001",
  "command": "home",
  "rpm": 100
}
```

Example result:
```json
{
  "command_id": "cmd_home_001",
  "command": "home",
  "command_status": "failure",
  "message": "rejected: homing_in_progress - homing is already in progress"
}
```

### 3. move_abs
Moves to an absolute gantry position (absolute target in drive units).

Publish to:
```text
factory/gantry/command
```

Payload (example):
```json
{
  "command_id": "cmd_move_abs_001",
  "command": "move_abs",
  "pos": 500000,
  "vel": 5000000,
  "acc": 20000,
  "dec": 20000,
  "immediate": true
}
```

#### All possible initial results for move_abs (exact `message` strings)

**Rejected cases:**

1. Drive is offline (checked first in command handler)
```json
{
  "command_id": "cmd_move_abs_001",
  "command": "move_abs",
  "command_status": "failure",
  "message": "rejected: drive_offline - command rejected because drive is offline"
}
```

2. Kill switch (node DIN input) is active
```json
{
  "command_id": "cmd_move_abs_001",
  "command": "move_abs",
  "command_status": "failure",
  "message": "rejected: kill_active - motion command ignored because kill switch is active"
}
```

3. Ramp stop recovery is in progress
```json
{
  "command_id": "cmd_move_abs_001",
  "command": "move_abs",
  "command_status": "failure",
  "message": "rejected: ramp_recovery_in_progress - PP move ignored because ramp stop recovery in progress"
}
```

4. Gantry is currently homing
```json
{
  "command_id": "cmd_move_abs_001",
  "command": "move_abs",
  "command_status": "failure",
  "message": "rejected: homing_in_progress - PP move ignored because homing in progress"
}
```

5. Gantry is in UNHOMED state (never completed homing since last power-up or fault reset)
```json
{
  "command_id": "cmd_move_abs_001",
  "command": "move_abs",
  "command_status": "failure",
  "message": "rejected: not_homed - PP move rejected because gantry is not homed"
}
```

6. Requested direction is blocked (either a limit latch is set for that direction, or the corresponding limit switch is currently active)
```json
{
  "command_id": "cmd_move_abs_001",
  "command": "move_abs",
  "command_status": "failure",
  "message": "rejected: limit_latched - PP move rejected by limit latch or active limit in the requested direction"
}
```

**Success case (command_status = "success") — sent ONLY after the move physically completes**

CRITICAL RULE:
For `move_abs` (and `move_rel`), `command_status: "success"` is **never** published when the command is first accepted.
It is published **only after** the gantry has actually reached the target position and the move is considered complete.

Exact conditions required before sending success for move_abs:
- The absolute move command was accepted and the drive was commanded to the target.
- The drive has set the "target reached" bit in the statusword.
- The position has stabilized at (or very near) the requested target.
- No limit switch was hit.
- No kill switch became active.
- No fault, stop, ramp_stop, halt, or external stop interrupted the move.
- No ramp recovery was triggered.

When all the above are true, the result published is:

```json
{
  "command_id": "cmd_move_abs_001",
  "command": "move_abs",
  "command_status": "success",
  "message": "absolute move completed at target position"
}
```

If the move is interrupted for any reason before the target is reached, the result for this `command_id` will be:

```json
{
  "command_id": "cmd_move_abs_001",
  "command": "move_abs",
  "command_status": "failure",
  "message": "rejected: <reason> - <human readable detail>"
}
```

No success result will ever be sent for that `command_id`.

This rule applies to both `move_abs` and `move_rel`.

Additional notes:
- Reaching the target is the only trigger for success on these motion commands.
- Mid-move events (limits, kill, faults, stops) are reported via status/feedback and cause a failure result for the original command.
- The `immediate` flag has no effect on when success is reported.

### 4. move_rel
Moves relative to the current gantry position.

Publish to:
```text
factory/gantry/command
```

Payload:
```json
{
  "command_id": "cmd_move_rel_001",
  "command": "move_rel",
  "pos": 10000,
  "vel": 5000000,
  "acc": 20000,
  "dec": 20000,
  "immediate": true
}
```

Example result:
```json
{
  "command_id": "cmd_move_rel_001",
  "command": "move_rel",
  "command_status": "failure",
  "message": "rejected: limit_latched - relative move rejected because target direction is limit-latched"
}
```

### 5. jog
Runs gantry in profile velocity mode.

Publish to:
```text
factory/gantry/command
```

Payload:
```json
{
  "command_id": "cmd_jog_001",
  "command": "jog",
  "vel": -100000,
  "acc": 20000,
  "dec": 20000
}
```

Example result:
```json
{
  "command_id": "cmd_jog_001",
  "command": "jog",
  "command_status": "failure",
  "message": "rejected: limit_active - jog command blocked by active limit in requested direction"
}
```

### 6. stop
Fastest stop path. It must preempt current motion, homing, or ramp recovery whenever the ESP can receive it. It must never be classified as busy.

Publish to:
```text
factory/gantry/command
```

Payload:
```json
{
  "command_id": "cmd_stop_001",
  "command": "stop"
}
```

Example result:
```json
{
  "command_id": "cmd_stop_001",
  "command": "stop",
  "command_status": "success",
  "message": "stop (fastest path) accepted"
}
```

### 7. ramp_stop
Explicit controlled stop.

Publish to:
```text
factory/gantry/command
```

Payload:
```json
{
  "command_id": "cmd_ramp_stop_001",
  "command": "ramp_stop",
  "dec": 20000
}
```

Example result:
```json
{
  "command_id": "cmd_ramp_stop_001",
  "command": "ramp_stop",
  "command_status": "success",
  "message": "ramp stop accepted"
}
```

### 8. halt
Low-level halt alias if retained.

Publish to:
```text
factory/gantry/command
```

Payload:
```json
{
  "command_id": "cmd_halt_001",
  "command": "halt",
  "dec": 20000
}
```

Example result:
```json
{
  "command_id": "cmd_halt_001",
  "command": "halt",
  "command_status": "success",
  "message": "halt accepted"
}
```

### 9. disable_voltage
Disables drive voltage.

Publish to:
```text
factory/gantry/command
```

Payload:
```json
{
  "command_id": "cmd_disable_voltage_001",
  "command": "disable_voltage"
}
```

Example result:
```json
{
  "command_id": "cmd_disable_voltage_001",
  "command": "disable_voltage",
  "command_status": "success",
  "message": "disable voltage accepted"
}
```

### 10. fault_reset
Resets drive fault state.

Publish to:
```text
factory/gantry/command
```

Payload:
```json
{
  "command_id": "cmd_fault_reset_001",
  "command": "fault_reset"
}
```

Example result:
```json
{
  "command_id": "cmd_fault_reset_001",
  "command": "fault_reset",
  "command_status": "success",
  "message": "fault reset accepted"
}
```

## Result Topic
```text
factory/gantry/result
```

ESP publishes command results to this topic.

Master server subscribes to this topic.

Each result must include the same `command_id` received in the command message when available.

The outcome field is always named `command_status`.

Only three allowed values:
```text
received
success
failure
```

"rejected" and "busy" are expressed as `command_status: "failure"` and classified via the text in the `message` field.

No separate `reason` field is used.

Result behavior (important timing rules):

- every successfully parsed command → `received` (message "command received", sent immediately on parse)
- `home`            → `success` only after homing completes successfully, otherwise `failure`
- `move_abs`        → `success` **only after** the gantry physically reaches the absolute target position. Success is **never** sent at acceptance time.
- `move_rel`        → `success` **only after** the gantry physically reaches the relative target. Success is **never** sent at acceptance time.
- `jog`             → `success` or `failure` (jog is continuous; success may be sent on explicit stop if a stop command was used to end it cleanly)
- `stop`            → `success` or `failure` (fastest stop path, never classified as busy)
- `ramp_stop`       → `success` or `failure`
- `halt`            → `success` or `failure`
- `disable_voltage` → `success` or `failure`
- `fault_reset`     → `success` or `failure`

For `move_abs` and `move_rel` specifically:
- `command_status: "success"` is published **only after** the move has completed and the target has been reached.
- If the move is interrupted for any reason before completion (limit, kill switch, fault, ramp recovery, stop, halt, etc.), a `command_status: "failure"` result is sent for that command_id. No success result will be published.

Example result payloads use `command_status` (see Command Examples section above).

Failure details are described inside the `message` field, for example:
- "rejected: not_homed - ..."
- "rejected: homing_in_progress - ..."
- "rejected: limit_latched - ..."

## Node Status Topic
```text
factory/gantry/node_status
```

ESP publishes high-level state changes to this topic.

Master server subscribes to this topic.

Recommended states (top-level `state` field):
```text
unhomed
homing
idle
moving
jogging
stopping
ramp_recovery
kill_active
drive_offline
disabled
fault
unknown
error
```

Recommended payload (node-related telemetry fields use `node_` prefix):
```json
{
  "state": "idle",
  "referenced": true,
  "drive_online": true,
  "kill": false,
  "pos": 500000,
  "speed": 0,
  "current": 12,
  "node_din_status": 65534,
  "drive": {
    "statusword": 4663,
    "error_code": 0
  },
  "limits": {
    "outer_limit_1": false,
    "inner_limit_1": false,
    "inner_limit_2": false,
    "outer_limit_2": false
  },
  "limit_latched": {
    "outer_limit_1": false,
    "inner_limit_1": false,
    "inner_limit_2": false,
    "outer_limit_2": false
  }
}
```

Publish status when any of these change:
```text
drive online/offline
gantry state
homing complete/fail/cancel
limit switch active state
limit latch state
kill active/released
drive fault/fault reset
```

Node telemetry fields (e.g. din_status) use the `node_` prefix. Gantry state and drive object fields do not.

## Feedback Topic
```text
factory/gantry/feedback
```

ESP publishes periodic telemetry to this topic.

Master server subscribes to this topic.

Recommended payload (node-related fields use `node_` prefix):
```json
{
  "pos": 500000,
  "speed": 120000,
  "current": 15,
  "node_din_status": 65534,
  "kill": false,
  "limits": {
    "outer_limit_1": false,
    "inner_limit_1": false,
    "inner_limit_2": false,
    "outer_limit_2": false
  }
}
```
