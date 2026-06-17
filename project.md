# conveyor

## Current Status

ESP-IDF starter project for controlling one motor through a BTS7960.

Changed the motor driver output from MD30C-style one PWM plus one direction GPIO to BTS7960-style two PWM outputs plus two enable pins. The configured BTS7960 pins are `RPWM=GPIO15`, `LPWM=GPIO16`, `REN=GPIO6`, and `LEN=GPIO7`. `direction=1` drives `RPWM` with PWM and keeps `LPWM` at zero; `direction=0` drives `LPWM` with PWM and keeps `RPWM` at zero. `REN` and `LEN` are set high during PWM setup.

Updated the encoder wiring to avoid the BTS7960 PWM pins. Encoder channel A is now GPIO17 and encoder channel B is now GPIO18.

Implemented a strict serial command layer using a vendored local `microrl` component. The app has one motor, `M0`, controlled by one combined FreeRTOS motor PID task.

Added function-level comments in the main app source files and `components/microrl/microrl.c` so future readers can quickly understand each function's job.

Simplified the app source by removing small helper functions that were not pulling their weight in a one-motor version. Command handling now lives in `main/tasks/command_task.c`, with `find_motor()` kept in shared state because it connects command motor names to the motor table.

Troubleshooting update: command input now uses `stdin` instead of directly reading `UART_NUM_0`, and the project config uses USB Serial/JTAG as the primary console. This matches `/dev/ttyACM0` on ESP32-S3 boards and avoids monitor write timeouts caused by UART0 GPIO43/44 console input mismatch.

Added two binary sensors, `S0` on GPIO4 and `S1` on GPIO5. A sensor reader task polls them every 20 ms and prints machine-readable event lines when `watchsensors on` is enabled.

Added a basic quadrature encoder setup for `M0` using ESP-IDF PCNT. GPIO17 is encoder channel A and GPIO18 is encoder channel B. The encoder setup configures both encoder pins as inputs with internal pullups, uses full x4 counting, and applies a 1000 ns PCNT glitch filter. `getencoder M0` prints a one-shot diagnostic line with count plus raw GPIO17/GPIO18 levels.

Added a combined motor PID task. `motor_pid_task` reads PCNT at a fixed 20 ms tick, calculates current speed in counts/sec from encoder count deltas, smooths it with a 5-sample moving average, stores `M0.position` and `M0.current_speed`, updates the speed controller when enabled, and writes BTS7960 RPWM/LPWM LEDC hardware directly. Startup speed averaging divides by the number of real samples collected so the first few samples are not biased low.

Renamed the motor PID source file to `main/tasks/motor_pid_task.c`. Inside it, `update_motor_speed_control()` owns the speed-control math and shared motor PWM/direction request updates, while `apply_motor_output()` owns the final GPIO/LEDC hardware writes.

Speed control now uses measured speed-to-PWM feed-forward plus P/D trim. `motor_pid_task` looks up a base PWM from the target speed, calculates speed error, applies `speed_kp` and tick-normalized `speed_kd`, clamps the requested PWM to `CONVEYOR_SPEED_PID_PWM_MAX`, then slews actual `M0.pwm` toward that request by `CONVEYOR_PWM_SLEW_STEP` each 20 ms tick. Motor direction comes from the sign of `target_speed`, but reversals first ramp PWM down to zero before changing the direction GPIO.

The current base PWM table was measured manually: `0->0`, `8->360`, `16->1040`, `24->1650`, `32->2270`, `48->3490`, `64->4670`, `72->5340`, `80->6050`, `88->6570`, `96->7230`, `104->7890`, `112->8590`, `120->9260`, `128->9870`. The code linearly interpolates between those points.

Current tested speed gains with the feed-forward table are `speed_kp = 0.010` and `speed_kd = 0.010`. This was reported as working well on the current no-load/manual test setup.

Conveyor jobs now only request fixed-direction run/stop for a named motor. `start_motor("M0")` uses `CONVEYOR_MOTOR_FORWARD_DIRECTION` and runtime `run_speed_counts_per_sec`. Normal TX/RX completion uses `stop_motor("M0")`, which sets target speed to zero and lets `motor_pid_task` ramp down. Errors, emergency stops, and explicit serial stops still use immediate stop behavior.

MQTT `tx`/`rx` commands do not carry speed. They only queue high-level jobs, so job motor speed always comes from runtime config `run_speed_counts_per_sec`. If serial `setspeed M0 <value>` feels correct but MQTT jobs crawl, check `getconfig run_speed_counts_per_sec` and set it with `setconfig run_speed_counts_per_sec <value>`.

