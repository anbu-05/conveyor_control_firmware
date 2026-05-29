# MQTT Implementation

MQTT support lives in `main/tasks/mqtt_task.c`.

The MQTT layer is only a remote high-level command interface. It does not own
the conveyor transfer logic, and it does not write motor GPIO or PWM directly.
It receives MQTT messages, converts accepted payloads into `conveyor_cmd_t`
commands, and sends those commands to the central conveyor job queue.

## Compile-Time Switches

MQTT is controlled by compile-time defines in `main/config/config.h`.

```text
CONVEYOR_MQTT_ENABLED
CONVEYOR_MQTT_STATUS_ENABLED
```

If `CONVEYOR_MQTT_ENABLED` is `0`, `main.c` does not call `configure_mqtt()`
and does not create `mqtt_status_task`.

If `CONVEYOR_MQTT_STATUS_ENABLED` is `0`, MQTT can still receive commands.
Periodic job-status publishing is skipped, but `mqtt_status_task` is still
created so tray-presence changes can be published.

## Hardcoded MQTT Config

These values are intentionally hardcoded in `main/config/config.h`:

```text
CONVEYOR_WIFI_SSID
CONVEYOR_WIFI_PASS
CONVEYOR_MQTT_BROKER_URI
CONVEYOR_MQTT_TOPIC_CMD
CONVEYOR_MQTT_TOPIC_EMERGENCY
CONVEYOR_MQTT_TOPIC_FEEDBACK
CONVEYOR_MQTT_TOPIC_ALL_EMERGENCY
CONVEYOR_MQTT_TOPIC_TRAY
```

The MQTT client ID is built from the conveyor ID:

```text
conveyor_C0
```

The MQTT status period is different. Its default is in `config.h`, but the
active value is runtime config:

```text
mqtt_status_period_ms
```

It can be changed over serial:

```text
setconfig mqtt_status_period_ms 500
```

## Startup Flow

`main.c` owns task creation.

When MQTT is enabled, startup is:

```text
configure_mqtt()
xTaskCreate(mqtt_status_task)
```

`configure_mqtt()` does setup only:

```text
wifi_init()
mqtt_init()
```

`wifi_init()`:

- Initializes ESP-NETIF.
- Creates the default event loop.
- Creates the default WiFi station interface.
- Registers WiFi/IP event handlers.
- Applies the hardcoded SSID and password.
- Starts WiFi station mode.

`mqtt_init()`:

- Creates the ESP MQTT client.
- Uses `CONVEYOR_MQTT_BROKER_URI`.
- Uses client ID `conveyor_` plus `CONVEYOR_ID`.
- Registers `mqtt_event_handler()`.
- Starts the MQTT client.

## Connection Events

The MQTT connected flag is:

```text
static volatile bool mqtt_connected
```

`mqtt_task_is_connected()` returns that flag.

On `MQTT_EVENT_CONNECTED`:

- `mqtt_connected` becomes `true`.
- The client subscribes to this conveyor's command topic.
- The client subscribes to this conveyor's emergency topic.
- The client subscribes to the all-conveyors emergency topic.

On `MQTT_EVENT_DISCONNECTED`:

- `mqtt_connected` becomes `false`.

On `WIFI_EVENT_STA_DISCONNECTED`:

- `mqtt_connected` becomes `false`.
- WiFi reconnect is requested.

## Subscribed Topics

Normal commands:

```text
conveyor/C0/cmd
```

This conveyor emergency:

```text
conveyor/C0/emergency
```

All conveyors emergency:

```text
conveyor/all/emergency
```

Tray presence:

```text
conveyor/C0/tray
```

## Accepted Payloads

The parser is deliberately simple and strict. It uses exact `strcmp()` checks,
not a JSON parser.

Normal command topic accepts:

```text
{"type":"tx"}
{"type":"rx"}
{"type":"emergency_stop"}
{"type":"clear_error"}
```

Emergency topics accept:

```text
{"type":"emergency_stop"}
STOP
```

Whitespace, extra fields, aliases, and old direction payloads are rejected.

Rejected example:

```text
{"type":"tx","direction":"right"}
```

## Command Handoff

MQTT does not run the TX/RX state machine itself.

Accepted MQTT messages are converted into `conveyor_cmd_t`:

```text
CONVEYOR_CMD_START_TX
CONVEYOR_CMD_START_RX
CONVEYOR_CMD_EMERGENCY_STOP
CONVEYOR_CMD_CLEAR_ERROR
```

Then MQTT calls:

```text
conveyor_job_send_command(command)
```

The conveyor job task later receives the command from the FreeRTOS queue and
owns all state transitions, sensor checks, timeouts, and motor requests.

TX and RX commands are rejected before queueing if the conveyor is not `IDLE`.
TX is also rejected if no tray is present. RX is rejected if a tray is already
present. Emergency stop and clear error are still sent to the queue.

MQTT precondition errors:

```text
NO_TRAY
TRAY_PRESENT
```

## Feedback Publishing

MQTT feedback is published to:

```text
conveyor/C0/feedback
```

Publishing uses:

```text
mqtt_publish_job_status()
```

That function is called from two places:

- `conveyor_job_task`, whenever the job state changes.
- `mqtt_status_task`, periodically.

Normal status format:

```json
{"id":"C0","state":"TX_WAIT_FOR_TX1_CLEAR","s0":1,"s1":0}
```

Error status format:

```json
{"id":"C0","state":"ERROR","error":"RX_DONE_TIMEOUT","s0":1,"s1":1}
```

Command parsing errors are also published to the feedback topic:

```json
{"id":"C0","state":"ERROR","error":"UNKNOWN_COMMAND"}
```

## Tray Publishing

Tray presence is derived by the conveyor job layer:

```text
has_tray = S0 detected OR S1 detected
```

The tray topic is:

```text
conveyor/C0/tray
```

Payload:

```json
{"id":"C0","has_tray":true,"s0":0,"s1":1}
```

Publishing rules:

- Publish once when MQTT connects.
- Publish from `mqtt_status_task` only when `has_tray` changes.
- Do not publish a new tray message when raw `S0/S1` values change but
  `has_tray` stays the same.
- Raw sensor `0` means tray detected.
- Raw sensor `1` means no tray detected.

This is separate from periodic job-status feedback. The tray topic is
change-driven, while job status can still be periodic when
`CONVEYOR_MQTT_STATUS_ENABLED` is enabled.

## Error Feedback

MQTT publishes these parser/queue errors:

```text
UNKNOWN_COMMAND
BAD_EMERGENCY
JOB_BUSY
QUEUE_FULL
NO_TRAY
TRAY_PRESENT
```

These are MQTT command handling errors. They are not the same as conveyor
state-machine errors like `TX_DETECT_TIMEOUT` or `RX_DONE_TIMEOUT`.

## Current Limits

- MQTT topics are compile-time constants.
- WiFi credentials are compile-time constants.
- Broker URI is a compile-time constant.
- Payload parsing is exact string matching.
- MQTT does not expose raw `setmotor`, `stopmotor`, or runtime config commands.
- MQTT does not change `run_pwm` or timeout settings.
- MQTT status publish period is runtime-configurable through serial only.
- MQTT tray presence is change-driven and does not publish continuously.
