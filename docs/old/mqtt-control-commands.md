# MQTT Control Commands

MQTT is the remote control interface for high-level conveyor jobs.

It does not accept raw PWM commands. Raw motor commands are available only on
the serial debug interface.

MQTT handling lives in `main/tasks/mqtt_task.c`. Parsed MQTT commands are sent
to the same central conveyor job queue used by serial `jobtx`, `jobrx`,
`estop`, and `clearerror`.

Implementation details are documented in
[`docs/mqtt-implementation.md`](mqtt-implementation.md).

## Defaults

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

The MQTT status publish period defaults to `CONVEYOR_MQTT_STATUS_PERIOD_MS`.
It can be changed at runtime with serial debug:

```text
setconfig mqtt_status_period_ms 500
```

## Laptop Mosquitto Commands

Install Mosquitto clients on the laptop, connect the laptop to the same network
as the ESP32 and broker, then subscribe to all conveyor messages in one terminal:

```bash
mosquitto_sub -h 192.168.1.126 -t 'conveyor/C0/#' -v
```

Use another terminal to publish commands:

```bash
mosquitto_pub -h 192.168.1.126 -t conveyor/C0/cmd -m '{"type":"getdirection"}'
mosquitto_pub -h 192.168.1.126 -t conveyor/C0/cmd -m '{"type":"setdirection","value":"s0tos1"}'
mosquitto_pub -h 192.168.1.126 -t conveyor/C0/cmd -m '{"type":"setdirection","value":"s1tos0"}'
mosquitto_pub -h 192.168.1.126 -t conveyor/C0/cmd -m '{"type":"getrssi"}'
mosquitto_pub -h 192.168.1.126 -t conveyor/C0/cmd -m '{"type":"tx"}'
mosquitto_pub -h 192.168.1.126 -t conveyor/C0/cmd -m '{"type":"rx"}'
mosquitto_pub -h 192.168.1.126 -t conveyor/C0/emergency -m '{"type":"emergency_stop"}'
mosquitto_pub -h 192.168.1.126 -t conveyor/all/emergency -m 'STOP'
mosquitto_pub -h 192.168.1.126 -t conveyor/C0/cmd -m '{"type":"clear_error"}'
mosquitto_pub -h 192.168.1.126 -t conveyor/C0/cmd -m '{"type":"setkp","value":"0.010"}'
mosquitto_pub -h 192.168.1.126 -t conveyor/C0/cmd -m '{"type":"setkd","value":"0.010"}'
mosquitto_pub -h 192.168.1.126 -t conveyor/C0/cmd -m '{"type":"resetk"}'
```

Use `tx` only when a tray is already detected on this conveyor. Use `rx` only
when this conveyor is empty.

## Laptop Python Web GUI

The repo also includes a small browser-based GUI that uses this same MQTT
protocol through a local Python backend. Install its dependencies and run it from
the repo root:

```bash
python3 -m pip install -r tools/conveyor_web/requirements.txt
python3 -m tools.conveyor_web
```

Optional broker and conveyor ID overrides:

```bash
python3 -m tools.conveyor_web --host 127.0.0.1 --port 8080 --mqtt-host 192.168.1.126 --mqtt-port 1883 --id C0
```

Open `http://127.0.0.1:8080` after the server starts. The webapp subscribes to
feedback and tray topics, displays the latest conveyor state, and publishes only
the compact command payloads documented below.

## Sensor Reference

```text
S0 = entry sensor
S1 = exit sensor
tray movement = S0 toward S1
tx0 = S0
tx1 = S1
rx0 = S0
rx1 = S1
has_tray = S0 detected OR S1 detected
```

The distance between `S0` and `S1` is smaller than the tray length. If a tray
is on the conveyor, at least one sensor should be detecting it.

## Command Topic

Publish normal job commands to:

```text
conveyor/C0/cmd
```

### Start TX

Payload:

```json
{"type":"tx"}
```

Effect:

- Starts a transmitter job if the conveyor is `IDLE`.
- Rejected with `NO_TRAY` if neither sensor detects a tray.
- Moves the tray from `S0` toward `S1`.
- The state machine applies the TX stop rule using `tx1`.

### Arm RX

Payload:

```json
{"type":"rx"}
```

Effect:

- Arms a receiver job if the conveyor is `IDLE`.
- Rejected with `TRAY_PRESENT` if a tray is already on this conveyor.
- The motor stays stopped until `rx0` detects a tray.
- When `rx0` detects, the conveyor moves from `S0` toward `S1`.
- The motor stops when `rx1` detects the tray.

### Emergency Stop

Payload:

```json
{"type":"emergency_stop"}
```

Effect:

- Queues an emergency stop.
- The state machine stops all motors and enters `ESTOP`.

### Clear Error

Payload:

```json
{"type":"clear_error"}
```

Effect:

- Clears `ERROR` or `ESTOP`.
- Returns the state machine to `IDLE`.

### Set Speed P Gain

Payload:

```json
{"type":"setkp","value":"0.010"}
```

