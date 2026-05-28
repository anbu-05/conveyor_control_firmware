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

## Serial Commands

Open the ESP-IDF monitor and type commands exactly as shown.

This project is configured for the ESP32-S3 USB Serial/JTAG console, which
usually appears as `/dev/ttyACM0` on Linux.

```text
setmotor M0 128 1
```

Sets motor `M0` to PWM `128` and direction `1`. Prints:

```text
OK SETMOTOR M0
```

```text
stopmotor M0
```

Stops only motor `M0`. Prints:

```text
OK STOPMOTOR M0
```

```text
stop
```

Stops all motors. This is the safety command. Prints:

```text
OK STOP
```

```text
watchsensors on
```

Starts printing sensor change events. Prints:

```text
OK WATCHSENSORS ON
```

```text
watchsensors off
```

Stops printing sensor change events. Prints:

```text
OK WATCHSENSORS OFF
```

The parser is strict:

- `setmotor` must be lowercase.
- `M0` must be uppercase.
- PWM must be from `0` to `255`.
- Direction must be `0` or `1`.

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

Errors:

```text
ERR UNKNOWN_COMMAND
ERR UNKNOWN_MOTOR
ERR BAD_ARGS
ERR BAD_PWM
ERR BAD_DIRECTION
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

## Current Limits

- Only one motor, `M0`, is configured.
- Two binary sensors, `S0` and `S1`, are configured.
- Position control is not implemented yet.
- Encoder PCNT reading is not implemented yet.
- MQTT control is not implemented yet.
- The motor struct already has fields for future position control work.
