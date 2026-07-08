# Serial Debug Commands

Serial debug commands are handled by the ESP console in `main/tasks/console.c`.

Use the ESP-IDF monitor over the ESP32-S3 USB Serial/JTAG console. On Linux,
this usually appears as `/dev/ttyACM0`.

Commands are strict. Case changes, aliases, missing arguments, or extra
arguments are rejected.

The current firmware exposes low-level motor commissioning commands only. MQTT
and the state machine are still placeholders, so these commands call the public
hardware and PID APIs directly.

## Motor Reference

The firmware currently configures one motor:

```text
M0 = first motor in motors[]
```

Motor IDs are strings. Use `M0` exactly.

## Output Style

Successful commands print `OK ...` or `CONFIG ...` lines.

Common error lines:

```text
ERR BAD_ARGS
ERR BAD_CONFIG_KEY
ERR ESP_ERR_INVALID_ARG
ERR UNKNOWN_COMMAND
```

ESP-IDF errors are printed with `esp_err_to_name()`.

## `setmotor M0 128 0`

Direct raw motor output command.

Input:

```text
setmotor M0 128 0
```

Arguments:

- `M0`: motor id.
- `128`: PWM request. Runtime `max_pwm` and hardware limits clamp the applied value.
- `0`: direction GPIO level. Must match one of the configured direction levels.

Effect:

- Calls `set_motor("M0", 128, 0)`.
- Applies PWM/direction immediately through GPIO/LEDC.
- Stores the applied `pwm` and `direction` in `motors[]`.

Success output:

```text
OK SETMOTOR motor=M0 pwm=128 dir=0
```

Error outputs:

```text
ERR BAD_ARGS
ERR ESP_ERR_INVALID_ARG
```

## `stopmotor M0`

Cuts PWM for one motor.

Input:

```text
stopmotor M0
```

Effect:

- Calls `stop_motor("M0")`.
- Sets the LEDC duty to `0`.
- Stores `pwm = 0` in `motors[]`.
- Keeps the last direction value unchanged.

Success output:

```text
OK STOPMOTOR motor=M0
```

Error outputs:

```text
ERR BAD_ARGS
```

## `setposition M0 1200`

Sets one motor's PID target position.

Input:

```text
setposition M0 1200
```

Arguments:

- `M0`: motor id.
- `1200`: target offset-corrected encoder position.

Effect:

- Calls `set_position("M0", 1200)`.
- Updates `motors[].target_position`.
- The PID task for `M0` reads this target on its next tick.

Note:

- This command does not currently enable `position_control` by itself.

Success output:

```text
OK SETPOSITION motor=M0 position=1200
```

Error outputs:

```text
ERR BAD_ARGS
ERR ESP_ERR_INVALID_ARG
```

## `getposition M0`

Reads one motor's latest offset-corrected position.

Input:

```text
getposition M0
```

Effect:

- Calls `get_position("M0", &position)`.
- Reads `motors[].current_position`.
- The hardware task updates this from PCNT encoder count plus `position_offset`.

Success output:

```text
OK POSITION motor=M0 pos=1234
```

Error outputs:

```text
ERR BAD_ARGS
ERR ESP_ERR_INVALID_ARG
```

## `setoffset M0 0`

Sets the position offset for one motor.

Input:

```text
setoffset M0 0
```

Arguments:

- `M0`: motor id.
- `0`: offset added to raw encoder count.

Effect:

- Calls `set_offset("M0", 0)`.
- Sets `position_offset`.
- Immediately republishes `current_position = encoder_count + position_offset`.

Success output:

```text
OK SETOFFSET motor=M0 offset=0
```

Error outputs:

```text
ERR BAD_ARGS
ERR ESP_ERR_INVALID_ARG
```

## `setk M0 0.500 0.000 0.050`

Sets live PID gains for one motor.

Input:

```text
setk M0 0.500 0.000 0.050
```

Arguments:

