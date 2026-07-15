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
- `0`: direction value. `1` drives BTS7960 `RPWM`; `0` drives BTS7960 `LPWM`.

Effect:

- Calls `set_motor("M0", 128, 0)`.
- Applies PWM/direction immediately through the BTS7960 GPIO/LEDC outputs.
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

## `stop`

Cuts PWM for every configured motor.

Input:

```text
stop
```

Effect:

- Calls `stop_motor()` for every motor id in `motors[]`.
- Clears both BTS7960 PWM inputs for each motor.
- Stores `pwm = 0` in each motor entry.

Success output:

```text
OK STOP motors=1
```

Error outputs:

```text
ERR BAD_ARGS
```

## `stopmotor M0`

Cuts PWM for one motor.

Input:

```text
stopmotor M0
```

Effect:

- Calls `stop_motor("M0")`.
- Clears both BTS7960 PWM inputs.
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

- This command does not currently enable `PID_control` by itself.

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

## `setspeed M0 250`

Sets one motor's PID target speed and switches that motor's PID mode to speed.

Input:

```text
setspeed M0 250
```

Arguments:

- `M0`: motor id.
- `250`: target encoder counts per second.

Effect:

- Calls `set_speed("M0", 250)`.
- Updates `motors[].target_speed`.
- Selects `MOTOR_PID_MODE_SPEED` so PID error becomes `target_speed - speed`.

Success output:

```text
OK SETSPEED motor=M0 speed=250
```

Error outputs:

```text
ERR BAD_ARGS
ERR PID_CONTROL_DISABLED
ERR ESP_ERR_INVALID_ARG
```

## `getspeed M0`

Reads one motor's latest measured speed.

Input:

```text
getspeed M0
```

Effect:

- Calls `get_speed("M0", &speed)`.
- Reads `motors[].speed`, which `hardware_task()` publishes from encoder-position deltas.

Success output:

```text
OK SPEED motor=M0 speed=250
```

Error outputs:

```text
ERR BAD_ARGS
ERR ESP_ERR_INVALID_ARG
```

## `get_pidmode M0`

Reads whether one motor's PID task is currently using position or speed error.

Input:

```text
get_pidmode M0
```

Effect:

- Reads `motors[].pid_mode` under `motor_mutex`.
- Reports `position` after `setposition` or boot default.
- Reports `speed` after `setspeed`.

Success output:

```text
OK PIDMODE motor=M0 mode=speed
```

Error outputs:

```text
ERR BAD_ARGS
ERR ESP_ERR_NOT_FOUND
```

## `pid_control M0 1`

Enables or disables PID ownership for one motor.

Input:

```text
pid_control M0 1
```

Arguments:

- `M0`: motor id.
- `1`: enable PID ownership. Use `0` to leave output to raw hardware commands.

Effect:

- Updates `motors[].PID_control`.
- Resets PID runtime memory so old integral/derivative state does not resume after manual mode.
- Resets target speed to zero when changing ownership.

Success output:

```text
OK PID_CONTROL motor=M0 enabled=1
```

Error outputs:

```text
ERR BAD_ARGS
ERR ESP_ERR_NOT_FOUND
```

## `getsensors M0`

Reads one motor's latest upstream/downstream sensor snapshot.

Input:

```text
getsensors M0
```

Effect:

- Reads `motors[].upstream_sensor` and `motors[].downstream_sensor`.
- The hardware task updates these from the configured sensor GPIO levels.
- `upstream` is the sensor on the feed/source side of conveyor flow.
- `downstream` is the sensor on the discharge/destination side of conveyor flow.

Success output:

```text
OK SENSORS motor=M0 upstream=1 downstream=0
```

Error outputs:

