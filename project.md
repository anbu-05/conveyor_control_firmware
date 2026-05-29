# conveyor

## Current Status

ESP-IDF starter project for controlling one motor through a Cytron MD30C.

Implemented a strict serial command layer using a vendored local `microrl` component. The app has one motor, `M0`, controlled by a FreeRTOS motor controller task.

Added function-level comments in the main app source files and `components/microrl/microrl.c` so future readers can quickly understand each function's job.

Simplified the app source by removing small helper functions that were not pulling their weight in a one-motor version. Command handling now lives in `main/tasks/command_task.c`, with `find_motor()` kept in shared state because it connects command motor names to the motor table.

Troubleshooting update: command input now uses `stdin` instead of directly reading `UART_NUM_0`, and the project config uses USB Serial/JTAG as the primary console. This matches `/dev/ttyACM0` on ESP32-S3 boards and avoids monitor write timeouts caused by UART0 GPIO43/44 console input mismatch.

Added two binary sensors, `S0` on GPIO4 and `S1` on GPIO5. A sensor reader task polls them every 20 ms and prints machine-readable event lines when `watchsensors on` is enabled.

Added a central conveyor job state machine for high-level tray transfer jobs. MQTT and microrl can submit TX/RX/emergency/clear-error commands to the same queue, while the state machine owns the active job. DONE states auto-return to `IDLE` after a short report hold.

Added MQTT support in the same style as the senior gantry repo: hardcoded WiFi/broker/topic config, `espressif/mqtt` dependency, WiFi/MQTT setup in its own module, JSON high-level command parsing, and feedback publishing. MQTT does not expose raw PWM commands.

Added runtime-editable config values backed by NVS. Serial debug commands can read, set, and reset runtime-safe values such as `run_pwm`, transfer timeouts, done hold time, and MQTT status period. Compile-time defaults still live in `main/config/config.h`.

Expanded the conveyor state-machine documentation with detailed TX/RX timeout meanings, timer start points, physical sensor mapping, failure causes, and tuning notes.

Removed the old conveyor `left`/`right` job convention. The conveyor now always moves trays from `S0` to `S1`. Logical job sensors are zero-based: `tx0 = S0`, `tx1 = S1`, `rx0 = S0`, and `rx1 = S1`.

Added `docs/mqtt-implementation.md` with MQTT startup flow, compile-time switches, exact parser behavior, queue handoff, feedback publishing, runtime status period behavior, and current MQTT limits.

Added central tray-presence status based on the physical assumption that tray length is greater than the distance between `S0` and `S1`. `has_tray` is true when either sensor detects a tray. MQTT publishes change-driven tray status on `conveyor/C0/tray`, and serial debug exposes `gettray`.

MQTT feedback now includes `state_elapsed_ms`, the elapsed milliseconds since the current conveyor state was entered. MQTT command errors now include the real current conveyor state, timer, and raw sensor values.

Current sensor interpretation is active-low: raw GPIO `0` means tray detected, and raw GPIO `1` means no tray.

Serial output is now token-based for a Python wrapper:

```text
READY conveyor
OK SETMOTOR M0
OK STOPMOTOR M0
OK STOP
OK WATCHSENSORS ON
OK WATCHSENSORS OFF
TRAY C0 1 0 1
OK JOBTX
OK JOBRX
OK ESTOP
OK CLEARERROR
ERR UNKNOWN_COMMAND
ERR UNKNOWN_MOTOR
ERR BAD_ARGS
ERR BAD_PWM
ERR BAD_DIRECTION
ERR JOB_QUEUE
ERR JOB_BUSY
ERR NO_TRAY
ERR TRAY_PRESENT
ERR UNKNOWN_CONFIG
ERR BAD_VALUE
ERR CONFIG_BUSY
ERR CONFIG_SAVE
EVENT SENSOR S0 1 0
EVENT JOB C0 TX_WAIT_FOR_TX1_DETECT
CONFIG run_pwm 128
```

## Files

