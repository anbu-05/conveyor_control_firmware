# Conveyor Firmware

ESP-IDF firmware for an ESP32-S3 motor-control node.

The current implementation is focused on local serial debugging, motor hardware
control, encoder/sensor polling, runtime config, and per-motor PID task
ownership. MQTT and the state machine are present as module placeholders and will
be built on top of the same shared state and control APIs.

## Current Status

Implemented:

- ESP console task with serial debug commands.
- BTS7960 motor driver output using RPWM/LPWM plus REN/LEN.
- Runtime config defaults, NVS load, NVS save, and serial config commands.
- Shared `motor_t motors[]` state with string motor IDs like `M0`.
- Hardware task for encoder and sensor polling.
- Raw motor output APIs: `set_motor()` and `stop_motor()`.
- One PID task per configured motor.
- PID APIs: `set_position()`, `get_position()`, `set_offset()`, and `setk()`.

In progress / placeholders:

- State machine behavior.
- MQTT command handling.
- Safety behavior.

## Architecture

```mermaid
flowchart TD
    A[console task]
    B[mqtt task]
    C[pid task]
    D[hardware task]

    E{{app_state}}
    E1{{app_state}}
    E2{{app_state}}
    E3{{app_state}}

    F{{runtime_config}}
    F1{{runtime_config}}

    G{{config}}

    H[statemachine]

    A   <---> |pid function\n calls| C
    A    ---> |hardware function\n calls| D
    A   <---> |update pid key fields| E1
    E1  <---> |read key fields| C
    C   <---> |read/set position fields\n & set pwm and dir\n| E2
    E2  <---> |read pwm and dir\n & update sensors\encoders| D

    A   <---> |read and update| F
    A   <---> |read and update| G
    A   <---> |read and update| E

    E3  <---> |read sensors\n & update current state| H
    F1   ---> |read and update positions| H
    H   <---> |function calls| C
    A    ---> |control\n statemachine| H
    B    ---> |control\n statemachine| H
```

## Main Modules

- `main/tasks/console.c`: ESP console command registration and serial command loop.
- `main/tasks/hardware.c`: motor GPIO/LEDC output, PCNT encoder setup, sensor polling.
- `main/tasks/pid.c`: per-motor PID task and PID-facing public APIs.
- `main/tasks/mqtt.c`: MQTT placeholder task.
- `main/tasks/safety.c`: safety placeholder task.
- `main/statemachine/statemachine.c`: state machine placeholder task.
- `main/shared/app_state.c`: shared `motors[]` state and mutex.
- `main/config/runtime_config.c`: RAM/NVS runtime config values.
- `main/config/config.h`: compile-time identity, pin, and timing defaults.

## BTS7960 Motor Driver

Motor output is configured for a BTS7960 H-bridge driver in `main/config/config.h`:

```c
MOTOR_RPWM_GPIO = GPIO_NUM_15
MOTOR_LPWM_GPIO = GPIO_NUM_16
MOTOR_REN_GPIO = GPIO_NUM_7
MOTOR_LEN_GPIO = GPIO_NUM_8
```

`hardware_motor_init()` enables both `REN` and `LEN`, then configures separate
LEDC channels for `RPWM` and `LPWM`. `set_motor()` clears both PWM inputs before
applying a new direction. Positive direction drives `RPWM`; negative direction
drives `LPWM`. `stop_motor()` clears both PWM inputs.

## Adding Runtime Config

Runtime config uses enum keys internally. Console names live in `console.c`. To
add a new value:

1. Add a key before `RUNTIME_CONFIG_COUNT` in `main/config/runtime_config.h`.
2. Add the matching value/NVS table entry in `main/config/runtime_config.c`.
3. Add the console name mapping in `s_runtime_configs[]` in `main/tasks/console.c`.

Example:

```c
RUNTIME_CONFIG_NEW_VALUE,
```

```c
[RUNTIME_CONFIG_NEW_VALUE] = {
    .nvs_key = "new_val",
    .default_value = 123,
    .value = 123,
},
```

```c
{"new_value", RUNTIME_CONFIG_NEW_VALUE},
```

After that, `getconfig`, `setconfig`, `resetconfig`, NVS load/save, and internal
`runtime_config_get()` / `runtime_config_set()` access work through the enum key.

## Adding Console Commands

Console commands are registered from one table in `main/tasks/console.c` by
`register_console_commands()` and handled by one switch in
`handle_console_command()`.

To add a command:

1. Add a value to `console_command_id_t`.
2. Add one row to `s_commands[]` with the command name, help text, and id.
3. Add one `case` in `handle_console_command()`.

Keep command behavior in that one handler unless the logic grows enough to
deserve its own module-level API.

## Serial Debug Commands

Current serial commands are documented in:

```text
docs/serial-debug-commands.md
```

Examples:

```text
setmotor M0 128 0
stop
stopmotor M0
setposition M0 1200
getposition M0
setoffset M0 0
setk M0 0.500 0.000 0.050
getconfig
setconfig max_pwm 200
resetconfig max_pwm
status
```

## Build

Use an ESP-IDF shell with `idf.py` available:

```bash
idf.py build
```

Flash and monitor with the ESP-IDF commands appropriate for the connected
ESP32-S3 board.