Added a central conveyor job state machine for high-level tray transfer jobs. MQTT and microrl can submit TX/RX/emergency/clear-error commands to the same queue, while the state machine owns the active job. DONE states auto-return to `IDLE` after a short report hold.

Added MQTT support in the same style as the senior gantry repo: hardcoded WiFi/broker/topic config, `espressif/mqtt` dependency, WiFi/MQTT setup in its own module, JSON high-level command parsing, and feedback publishing. MQTT does not expose raw PWM commands.

Added runtime-editable config values backed by NVS. Serial debug commands can read, set, and reset runtime-safe values such as `run_pwm`, `run_speed_counts_per_sec`, `speed_kp`, `speed_kd`, transfer timeouts, done hold time, and MQTT status period. Compile-time defaults still live in `main/config/config.h`.

Added `resetk` as a narrow gain reset command. It restores only `speed_kp` and `speed_kd` to the defaults from `main/config/config.h`, saves them to NVS, and leaves every other runtime config value unchanged.

Config files now include inline comments for each compile-time parameter and runtime config getter/key meaning, so tuning values can be understood without chasing their use sites.

Expanded the conveyor state-machine documentation with detailed TX/RX timeout meanings, timer start points, physical sensor mapping, failure causes, and tuning notes.

Removed the old conveyor `left`/`right` job convention. The conveyor now always moves trays from `S0` to `S1`. Logical job sensors are zero-based: `tx0 = S0`, `tx1 = S1`, `rx0 = S0`, and `rx1 = S1`.

Added `docs/mqtt-implementation.md` with MQTT startup flow, compile-time switches, exact parser behavior, queue handoff, feedback publishing, runtime status period behavior, and current MQTT limits.

Added central tray-presence status based on the physical assumption that tray length is greater than the distance between `S0` and `S1`. `has_tray` is true when either sensor detects a tray. MQTT publishes change-driven tray status on `conveyor/C0/tray`, and serial debug exposes `gettray`.

MQTT feedback now includes `state_elapsed_ms`, the elapsed milliseconds since the current conveyor state was entered. MQTT command errors now include the real current conveyor state, timer, and raw sensor values.

Current sensor interpretation is active-low: raw GPIO `0` means tray detected, and raw GPIO `1` means no tray.

Rebuilt `docs/architecture.mmd` and `docs/code-structure.mmd` as cleaner top-to-bottom `stateDiagram-v2` diagrams. The architecture diagram now separates external inputs, readers, shared control, motor control, and outputs. The code-structure diagram is now a very small source-area map instead of function-level wiring.

Added file-level folder detail to `docs/code-structure.mmd` while keeping the same small set of high-level arrows.

Added brief purpose labels under each `docs/code-structure.mmd` block and file item so the diagram explains what each source area does without adding more arrows.

Reworked `docs/architecture.mmd` with the same layered Mermaid method: broad runtime blocks first, brief purpose labels inside each block, and only the main data/control arrows.

Added `docs/data-flow.mmd` as a simple top-to-bottom data-flow diagram showing external inputs, parsed/sampled values, shared state, control decisions, and output data.

Started design discussion for an SD-card logging system that should stay reusable across similar motor-control projects, including Kinco, CubeMars, and DC-motor variants. No logging code has been added yet.

SD logging design note: company code commonly uses ESP-IDF `ESP_LOG*` macros, so the SD logging design should support ESP_LOG capture for human-readable firmware diagnostics. Structured CSV/event files should still exist separately for motor/sensor/job analysis because ESP_LOG text is not a stable data format.

Gantry logging reference checked: `/home/anbu/Z/projects/internship/antropi/gantry/Kinco_Gantry` uses `ESP_LOG*` heavily for firmware diagnostics and its `project.md` plans SD logging as a separate logger task with uptime timestamps, structured events, and optional `ESP_LOG*` tee/capture. It does not currently contain a finished SD logger implementation. `/home/anbu/Z/projects/internship/antropi/gantry/kinco_control_firmware` is only a small GPIO smoke test using `printf`, so it is not a useful logging reference.

Detailed gantry logging pattern: app files include `esp_log.h`, define `static const char *TAG = "module"`, and use `ESP_LOGI/W/E(TAG, ...)` for state transitions, MQTT connect/disconnect/RX, CANopen SDO/PDO setup, drive reconnects, command rejections, homing progress, limit hits, and persistence failures. Raw `printf` remains in `main/canopen_listener.c` for CAN frame dumps, which means SD log capture should either convert that listener to `ESP_LOG*` or keep a separate raw-frame logging path.

