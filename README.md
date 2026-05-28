# conveyor

ESP-IDF starter project for controlling a motor through a Cytron MD30C.

## Hardware

- Target: ESP32-S3
- Cytron MD30C `P` pin: GPIO7
- Cytron MD30C `D` pin: GPIO6
- ESP32 ground and MD30C ground must be connected together.
- Motor power should come from the motor power supply, not from the ESP32.

Check your exact ESP32-S3 board pinout before wiring. GPIO7 and GPIO6 must be available on your board.

## Serial Commands

Open the ESP-IDF monitor and type commands exactly as shown.

```text
setmotor M0 128 1
```

Sets motor `M0` to PWM `128` and direction `1`.

```text
stopmotor M0
```

Stops only motor `M0`.

```text
stop
```

Stops all motors. This is the safety command.

The parser is strict:

- `setmotor` must be lowercase.
- `M0` must be uppercase.
- PWM must be from `0` to `255`.
- Direction must be `0` or `1`.

## Build And Flash

From this folder:

```bash
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

## Current Limits

- Only one motor, `M0`, is configured.
- Position control is not implemented yet.
- Encoder PCNT reading is not implemented yet.
- MQTT control is not implemented yet.
- The motor struct already has fields for future position control work.
