# conveyor

ESP-IDF project for one conveyor controller using an ESP32-S3, a BTS7960
motor driver, two tray sensors, serial debug commands, and MQTT high-level
control.

## Hardware

- Target: ESP32-S3
- BTS7960 `RPWM` pin: GPIO15
- BTS7960 `LPWM` pin: GPIO16
- BTS7960 `REN` pin: GPIO6
- BTS7960 `LEN` pin: GPIO7
- Encoder `M0` channel A: GPIO17
- Encoder `M0` channel B: GPIO18
- Sensor `S0`: GPIO4
- Sensor `S1`: GPIO5
- ESP32 ground and BTS7960 ground must be connected together.
- Motor power should come from the motor power supply, not from the ESP32.
- The sensor inputs are active-low in the current firmware.
- The sensor inputs use external pullups. Internal pullups and pulldowns are disabled.

Check your exact ESP32-S3 board pinout before wiring. GPIO15, GPIO16, GPIO6,
GPIO7, GPIO17, and GPIO18 must be available on your board.

## Conveyor Sensor Reference

This firmware treats the two physical sensors like this:

```text
S0 = entry sensor
S1 = exit sensor
```

The conveyor always moves a tray from `S0` toward `S1`.

For TX jobs:

```text
tx0 = S0
tx1 = S1
```

For RX jobs:

```text
rx0 = S0
rx1 = S1
```

The sensor spacing is smaller than the tray length, so a tray on the conveyor
always triggers `S0`, `S1`, or both. The firmware derives tray presence as:

```text
has_tray = S0 detected OR S1 detected
```

Sensor GPIO meaning in the current firmware:

```text
GPIO 0 = tray detected
GPIO 1 = no tray
```

MQTT publishes tray-presence changes on:

```text
conveyor/C0/tray
```

MQTT job feedback is published on:

```text
conveyor/C0/feedback
```

Feedback includes `state_elapsed_ms`, the elapsed milliseconds since the
current conveyor state was entered.

Example:

```json
{"id":"C0","state":"TX_WAIT_FOR_TX1_CLEAR","state_elapsed_ms":320,"s0":1,"s1":0}
```

## MQTT Defaults

```text
WiFi SSID: thrd_warehouse
Broker: mqtt://192.168.1.126
Conveyor ID: C0
Command topic: conveyor/C0/cmd
Emergency topic: conveyor/C0/emergency
All-conveyors emergency topic: conveyor/all/emergency
Feedback topic: conveyor/C0/feedback
Tray topic: conveyor/C0/tray
```

MQTT accepts high-level compact payloads only:

```json
{"type":"tx"}
{"type":"rx"}
{"type":"emergency_stop"}
{"type":"clear_error"}
```

Tray status publishes once on MQTT connect, then only when `has_tray` changes.

## Runtime Config

Runtime-editable values are loaded from NVS. Defaults are still defined in
`main/config/config.h`.

Current editable keys:

```text
run_pwm
run_speed_counts_per_sec
speed_kp
speed_kd
done_hold_ms
tx_detect_timeout_ms
tx_clear_timeout_ms
rx_detect_timeout_ms
rx_done_timeout_ms
mqtt_status_period_ms
```

Use the serial debug commands `getconfig`, `setconfig`, and `resetconfig`.
Use `setkp` and `setkd` for decimal speed gain tuning. Use `resetk` to restore
only the P/D speed gains to their defaults.

## Detailed Docs

- [Serial Debug Commands](docs/serial-debug-commands.md)
- [MQTT Control Commands](docs/mqtt-control-commands.md)
- [MQTT Implementation](docs/mqtt-implementation.md)
- [Conveyor Controller State Machine](docs/conveyor-state-machine.md)

## Serial Output Format

The useful output lines are machine-readable. A Python wrapper can split each
line by spaces and check the first token.

Startup:

```text
READY conveyor
```

Sensor event examples:

```text
EVENT SENSOR S0 1 0
EVENT SENSOR S0 0 1
EVENT SENSOR S1 1 0
EVENT SENSOR S1 0 1
```

Encoder event examples:

```text
EVENT ENCODER M0 120 100
EVENT ENCODER M0 124 120
EVENT ENCODER M0 116 -80
```

Encoder diagnostic example:

```text
ENCODER M0 120 1 0
```

Job event examples:

```text
EVENT JOB C0 TX_WAIT_FOR_TX1_DETECT
EVENT JOB C0 TX_WAIT_FOR_TX1_CLEAR
EVENT JOB C0 TX_DONE
EVENT JOB C0 IDLE
```

Errors:

```text
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
```

ESP-IDF boot logs can still appear before `READY conveyor`. A wrapper should
ignore lines until it sees `READY conveyor`.

## Build And Flash

From this folder:

```bash
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

## Source Layout

- `main/main.c`: app startup, mutex creation, hardware setup calls, task creation.
- `main/config/config.h`: hardcoded conveyor ID, MQTT, motor speed, and timeout config.
- `main/config/runtime_config.c`: runtime-editable config values loaded from and saved to NVS.
- `main/shared/app_state.c`: shared motor/sensor state, console printing, motor lookup.
- `main/tasks/command_task.c`: microrl serial command task and command handling.
- `main/tasks/motor_task.c`: motor PWM/direction setup.
- `main/tasks/motor_pid_task.c`: combined motor PID task, encoder count reading, speed calculation, and motor output.
- `main/tasks/mqtt_task.c`: WiFi/MQTT setup, JSON command parsing, MQTT status task, and feedback publishing.
- `main/tasks/sensor_task.c`: sensor GPIO setup and polling task.
- `main/tasks/encoder_task.c`: encoder PCNT setup.
- `main/conveyor/conveyor_job.c`: central TX/RX conveyor state machine and job queue setup.

## Current Limits

- Only one motor, `M0`, is configured.
- Two binary sensors, `S0` and `S1`, are configured.
- Basic raw encoder PCNT reading is implemented for `M0` on GPIO17/GPIO18.
- P/D speed control is implemented in `motor_pid_task`.
- Speed control direction follows the target speed sign.
- A measured speed/PWM table estimates the base PWM, then P/D trims around that base request.
- The measured table maps PWM `8..128` to about `360..9870` encoder counts/sec and interpolates between points.
- `motor.pwm` slews toward the requested PWM by `CONVEYOR_PWM_SLEW_STEP` each motor PID tick.
- Direction reversals ramp PWM down to zero before changing the direction GPIO.
- Speed measurement uses a 5-sample moving average without low startup bias.
- Encoder GPIO17/GPIO18 are configured as inputs with internal pullups.
- A 1000 ns PCNT glitch filter rejects very short encoder input noise.
- Encoder filtering, zeroing, MQTT publishing, I control, and position control are not implemented yet.
