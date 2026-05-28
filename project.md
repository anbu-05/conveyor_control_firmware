# conveyor

## Current Status

ESP-IDF starter project for controlling one motor through a Cytron MD30C.

Implemented a strict serial command layer using a vendored local `microrl` component. The app has one motor, `M0`, controlled by a FreeRTOS motor controller task.

Added function-level comments in the main app source files and `components/microrl/microrl.c` so future readers can quickly understand each function's job.

Simplified the app source by removing small helper functions that were not pulling their weight in a one-motor version. Command handling now lives in `main/command_task.c`, with `find_motor()` kept in shared state because it connects command motor names to the motor table.

Troubleshooting update: command input now uses `stdin` instead of directly reading `UART_NUM_0`, and the project config uses USB Serial/JTAG as the primary console. This matches `/dev/ttyACM0` on ESP32-S3 boards and avoids monitor write timeouts caused by UART0 GPIO43/44 console input mismatch.

Added two binary sensors, `S0` on GPIO4 and `S1` on GPIO5. A sensor reader task polls them every 20 ms and prints machine-readable event lines when `watchsensors on` is enabled.

Serial output is now token-based for a Python wrapper:

```text
READY conveyor
OK SETMOTOR M0
OK STOPMOTOR M0
OK STOP
OK WATCHSENSORS ON
OK WATCHSENSORS OFF
ERR UNKNOWN_COMMAND
ERR UNKNOWN_MOTOR
ERR BAD_ARGS
ERR BAD_PWM
ERR BAD_DIRECTION
EVENT SENSOR S0 1 0
```

## Files

- `CMakeLists.txt`: Top-level ESP-IDF project file.
- `main/CMakeLists.txt`: Main component build file. Depends on ESP-IDF GPIO, LEDC, and `microrl`.
- `main/main.c`: ESP-IDF app startup, mutex creation, setup calls, task creation, and `READY conveyor`.
- `main/app_state.h`: Shared structs, constants, globals, and task/setup prototypes.
- `main/app_state.c`: Motor table, sensor table, shared mutex globals, console printing, and motor lookup.
- `main/command_task.c`: Strict command parser and `microrl_task`.
- `main/motor_task.c`: LEDC/direction GPIO setup and `motor_controller_task`.
- `main/sensor_task.c`: Sensor GPIO setup and `sensor_reader_task`.
- `components/microrl/`: Small vendored microrl-style command parser used by this app.
- `docs/architecture.mmd`: Mermaid chart of the current command and motor-control flow.
- `docs/code-structure.mmd`: Mermaid chart of files, functions, callbacks, and shared state.
- `README.md`: Human-facing usage, wiring, commands, and build notes.
- `sdkconfig.defaults`: Default log level config.
- `project.md`: Project status notes for future chats.

## Motor Struct

`motor_t` currently stores:

- `name`
- `pwm`
- `direction`
- `position`
- `target_pos`
- `pos_control`
- `pwm_gpio`
- `dir_gpio`
- `ledc_channel`

`sensor_t` currently stores:

- `name`
- `gpio`
- `value`
- `last_value`

Current motor:

- `M0`
- PWM GPIO: `GPIO_NUM_7`
- Direction GPIO: `GPIO_NUM_6`
- LEDC channel: `LEDC_CHANNEL_0`

Current sensors:

- `S0`
- GPIO: `GPIO_NUM_4`
- `S1`
- GPIO: `GPIO_NUM_5`

## Commands

Strict commands currently supported:

```text
setmotor M0 128 1
stopmotor M0
stop
watchsensors on
watchsensors off
```

- `setmotor M0 128 1`: sets `M0.pwm = 128` and `M0.direction = 1`.
- `stopmotor M0`: sets only `M0.pwm = 0`.
- `stop`: sets PWM to `0` for all motors.
- `watchsensors on`: enables sensor event printing.
- `watchsensors off`: disables sensor event printing.

Invalid command names, motor names, argument counts, PWM values, or direction values are rejected.

## Architecture

- `main/main.c`: creates shared mutexes, configures console/PWM/sensors, starts tasks.
- `microrl_task`: reads console stdin input and edits shared state through command handlers.
- `motor_controller_task`: reads the motor struct and writes direction GPIO plus LEDC PWM.
- `sensor_reader_task`: reads sensor GPIOs and prints sensor events when watching is enabled.
- `motor_mutex`: protects motor struct reads and writes.
- `console_mutex`: keeps command responses and sensor event lines from interleaving.

Future threads can edit the same motor struct:

- PCNT reader task: read encoder count and write `motor.position`.
- MQTT task: remote commands that edit motor fields.
- PD/PID task: read `position` and `target_pos`, then write `pwm` and `direction`.

## Assumptions

- Project name is `conveyor`.
- Target is ESP32-S3 based on current `sdkconfig`.
- GPIO7 and GPIO6 are valid on the actual board.
- The MD30C `P` pin is connected to GPIO7.
- The MD30C `D` pin is connected to GPIO6.
- Sensor `S0` is connected to GPIO4 with an external pullup.
- Sensor `S1` is connected to GPIO5 with an external pullup.
- Commands are strict and literal.
- Sensor output is binary. Both 1 to 0 and 0 to 1 transitions are printed when watching is enabled.
- PCNT will be handled later by a task that reads PCNT count and updates `motor.position`.

## Next Useful Commands

```bash
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```