- `CMakeLists.txt`: Top-level ESP-IDF project file.
- `main/CMakeLists.txt`: Main component build file. Depends on ESP-IDF GPIO, LEDC, and `microrl`.
- `main/idf_component.yml`: ESP-IDF component dependency manifest for `espressif/mqtt`.
- `main/main.c`: ESP-IDF app startup, mutex creation, setup calls, task creation, and `READY conveyor`.
- `main/config/config.h`: Hardcoded conveyor ID, MQTT, open-loop speed, task, and timeout config.
- `main/config/runtime_config.h`: Runtime config getter/setter API.
- `main/config/runtime_config.c`: NVS-backed runtime config loading, validation, printing, setting, and reset.
- `main/shared/app_state.h`: Shared structs, constants, globals, and task/setup prototypes.
- `main/shared/app_state.c`: Motor table, sensor table, shared mutex globals, console printing, and motor lookup.
- `main/tasks/command_task.c`: Strict command parser and `microrl_task`.
- `main/tasks/motor_task.c`: LEDC/direction GPIO setup and `motor_controller_task`.
- `main/tasks/mqtt_task.h`: MQTT setup, status task, and publishing API.
- `main/tasks/mqtt_task.c`: WiFi/MQTT setup, JSON parsing, command queue submission, and status publishing.
- `main/tasks/sensor_task.c`: Sensor GPIO setup and `sensor_reader_task`.
- `main/conveyor/conveyor_job.h`: Conveyor command, state, status, setup, and task declarations.
- `main/conveyor/conveyor_job.c`: Central TX/RX conveyor transfer state machine and job queue setup.
- `components/microrl/`: Small vendored microrl-style command parser used by this app.
- `docs/architecture.mmd`: Mermaid chart of the current command and motor-control flow.
- `docs/code-structure.mmd`: Mermaid chart of files, functions, callbacks, and shared state.
- `docs/serial-debug-commands.md`: Detailed microrl command reference.
- `docs/mqtt-control-commands.md`: Detailed MQTT topic, payload, and feedback reference.
- `docs/mqtt-implementation.md`: Detailed MQTT implementation and task-flow reference.
- `docs/conveyor-state-machine.md`: Detailed TX/RX state machine reference.
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
gettray
getconfig
getconfig run_pwm
setconfig run_pwm 140
resetconfig
jobtx
jobrx
estop
clearerror
```

- `setmotor M0 128 1`: sets `M0.pwm = 128` and `M0.direction = 1`.
- `stopmotor M0`: sets only `M0.pwm = 0`.
- `stop`: sets PWM to `0` for all motors.
- `watchsensors on`: enables sensor event printing.
- `watchsensors off`: disables sensor event printing.
- `gettray`: prints derived tray presence and raw `S0/S1` values.
- `getconfig`: prints all editable runtime config values.
- `getconfig run_pwm`: prints one editable runtime config value.
- `setconfig run_pwm 140`: validates, saves, and applies a runtime config value.
- `resetconfig`: restores editable runtime config values to defaults.
- `jobtx`: submits a transmitter job to the conveyor state machine.
- `jobrx`: submits a receiver job to the conveyor state machine.
- `estop`: stops the active conveyor job and motor immediately.
- `clearerror`: returns `ERROR` or `ESTOP` to `IDLE`.

Invalid command names, motor names, argument counts, PWM values, direct motor direction values, config names, or config values are rejected.

`jobtx` is rejected with `ERR NO_TRAY` when neither sensor detects a tray.
`jobrx` is rejected with `ERR TRAY_PRESENT` when a tray is already on the
conveyor.

MQTT commands are high-level JSON only:

```json
{"type":"tx"}
{"type":"rx"}
{"type":"emergency_stop"}
{"type":"clear_error"}
```

MQTT defaults:

- WiFi SSID: `thrd_warehouse`
- Broker URI: `mqtt://192.168.1.126`
- Conveyor ID: `C0`
- Command topic: `conveyor/C0/cmd`
- Emergency topic: `conveyor/C0/emergency`
- Shared emergency topic: `conveyor/all/emergency`
- Feedback topic: `conveyor/C0/feedback`
- Tray topic: `conveyor/C0/tray`

## Architecture

- `main/main.c`: creates shared mutexes, configures console/PWM/sensors/job queue, starts tasks.
- `microrl_task`: reads console stdin input and edits shared state through command handlers.
- `mqtt_event_handler`: receives high-level JSON commands and sends conveyor commands to the job queue.
- `mqtt_status_task`: publishes periodic conveyor feedback when MQTT status output is enabled and publishes tray status when `has_tray` changes.
- `conveyor_job_task`: owns the TX/RX state machine and submits move/stop requests to the motor state.
- `motor_controller_task`: reads the motor struct and writes direction GPIO plus LEDC PWM.
- `sensor_reader_task`: reads sensor GPIOs and prints sensor events when watching is enabled.
- `motor_mutex`: protects motor struct reads and writes.
- `console_mutex`: keeps command responses and sensor event lines from interleaving.

Current high-level job states:

- `IDLE`
- `TX_WAIT_FOR_TX1_DETECT`
- `TX_WAIT_FOR_TX1_CLEAR`
- `RX_WAIT_FOR_RX0`
- `RX_WAIT_FOR_RX1`
- `TX_DONE`
- `RX_DONE`
- `ERROR`
- `ESTOP`

## Assumptions

- Project name is `conveyor`.
- Target is ESP32-S3 based on current `sdkconfig`.
- GPIO7 and GPIO6 are valid on the actual board.
- The MD30C `P` pin is connected to GPIO7.
- The MD30C `D` pin is connected to GPIO6.
- Sensor `S0` is connected to GPIO4 with an external pullup.
- Sensor `S1` is connected to GPIO5 with an external pullup.
- `S0` is the entry sensor.
- `S1` is the exit sensor.
- The tray always moves from `S0` toward `S1`.
- The tray length is greater than the distance between `S0` and `S1`.
- A tray on the conveyor always triggers at least one of `S0` or `S1`.
- Sensors are active low: GPIO `0` means tray detected.
- MQTT feedback includes `state_elapsed_ms`, measured from the current state's entry time.
- Commands are strict and literal.
- Sensor output is binary. Both 1 to 0 and 0 to 1 transitions are printed when watching is enabled.
- PCNT will be handled later by a task that reads PCNT count and updates `motor.position`.

## Next Useful Commands

```bash
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```