```text
ERR BAD_ARGS
ERR ESP_ERR_NOT_FOUND
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

## `setpid M0 500 0 50`

Sets live PID gains for one motor.

Input:

```text
setpid M0 500 0 50
```

Arguments:

- `M0`: motor id.
- `500`: live `kp` in milli-units.
- `0`: live `ki` in milli-units.
- `50`: live `kd` in milli-units.

Effect:

- Calls `set_pid_gains("M0", 500, 0, 50)`.
- Updates `motors[].kp`, `motors[].ki`, and `motors[].kd`.
- Resets that motor's PID integral/derivative memory.

Success output:

```text
OK SETPID motor=M0 kp_milli=500 ki_milli=0 kd_milli=50
```

Error outputs:

```text
ERR BAD_ARGS
ERR ESP_ERR_INVALID_ARG
```

## `getpid M0`

Reads live PID gains for one motor.

Input:

```text
getpid M0
```

Effect:

- Calls `get_pid_gains("M0", &kp, &ki, &kd)`.
- Returns the live per-motor gains in milli-units.

Success output:

```text
OK PID motor=M0 kp_milli=500 ki_milli=0 kd_milli=50
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
CONFIG max_pwm 245
CONFIG position_tolerance_counts 20
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

## `flip_direction`

Toggles the runtime conveyor direction map.

Input:

```text
flip_direction
```

Effect:

- Calls `hardware_flip_direction(&flipped)`.
- Makes future `set_motor()` calls drive the opposite BTS7960 PWM side for the same direction value.
- Reapplies any currently nonzero motor output so an already-moving conveyor reverses immediately.
- Makes `jobrx` wait at the upstream sensor and stop at the downstream sensor while flipped.
- Leaves the compile-time direction macros unchanged.

Success output:

```text
OK FLIP_DIRECTION flipped=1
```

Run the command again to restore the default map:

```text
OK FLIP_DIRECTION flipped=0
```

Error outputs:

```text
ERR BAD_ARGS
```

## `status`

Prints firmware identity, available commands, and configured motor IDs.

Input:

```text
status
```

Effect:

- Prints the app/machine/MQTT/WiFi values from `main/config/config.h`.
- Prints every registered console command and its argument format.
- Prints every configured motor id.

Success output includes lines like:

```text
STATUS app_name=conveyor
COMMAND setmotor - Set raw motor output: setmotor <motor_id> <pwm> <dir>
COMMAND flip_direction - Flip runtime conveyor direction map: flip_direction
MOTOR M0
```

Error outputs:

```text
ERR BAD_ARGS
```

## Runtime Config Keys

Editable keys:

```text
max_pwm
position_tolerance_counts
```

## Adding Runtime Config

Runtime config is defined by enum keys and a value/NVS table. Console command
names are mapped in `main/tasks/console.c`.

Add the enum key in `main/config/runtime_config.h` before
`RUNTIME_CONFIG_COUNT`:

```c
RUNTIME_CONFIG_NEW_VALUE,
```

Add one table entry in `main/config/runtime_config.c`:

```c
[RUNTIME_CONFIG_NEW_VALUE] = {
    .nvs_key = "new_val",
    .default_value = 123,
    .value = 123,
},
```

Add one console mapping in `s_runtime_configs[]` in `main/tasks/console.c`:

```c
{"new_value", RUNTIME_CONFIG_NEW_VALUE},
```

That is enough for:

- `getconfig new_value`
- `setconfig new_value 456`
- `resetconfig new_value`
- NVS load/save
- internal `runtime_config_get()` and `runtime_config_set()` calls

Runtime config keys do not need separate console command handlers. The generic
config command cases translate console strings to enum keys and then call the
runtime config API.

## Adding Console Commands

Console commands are registered from `s_commands[]` in `main/tasks/console.c` by
`register_console_commands()`. All commands dispatch through
`handle_console_command()`.

To add a command:

1. Add a `console_command_id_t` enum value.
2. Add one row to `s_commands[]` with the command name, help text, and id.
3. Add one `case` in `handle_console_command()`.

This keeps `console.c` small: parsing helpers, one handler, one registration
function, init, and the task loop.

## Current Flow

```text
serial command
  -> console.c handler
  -> hardware.c or pid.c public API
  -> motors[] shared state and/or GPIO/LEDC
```

`setmotor`, `stop`, `stopmotor`, and `flip_direction` apply hardware output immediately. The
hardware task only polls encoder and sensor feedback into `motors[]`.