- `M0`: motor id.
- `0.500`: live `kp`.
- `0.000`: live `ki`.
- `0.050`: live `kd`.

Effect:

- Calls `setk("M0", kp, ki, kd)`.
- Updates live PID gains only.
- Does not save to NVS.
- Resets that motor's PID integral/derivative memory.

Success output:

```text
OK SETK motor=M0 kp=0.500 ki=0.000 kd=0.050
```

Error outputs:

```text
ERR BAD_ARGS
ERR ESP_ERR_INVALID_ARG
```

## `getconfig`

Prints every editable runtime config value.

Input:

```text
getconfig
```

Effect:

- Reads all runtime config values from RAM.
- Does not modify NVS.

Success output:

```text
CONFIG pid_kp_milli 500
CONFIG pid_ki_milli 0
CONFIG pid_kd_milli 50
CONFIG max_pwm 245
```

Error outputs:

```text
ERR BAD_ARGS
ERR ESP_ERR_INVALID_ARG
```

## `getconfig max_pwm`

Prints one runtime config value.

Input:

```text
getconfig max_pwm
```

Effect:

- Reads the selected runtime config value from RAM.

Success output:

```text
CONFIG max_pwm 245
```

Error outputs:

```text
ERR BAD_ARGS
ERR BAD_CONFIG_KEY
ERR ESP_ERR_INVALID_ARG
```

## `setconfig max_pwm 200`

Sets and saves one runtime config value.

Input:

```text
setconfig max_pwm 200
```

Effect:

- Updates the runtime config value in RAM.
- Stores the value to NVS.
- Runtime readers see the new value immediately.

Success output:

```text
OK SETCONFIG max_pwm 200
```

Error outputs:

```text
ERR BAD_ARGS
ERR BAD_CONFIG_KEY
ERR ESP_ERR_INVALID_ARG
```

## `resetconfig max_pwm`

Resets one runtime config value to its compiled default.

Input:

```text
resetconfig max_pwm
```

Effect:

- Loads the compiled default for the selected key.
- Stores that default to NVS.
- Runtime readers see the reset value immediately.

Success output:

```text
OK RESETCONFIG max_pwm
```

Error outputs:

```text
ERR BAD_ARGS
ERR BAD_CONFIG_KEY
ERR ESP_ERR_INVALID_ARG
```

## `resetconfig`

Resets all runtime config values to compiled defaults.

Input:

```text
resetconfig
```

Effect:

- Loads compiled defaults for every runtime config key.
- Stores all defaults to NVS.
- Applies PID gain defaults to all configured motors.

Success output:

```text
OK RESETCONFIG
```

Error outputs:

```text
ERR BAD_ARGS
ERR ESP_ERR_INVALID_ARG
```

## Runtime Config Keys

Editable keys:

```text
pid_kp_milli
pid_ki_milli
pid_kd_milli
max_pwm
min_start_pwm
reference_speed_counts_per_sec
positive_speed_counts_per_sec
negative_speed_counts_per_sec
sensor_seek_speed_counts_per_sec
max_speed_counts_per_sec
position_tolerance_counts
reference_timeout_ms
positive_timeout_ms
negative_timeout_ms
stall_check_ms
stall_min_delta_counts
direction_check_delay_ms
limit_switch_qualify_ms
mqtt_status_period_ms
```

PID gain config keys are stored as milli-units:

```text
pid_kp_milli 500 = live kp 0.500
pid_ki_milli 0 = live ki 0.000
pid_kd_milli 50 = live kd 0.050
```

Changing or resetting a PID gain config key updates the live PID gains for all
configured motors.

## Current Flow

```text
serial command
  -> console.c handler
  -> hardware.c or pid.c public API
  -> motors[] shared state and/or GPIO/LEDC
```

`setmotor` and `stopmotor` apply hardware output immediately. The hardware task
only polls encoder and sensor feedback into `motors[]`.
