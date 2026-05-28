# conveyor

## Current Status

ESP-IDF starter project for controlling one motor through a Cytron MD30C.

Implemented a strict serial command layer using a vendored local `microrl` component. The app has one motor, `M0`, controlled by a FreeRTOS motor controller task.

Added function-level comments in `main/main.c` and `components/microrl/microrl.c` so future readers can quickly understand each function's job.

Simplified `main/main.c` by removing small helper functions that were not pulling their weight in a one-motor version. Command handling now lives mostly inside `execute_command()`, with `find_motor()` kept because it connects command motor names to the motor table.

Troubleshooting update: command input now uses `stdin` instead of directly reading `UART_NUM_0`, and the project config uses USB Serial/JTAG as the primary console. This matches `/dev/ttyACM0` on ESP32-S3 boards and avoids monitor write timeouts caused by UART0 GPIO43/44 console input mismatch.

## Files

- `CMakeLists.txt`: Top-level ESP-IDF project file.
- `main/CMakeLists.txt`: Main component build file. Depends on ESP-IDF GPIO, LEDC, and `microrl`.
- `main/main.c`: Motor struct, strict command parser, UART input task, and motor controller task.
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

Current motor:

- `M0`
- PWM GPIO: `GPIO_NUM_7`
- Direction GPIO: `GPIO_NUM_6`
- LEDC channel: `LEDC_CHANNEL_0`

## Commands

Strict commands currently supported:

```text
setmotor M0 128 1
stopmotor M0
stop
```

- `setmotor M0 128 1`: sets `M0.pwm = 128` and `M0.direction = 1`.
- `stopmotor M0`: sets only `M0.pwm = 0`.
- `stop`: sets PWM to `0` for all motors.

Invalid command names, motor names, argument counts, PWM values, or direction values are rejected.

## Architecture

- `microrl_task`: reads console stdin input and edits the motor struct.
- `motor_controller_task`: reads the motor struct and writes direction GPIO plus LEDC PWM.
- `motor_mutex`: protects motor struct reads and writes.

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
- Commands are strict and literal.
- PCNT will be handled later by a task that reads PCNT count and updates `motor.position`.

## Next Useful Commands

```bash
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```
