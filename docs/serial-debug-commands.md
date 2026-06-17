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

The distance between `S0` and `S1` is smaller than the tray length, so a tray
on the conveyor always triggers `S0`, `S1`, or both.

```text
tx0 = S0
tx1 = S1
rx0 = S0
rx1 = S1
has_tray = S0 detected OR S1 detected
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
- Disables speed control for `M0`.
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
- Disables speed control for `M0`.

Success output:

```text
OK STOPMOTOR M0
```

Error outputs:

```text
ERR BAD_ARGS
ERR UNKNOWN_MOTOR
```

## `setspeed M0 100`

Sets the signed speed target for `M0`.

Input:

```text
setspeed M0 100
```

Arguments:

- `M0`: the only configured motor name.
- `100`: target speed in encoder counts per second. Negative values reverse direction.

Effect:

- Sets `M0.target_speed` to `100`.
- Enables `M0.speed_control`.
- The motor PID task reads PCNT, calculates current speed, and writes PWM/direction to hardware.
- The speed controller estimates base PWM from the target speed, then P/D trims around it.
- If the target speed changes sign, PWM ramps down to zero before the direction GPIO changes.

Success output:

```text
OK SETSPEED M0
```

Error outputs:

```text
ERR BAD_ARGS
ERR UNKNOWN_MOTOR
ERR BAD_VALUE
```

## `setkp 0.500`

Sets and saves the P gain for speed control.

Input:

```text
setkp 0.500
```

Effect:

- Saves `speed_kp` to NVS.
- The value is stored internally as thousandths.

Success output:

```text
OK SETKP 0.500
```

Error outputs:

```text
ERR BAD_ARGS
ERR BAD_VALUE
ERR CONFIG_SAVE
```

## `setkd 0.005`

Sets and saves the D gain for speed control.

Input:

```text
setkd 0.005
```

Effect:

- Saves `speed_kd` to NVS.
- The value is stored internally as thousandths.
- The D term is normalized to the 20 ms motor PID tick used by this firmware.
- Set `speed_kd` to `0.000` to disable D control.

Success output:

```text
OK SETKD 0.005
```

Error outputs:

```text
ERR BAD_ARGS
ERR BAD_VALUE
ERR CONFIG_SAVE
```

## `resetk`

Restores only the speed control gains to defaults from `main/config/config.h`.

Input:

```text
resetk
```

Effect:

- Saves default `speed_kp` and `speed_kd` to NVS.
- Does not change other runtime config values.
- Takes effect immediately, like `setkp` and `setkd`.

Success output:

```text
OK RESETK
```

Error outputs:

```text
ERR BAD_ARGS
ERR CONFIG_SAVE
```

## `stop`

Safety/debug stop command.

Input:

```text
stop
```

Effect:

- Sets every motor PWM to `0`.
- Disables speed control for every motor.
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

## `watchencoder M0 on`

Enables raw encoder count output for `M0`.

Input:

```text
watchencoder M0 on
```

Effect:

- Prints `EVENT ENCODER M0 <count> <speed>` lines about every 100 ms.
- The count comes from PCNT using GPIO17 as channel A and GPIO18 as channel B.
- The speed is calculated by the motor PID task in encoder counts per second.
- The same raw count is stored in `M0.position`.

Success output:

```text
OK WATCHENCODER M0 ON
```

Example event output:

```text
EVENT ENCODER M0 120 100
```

Error outputs:

```text
ERR BAD_ARGS
ERR UNKNOWN_MOTOR
```

## `watchencoder M0 off`

Disables raw encoder count output for `M0`.

Input:

```text
watchencoder M0 off
```

Success output:

```text
OK WATCHENCODER M0 OFF
```

Error outputs:

```text
ERR BAD_ARGS
ERR UNKNOWN_MOTOR
```

## `getencoder M0`

Prints one raw encoder diagnostic line.

Input:

```text
getencoder M0
```

Output:

```text
ENCODER M0 120 1 0
```

Field order:

```text
ENCODER <motor> <count> <gpio17_a> <gpio18_b>
```

Error outputs:

```text
ERR BAD_ARGS
ERR UNKNOWN_MOTOR
```

## `getmotor M0`

Prints the current motor debug state.

Input:

```text
getmotor M0
```

Output:

```text
MOTOR M0 12 1 1200 5000 4800 1
```

Field order:

```text
MOTOR <motor> <pwm> <direction> <position> <target_speed> <current_speed> <speed_control>
```

Error outputs:

```text
ERR BAD_ARGS
ERR UNKNOWN_MOTOR
```

## `gettray`

Prints the derived tray-presence status.

Input:

```text
gettray
```

Output:

```text
TRAY C0 1 0 1
```

Field order:

```text
TRAY <conveyor_id> <has_tray> <s0> <s1>
```

Meaning:

- `has_tray` is `1` when `S0` or `S1` detects a tray.
- `s0` and `s1` are raw GPIO readings.
- Raw sensor `0` means tray detected.
- Raw sensor `1` means no tray detected.

Error output:

```text
ERR BAD_ARGS
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
CONFIG run_speed_counts_per_sec 100
CONFIG speed_kp 0.500
CONFIG speed_kd 0.000
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
run_speed_counts_per_sec 0..100000
speed_kp_milli           0..100000
speed_kd_milli           0..100000
done_hold_ms             0..60000
tx_detect_timeout_ms     1..600000
tx_clear_timeout_ms      1..600000
rx_detect_timeout_ms     1..600000
rx_done_timeout_ms       1..600000
mqtt_status_period_ms    100..60000
```

Use `setkp <decimal>` and `setkd <decimal>` for normal speed gain tuning.
Use `resetk` to restore only those two gains to the defaults from `main/config/config.h`.
`speed_kp_milli` and `speed_kd_milli` are the integer values saved internally.

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
- Rejected with `ERR NO_TRAY` if neither sensor detects a tray.
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
ERR NO_TRAY
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
- Rejected with `ERR TRAY_PRESENT` if a tray is already on this conveyor.
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
ERR TRAY_PRESENT
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

Encoder events:

```text
EVENT ENCODER M0 120 100
```

Tray status:

```text
TRAY C0 1 0 1
```

Unknown command:

```text
ERR UNKNOWN_COMMAND
```
