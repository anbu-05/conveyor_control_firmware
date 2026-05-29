# conveyor

ESP-IDF starter project for controlling a motor through a Cytron MD30C.

## Hardware

- Target: ESP32-S3
- Cytron MD30C `P` pin: GPIO7
- Cytron MD30C `D` pin: GPIO6
- Sensor `S0`: GPIO4
- Sensor `S1`: GPIO5
- ESP32 ground and MD30C ground must be connected together.
- Motor power should come from the motor power supply, not from the ESP32.
- The sensor inputs use external pullups. Internal pullups and pulldowns are disabled.

Check your exact ESP32-S3 board pinout before wiring. GPIO7 and GPIO6 must be available on your board.

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

MQTT publishes tray-presence changes on:

```text
conveyor/C0/tray
```

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
- `main/tasks/motor_task.c`: motor PWM/direction setup and output task.
- `main/tasks/mqtt_task.c`: WiFi/MQTT setup, JSON command parsing, MQTT status task, and feedback publishing.
- `main/tasks/sensor_task.c`: sensor GPIO setup and polling task.
- `main/conveyor/conveyor_job.c`: central TX/RX conveyor state machine and job queue setup.

## Current Limits

- Only one motor, `M0`, is configured.
- Two binary sensors, `S0` and `S1`, are configured.
- Encoder PCNT reading is not implemented yet.
- The motor struct already has fields for future position control work.