Effect:

- Saves `speed_kp` to NVS.
- Takes effect immediately because the motor PID task reads runtime config.
- Accepts decimal values from `0.000` to `100.000`.
- The value must be a quoted decimal with no more than 3 digits after the dot.

Success feedback:

```json
{"id":"C0","config":"speed_kp","value":"0.010"}
```

### Set Speed D Gain

Payload:

```json
{"type":"setkd","value":"0.010"}
```

Effect:

- Saves `speed_kd` to NVS.
- Takes effect immediately because the motor PID task reads runtime config.
- Accepts decimal values from `0.000` to `100.000`.
- The value must be a quoted decimal with no more than 3 digits after the dot.

Success feedback:

```json
{"id":"C0","config":"speed_kd","value":"0.010"}
```

### Reset Speed Gains

Payload:

```json
{"type":"resetk"}
```

Effect:

- Restores only `speed_kp` and `speed_kd` to the defaults in
  `main/config/config.h`.
- Saves both defaults to NVS.
- Leaves other runtime config values unchanged.

Success feedback:

```json
{"id":"C0","config":"speed_gains","speed_kp":"0.010","speed_kd":"0.010"}
```

### Set Travel Direction

Payloads:

```json
{"type":"setdirection","value":"s0tos1"}
```

```json
{"type":"setdirection","value":"s1tos0"}
```

Effect:

- Changes the logical travel direction when the conveyor is `IDLE`.
- Rejected with `JOB_BUSY` if not idle.
- Rejected with `BAD_VALUE` for unknown direction strings.

Success feedback:

```json
{"id":"C0","direction":"S0_TO_S1"}
```

```json
{"id":"C0","direction":"S1_TO_S0"}
```

### Get Travel Direction

Payload:

```json
{"type":"getdirection"}
```

Success feedback returns the current direction exactly as the set-direction success payloads.

### Get RSSI

Payload:

```json
{"type":"getrssi"}
```

Success feedback:

```json
{"id":"C0","rssi":-55}
```

Failure (Wi-Fi not connected or read error) returns:

```json
{"id":"C0","state":"IDLE","state_elapsed_ms":...,"error":"RSSI_UNAVAILABLE","s0":...,"s1":...,"direction":"S0_TO_S1"}
```

## Emergency Topics

This conveyor listens to its own emergency topic:

```text
conveyor/C0/emergency
```

It also listens to the shared all-conveyors emergency topic:

```text
conveyor/all/emergency
```

Accepted emergency payloads:

```json
{"type":"emergency_stop"}
```

```text
STOP
```

## Feedback Topic

Feedback is published to:

```text
conveyor/C0/feedback
```

Normal status example:

```json
{"id":"C0","state":"TX_WAIT_FOR_TX1_CLEAR","state_elapsed_ms":320,"s0":1,"s1":0,"direction":"S0_TO_S1"}
```

Done example:

```json
{"id":"C0","state":"RX_DONE","state_elapsed_ms":40,"s0":0,"s1":0,"direction":"S0_TO_S1"}
```

Error example:

```json
{"id":"C0","state":"ERROR","state_elapsed_ms":20,"error":"RX_DONE_TIMEOUT","s0":1,"s1":1,"direction":"S0_TO_S1"}
```

Bad command example:

```json
{"id":"C0","state":"IDLE","state_elapsed_ms":8420,"error":"UNKNOWN_COMMAND","s0":0,"s1":0,"direction":"S0_TO_S1"}
```

`state_elapsed_ms` is the number of milliseconds since the current state was
entered. It resets each time the conveyor state changes.

Other command error values include:

```text
JOB_BUSY
QUEUE_FULL
NO_TRAY
TRAY_PRESENT
BAD_EMERGENCY
BAD_VALUE
CONFIG_SAVE
```

## Tray Topic

Tray presence is published to:

```text
conveyor/C0/tray
```

Payload:

```json
{"id":"C0","has_tray":true,"s0":0,"s1":1}
```

Rules:

- Published once when MQTT connects.
- Published again only when `has_tray` changes.
- Moving from only `S0` active to both sensors active does not publish a new
  tray message if `has_tray` stays `true`.
- `s0` and `s1` are raw GPIO readings.
- Raw sensor `0` means tray detected.
- Raw sensor `1` means no tray detected.

## Parser Notes

The current JSON parser is intentionally simple and literal. It checks for
exact compact payloads like:

```text
{"type":"tx"}
{"type":"rx"}
{"type":"emergency_stop"}
{"type":"clear_error"}
{"type":"setkp","value":"0.010"}
{"type":"setkd","value":"0.010"}
{"type":"resetk"}
{"type":"setdirection","value":"s0tos1"}
{"type":"setdirection","value":"s1tos0"}
{"type":"getdirection"}
{"type":"getrssi"}
```

Whitespace inside JSON fields is not currently handled by a real JSON parser.
Use compact payloads exactly like the examples above.

Unknown command types, full queues, busy state, and tray precondition failures
publish an error message to the feedback topic.
