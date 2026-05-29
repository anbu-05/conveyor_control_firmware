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
Broker: mqtt://192.168.1.220
Conveyor ID: C0
Command topic: conveyor/C0/cmd
Emergency topic: conveyor/C0/emergency
All-conveyors emergency topic: conveyor/all/emergency
Feedback topic: conveyor/C0/feedback
```

The MQTT status publish period defaults to `CONVEYOR_MQTT_STATUS_PERIOD_MS`.
It can be changed at runtime with serial debug:

```text
setconfig mqtt_status_period_ms 500
```

## Sensor Reference

```text
S0 = entry sensor
S1 = exit sensor
tray movement = S0 toward S1
tx0 = S0
tx1 = S1
rx0 = S0
rx1 = S1
```

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
- Moves the tray from `S0` toward `S1`.
- The state machine applies the TX stop rule using `tx1`.

### Arm RX

Payload:

```json
{"type":"rx"}
```

Effect:

- Arms a receiver job if the conveyor is `IDLE`.
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
{"id":"C0","state":"TX_WAIT_FOR_TX1_CLEAR","s0":1,"s1":0}
```

Done example:

```json
{"id":"C0","state":"RX_DONE","s0":0,"s1":0}
```

Error example:

```json
{"id":"C0","state":"ERROR","error":"RX_DONE_TIMEOUT","s0":1,"s1":1}
```

Bad command example:

```json
{"id":"C0","state":"ERROR","error":"UNKNOWN_COMMAND"}
```

## Parser Notes

The current JSON parser is intentionally simple and literal. It checks for
exact compact payloads like:

```text
{"type":"tx"}
{"type":"rx"}
{"type":"emergency_stop"}
{"type":"clear_error"}
```

Whitespace inside JSON fields is not currently handled by a real JSON parser.
Use compact payloads exactly like the examples above.

Unknown command types, full queues, and busy state publish an error message to
the feedback topic.
