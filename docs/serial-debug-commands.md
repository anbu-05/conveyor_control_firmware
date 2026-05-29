# Serial Debug Commands

Serial commands are handled by `microrl_task` in `main/tasks/command_task.c`.

Use the ESP-IDF monitor over the ESP32-S3 USB Serial/JTAG console. On Linux,
this usually appears as `/dev/ttyACM0`.

Commands are strict and literal. Case changes, aliases, missing arguments, or
extra arguments are rejected.

## Sensor Reference

The firmware treats the physical sensors like this:

```text
S0 = entry sensor
S1 = exit sensor
```

The conveyor always moves a tray from `S0` toward `S1`.

```text
tx0 = S0
tx1 = S1
rx0 = S0
rx1 = S1
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

## `getconfig`

Prints all editable runtime config values.

Input:

```text
getconfig
```

Output:

```text
CONFIG run_pwm 128
CONFIG done_hold_ms 100
CONFIG tx_detect_timeout_ms 5000
CONFIG tx_clear_timeout_ms 5000
CONFIG rx_detect_timeout_ms 5000
CONFIG rx_done_timeout_ms 5000
CONFIG mqtt_status_period_ms 100
```

## `getconfig run_pwm`

Prints one editable runtime config value.

Input:

```text
getconfig run_pwm
```

Output:

```text
CONFIG run_pwm 128
```

Unknown config output:

```text
ERR UNKNOWN_CONFIG
```

## `setconfig run_pwm 140`

Sets one editable runtime config value and saves it to NVS.

Input:

```text
setconfig run_pwm 140
```

Effect:

- Validates the config name and value.
- Rejects the change if the conveyor state machine is not `IDLE`.
- Saves the value to NVS.
- Updates the RAM value used by later jobs.

Success output:

```text
OK SETCONFIG run_pwm 140
```

Possible error outputs:

```text
ERR BAD_ARGS
ERR UNKNOWN_CONFIG
ERR BAD_VALUE
ERR CONFIG_BUSY
ERR CONFIG_SAVE
```

Editable keys:

```text
run_pwm                  0..255
done_hold_ms             0..60000
tx_detect_timeout_ms     1..600000
tx_clear_timeout_ms      1..600000
rx_detect_timeout_ms     1..600000
rx_done_timeout_ms       1..600000
mqtt_status_period_ms    100..60000
```

## `resetconfig`

Restores editable runtime config values to defaults from `main/config/config.h`
and saves those defaults to NVS.

Input:

```text
resetconfig
```

Effect:

- Rejects the reset if the conveyor state machine is not `IDLE`.
- Restores all editable values to compile-time defaults.
- Saves defaults to NVS.

Success output:

```text
OK RESETCONFIG
```

Possible error outputs:

```text
ERR BAD_ARGS
ERR CONFIG_BUSY
ERR CONFIG_SAVE
```

## `jobtx`

Submits a transmitter job to the central conveyor state machine.

Input:

```text
jobtx
```

Effect:

- Queues `CONVEYOR_CMD_START_TX`.
- The tray moves from `S0` toward `S1`.
- The state machine starts the motor immediately if it is idle.
- TX waits for `tx1` / `S1`, then stops after `tx1` clears.

Success output:

```text
OK JOBTX
```

Possible error outputs:

```text
ERR BAD_ARGS
ERR JOB_BUSY
ERR JOB_QUEUE
```

## `jobrx`

Submits a receiver job to the central conveyor state machine.

Input:

```text
jobrx
```

Effect:

- Queues `CONVEYOR_CMD_START_RX`.
- The receiver arms without moving.
- The motor starts only after `rx0` / `S0` detects a tray.
- The motor stops when `rx1` / `S1` detects the tray.

Success output:

```text
OK JOBRX
```

Possible error outputs:

```text
ERR BAD_ARGS
ERR JOB_BUSY
ERR JOB_QUEUE
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
EVENT JOB C0 TX_WAIT_FOR_TX1_DETECT
EVENT JOB C0 TX_WAIT_FOR_TX1_CLEAR
EVENT JOB C0 TX_DONE
EVENT JOB C0 IDLE
```

Unknown command:

```text
ERR UNKNOWN_COMMAND
```
