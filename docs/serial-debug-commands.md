# Serial Debug Commands

Serial commands are handled by `microrl_task` in `main/tasks/command_task.c`.

Use the ESP-IDF monitor over the ESP32-S3 USB Serial/JTAG console. On Linux,
this usually appears as `/dev/ttyACM0`.

Commands are strict and literal. Case changes, aliases, missing arguments, or
extra arguments are rejected.

## Direction Reference

The firmware treats the physical sensors like this:

```text
S0 = left sensor
S1 = right sensor
```

Job commands use conveyor-relative directions:

```text
right = move from S0 toward S1
left  = move from S1 toward S0
```

## `setmotor M0 128 1`

Direct motor debug command.

Input:

```text
setmotor M0 128 1
```

Arguments:

- `M0`: the only configured motor name.
- `128`: PWM value. Must be `0` to `255`.
- `1`: direction value. Must be `0` or `1`.

Effect:

- Sets `M0.pwm` to `128`.
- Sets `M0.direction` to `1`.
- The motor controller task later writes those values to GPIO/LEDC.

Success output:

```text
OK SETMOTOR M0
```

Error outputs:

```text
ERR BAD_ARGS
ERR UNKNOWN_MOTOR
ERR BAD_PWM
ERR BAD_DIRECTION
```

## `stopmotor M0`

Stops one motor without changing its direction field.

Input:

```text
stopmotor M0
```

Effect:

- Sets `M0.pwm` to `0`.

Success output:

```text
OK STOPMOTOR M0
```

Error outputs:

```text
ERR BAD_ARGS
ERR UNKNOWN_MOTOR
```

## `stop`

Safety/debug stop command.

Input:

```text
stop
```

Effect:

- Sets every motor PWM to `0`.
- Sends an emergency-stop command to the conveyor state machine.
- The conveyor job state becomes `ESTOP` after the queued command is processed.

Success output:

```text
OK STOP
```

Error output:

```text
ERR BAD_ARGS
```

## `watchsensors on`

Enables physical sensor event output.

Input:

```text
watchsensors on
```

Effect:

- Enables `EVENT SENSOR ...` lines when `S0` or `S1` changes.

Success output:

```text
OK WATCHSENSORS ON
```

Example event output:

```text
EVENT SENSOR S0 1 0
EVENT SENSOR S1 0 1
```

## `watchsensors off`

Disables physical sensor event output.

Input:

```text
watchsensors off
```

Effect:

- Stops printing `EVENT SENSOR ...` lines.

Success output:

```text
OK WATCHSENSORS OFF
```

## `jobtx right`

Submits a transmitter job to the central conveyor state machine.

Input:

```text
jobtx right
```

Effect:

- Queues `CONVEYOR_CMD_START_TX`.
- Direction is `right`, so the tray moves from `S0` toward `S1`.
- The state machine starts the motor immediately if it is idle.

Success output:

```text
OK JOBTX
```

Possible error outputs:

```text
ERR BAD_ARGS
ERR BAD_DIRECTION
ERR JOB_BUSY
ERR JOB_QUEUE
```

## `jobtx left`

Submits a transmitter job to the central conveyor state machine.

Input:

```text
jobtx left
```

Effect:

- Queues `CONVEYOR_CMD_START_TX`.
- Direction is `left`, so the tray moves from `S1` toward `S0`.
- The state machine starts the motor immediately if it is idle.

Success output:

```text
OK JOBTX
```

## `jobrx right`

Submits a receiver job to the central conveyor state machine.

Input:

```text
jobrx right
```

Effect:

- Queues `CONVEYOR_CMD_START_RX`.
- Direction is `right`.
- The receiver arms without moving.
- The motor starts only after `rx_1` detects a tray.

Success output:

```text
OK JOBRX
```

Possible error outputs:

```text
ERR BAD_ARGS
ERR BAD_DIRECTION
ERR JOB_BUSY
ERR JOB_QUEUE
```

## `jobrx left`

Submits a receiver job to the central conveyor state machine.

Input:

```text
jobrx left
```

Effect:

- Queues `CONVEYOR_CMD_START_RX`.
- Direction is `left`.
- The receiver arms without moving.
- The motor starts only after `rx_1` detects a tray.

Success output:

```text
OK JOBRX
```

## `estop`

Submits an emergency stop to the central conveyor state machine.

Input:

```text
estop
```

Effect:

- Queues `CONVEYOR_CMD_EMERGENCY_STOP`.
- Stops all motors when the state machine processes it.
- Sets conveyor state to `ESTOP`.

Success output:

```text
OK ESTOP
```

Possible error outputs:

```text
ERR BAD_ARGS
ERR JOB_QUEUE
```

## `clearerror`

Clears `ERROR` or `ESTOP`.

Input:

```text
clearerror
```

Effect:

- Queues `CONVEYOR_CMD_CLEAR_ERROR`.
- If the current state is `ERROR` or `ESTOP`, the state machine returns to
  `IDLE`.

Success output:

```text
OK CLEARERROR
```

Possible error outputs:

```text
ERR BAD_ARGS
ERR JOB_QUEUE
```

## Common Outputs

Startup:

```text
READY conveyor
```

Job events:

```text
EVENT JOB C0 TX_WAIT_FOR_TX2_DETECT right
EVENT JOB C0 TX_WAIT_FOR_TX2_CLEAR right
EVENT JOB C0 TX_DONE right
EVENT JOB C0 IDLE right
```

Unknown command:

```text
ERR UNKNOWN_COMMAND
```