## Mermaid Diagram Update Methodology

For future `.mmd` updates, start with the simplest useful diagram first.
Use `stateDiagram-v2` with `direction TB` unless there is a clear reason not to.
Keep inputs or startup blocks near the top, shared/control blocks in the middle,
and outputs/platform blocks near the bottom.

Add detail slowly in layers:

1. Start with only the main blocks and a few high-level arrows.
2. Add file or folder names inside existing blocks before adding new arrows.
3. Add short purpose labels under blocks or files only after the structure is readable.
4. Avoid per-function or per-call dependency wiring unless specifically needed.
5. If the diagram starts getting crossed lines or long horizontal sprawl, remove arrows first instead of adding layout tricks.

Render-check Mermaid changes with `mmdc`, usually through:

```text
npx -p @mermaid-js/mermaid-cli mmdc -i docs/<file>.mmd -o /tmp/<file>.png
```

## Laptop MQTT Quickstart

The conveyor firmware is an MQTT client. It does not host the broker. A laptop
should connect to the same broker configured in `main/config/config.h`.

Current broker and topics:

```text
Broker: 192.168.1.126
Command topic: conveyor/C0/cmd
Feedback topic: conveyor/C0/feedback
Tray topic: conveyor/C0/tray
Emergency topic: conveyor/C0/emergency
All-conveyors emergency topic: conveyor/all/emergency
```

Useful laptop commands with Mosquitto clients:

```text
mosquitto_sub -h 192.168.1.126 -t 'conveyor/C0/#' -v
mosquitto_pub -h 192.168.1.126 -t conveyor/C0/cmd -m '{"type":"tx"}'
mosquitto_pub -h 192.168.1.126 -t conveyor/C0/cmd -m '{"type":"rx"}'
mosquitto_pub -h 192.168.1.126 -t conveyor/C0/emergency -m '{"type":"emergency_stop"}'
mosquitto_pub -h 192.168.1.126 -t conveyor/C0/cmd -m '{"type":"clear_error"}'
```

Serial output is now token-based for a Python wrapper:

```text
READY conveyor
OK SETMOTOR M0
OK STOPMOTOR M0
OK STOP
OK WATCHSENSORS ON
OK WATCHSENSORS OFF
OK WATCHENCODER M0 ON
OK WATCHENCODER M0 OFF
ENCODER M0 120 1 0
OK SETSPEED M0
OK SETKP 0.500
OK SETKD 0.000
OK RESETK
MOTOR M0 12 1 1200 5000 4800 1
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
EVENT ENCODER M0 120 100
EVENT JOB C0 TX_WAIT_FOR_TX1_DETECT
CONFIG run_pwm 128
CONFIG run_speed_counts_per_sec 100
CONFIG speed_kp 0.500
CONFIG speed_kd 0.000
```

## Files

- `CMakeLists.txt`: Top-level ESP-IDF project file.
- `main/CMakeLists.txt`: Main component build file. Depends on ESP-IDF GPIO, LEDC, PCNT, and `microrl`.
- `main/idf_component.yml`: ESP-IDF component dependency manifest for `espressif/mqtt`.
- `main/main.c`: ESP-IDF app startup, mutex creation, setup calls, task creation, and `READY conveyor`.
- `main/config/config.h`: Hardcoded conveyor ID, MQTT, open-loop speed, task, and timeout config.
- `main/config/runtime_config.h`: Runtime config getter/setter API.
- `main/config/runtime_config.c`: NVS-backed runtime config loading, validation, printing, setting, and reset.
- `main/shared/app_state.h`: Shared structs, constants, globals, and task/setup prototypes.
- `main/shared/app_state.c`: Motor table, sensor table, shared mutex globals, console printing, and motor lookup.
- `main/tasks/command_task.c`: Strict command parser and `microrl_task`.
- `main/tasks/motor_task.c`: LEDC/direction GPIO setup.
- `main/tasks/motor_pid_task.c`: Combined motor PID task that reads PCNT, calculates speed, controls PWM, and writes motor hardware.
- `main/tasks/mqtt_task.h`: MQTT setup, status task, and publishing API.
- `main/tasks/mqtt_task.c`: WiFi/MQTT setup, JSON parsing, command queue submission, and status publishing.
- `main/tasks/sensor_task.c`: Sensor GPIO setup and `sensor_reader_task`.
- `main/tasks/encoder_task.c`: Encoder PCNT setup.
- `main/conveyor/conveyor_job.h`: Conveyor command, state, status, setup, and task declarations.
- `main/conveyor/conveyor_job.c`: Central TX/RX conveyor transfer state machine and job queue setup.
- `components/microrl/`: Small vendored microrl-style command parser used by this app.
- `docs/architecture.mmd`: Mermaid chart of the current command and motor-control flow.
- `docs/code-structure.mmd`: Mermaid chart of files, functions, callbacks, and shared state.
- `docs/data-flow.mmd`: Mermaid chart of command, sensor, encoder, runtime config, motor, serial, and MQTT data flow.
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
- `target_speed`
- `current_speed`
- `pos_control`
- `speed_control`
- `rpwm_gpio`
- `lpwm_gpio`
- `ren_gpio`
- `len_gpio`
- `encoder_a_gpio`
- `encoder_b_gpio`
- `rpwm_ledc_channel`
- `lpwm_ledc_channel`
- `pcnt_unit`

`sensor_t` currently stores:

- `name`
- `gpio`
- `value`
- `last_value`

Current motor:

- `M0`
- RPWM GPIO: `GPIO_NUM_15`
- LPWM GPIO: `GPIO_NUM_16`
- REN GPIO: `GPIO_NUM_6`
- LEN GPIO: `GPIO_NUM_7`
- Encoder A GPIO: `GPIO_NUM_17`
- Encoder B GPIO: `GPIO_NUM_18`
- RPWM LEDC channel: `LEDC_CHANNEL_0`
- LPWM LEDC channel: `LEDC_CHANNEL_1`

Current sensors:

- `S0`
- GPIO: `GPIO_NUM_4`
- `S1`
- GPIO: `GPIO_NUM_5`

## Commands

Strict commands currently supported:

```text
setmotor M0 128 1
setspeed M0 100
setkp 0.500
setkd 0.000
resetk
stopmotor M0
stop
watchsensors on
watchsensors off
watchencoder M0 on
watchencoder M0 off
getencoder M0
getmotor M0
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

- `setmotor M0 128 1`: sets `M0.pwm = 128`, `M0.direction = 1`, and disables speed control.
- `setspeed M0 100`: sets `M0.target_speed = 100` counts/sec and enables speed control.
- `setkp 0.500`: saves the speed P gain to NVS.
- `setkd 0.000`: saves the speed D gain to NVS.
- `resetk`: restores `speed_kp` and `speed_kd` to compile-time defaults and saves them to NVS.
- `stopmotor M0`: sets `M0.pwm = 0` and disables speed control.
- `stop`: sets PWM to `0` for all motors and disables speed control.
- `watchsensors on`: enables sensor event printing.
- `watchsensors off`: disables sensor event printing.
- `watchencoder M0 on`: enables encoder count and speed event printing.
- `watchencoder M0 off`: disables raw encoder count event printing.
- `getencoder M0`: prints `ENCODER M0 <count> <gpio17_a> <gpio18_b>`.
- `getmotor M0`: prints `MOTOR M0 <pwm> <direction> <position> <target_speed> <current_speed> <speed_control>`.
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
- `conveyor_job_task`: owns the TX/RX state machine and submits speed/stop requests to the motor state.
- `motor_pid_task`: reads PCNT, calculates smoothed speed, calculates a feed-forward base PWM plus P/D trim, slews actual PWM toward it, and writes direction GPIO plus LEDC PWM hardware.
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
- GPIO15, GPIO16, GPIO6, GPIO7, GPIO17, and GPIO18 are valid on the actual board.
- The BTS7960 `RPWM` pin is connected to GPIO15.
- The BTS7960 `LPWM` pin is connected to GPIO16.
- The BTS7960 `REN` pin is connected to GPIO6.
- The BTS7960 `LEN` pin is connected to GPIO7.
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
- Encoder `M0` channel A is GPIO17.
- Encoder `M0` channel B is GPIO18.
- Encoder GPIO17/GPIO18 are configured as inputs with internal pullups before PCNT setup.
- Encoder PCNT uses full x4 quadrature counting.
- Encoder PCNT uses a 1000 ns hardware glitch filter to reject very short noise pulses.
- PID speed control currently has P and optional D terms. Integral control is not implemented yet.
- Speed control uses a measured speed/PWM table plus P/D trim. Actual PWM changes by `CONVEYOR_PWM_SLEW_STEP` each motor PID tick.
- Speed measurement uses a 5-sample moving average without low startup bias.
- Encoder filtering, zeroing, MQTT publishing, and position control are not implemented yet.

## Next Useful Commands

```bash
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```
