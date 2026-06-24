# Conveyor MQTT Topic System

This conveyor firmware supports the centralized MQTT topic pattern used by the backend.

The three main centralized topics are:

```text
factory/conveyor/C0/command
factory/conveyor/C0/result
factory/conveyor/C0/node_status
```

## Command Topic

Backend publishes conveyor commands here:

```text
factory/conveyor/C0/command
```

Command payload example:

```json
{
  "command_id": "cmd_tray_transmit_001",
  "command": "tray_transmit"
}
```

The `001` suffix in example command IDs is only an example sequence number. In production, the backend can use any unique `command_id`; the ESP should copy the full `command_id` into result messages without interpreting its format.

Supported conveyor commands:

```text
ack_test
tray_transmit
tray_receive
stop
clear_error
get_commands
```

Command summary:

- `ack_test`: connectivity check only. The ESP acknowledges that MQTT command handling is alive.
- `tray_transmit`: starts a conveyor transmit job. The conveyor expects a tray to already be present and moves it toward the next station.
- `tray_receive`: arms the conveyor to receive an incoming tray. The motor starts when the entry sensor detects the tray.
- `stop`: immediately stops the conveyor and puts the node into the stopped safety state. This firmware does not currently have a separate soft stop and emergency stop, so `stop` is the safety stop command.
- `clear_error`: clears an error or stopped state and returns the conveyor to idle when safe.
- `get_commands`: asks the ESP to publish the full list of supported command types on the result topic.

`tray_transmit` and `tray_receive` are used instead of plain `transmit` and `receive` so the command names do not conflict with `command_status: "receive"`.

## Result Topic

ESP publishes command acknowledgements and command results here:

```text
factory/conveyor/C0/result
```

Each result includes the same `command_id` received in the command message.

Every valid command should first produce a `receive` result so the backend knows the ESP received the command:

```json
{
  "command_id": "cmd_tray_transmit_001",
  "command": "tray_transmit",
  "command_status": "receive",
  "message": "command received"
}
```

The acknowledgement status is called `receive` because that is the shared factory contract name used by the other nodes. The conveyor motion command is named `tray_receive` to avoid confusion.

Final command result example:

```json
{
  "command_id": "cmd_tray_transmit_001",
  "command": "tray_transmit",
  "command_status": "success",
  "message": "tray transmit complete"
}
```

`command_status` values are:

```text
receive
success
failure
```

`success` and `failure` describe the final command outcome. Conditions such as busy or rejected are described in the `message` field.

Example busy response:

```json
{
  "command_id": "cmd_tray_transmit_002",
  "command": "tray_transmit",
  "command_status": "failure",
  "message": "rejected: busy - conveyor job already active"
}
```

Example `get_commands` response:

```json
{
  "command_id": "cmd_get_commands_001",
  "command": "get_commands",
  "command_status": "success",
  "message": "supported commands",
  "commands": ["ack_test", "tray_transmit", "tray_receive", "stop", "clear_error", "get_commands"]
}
```

## Node Status Topic

ESP publishes conveyor state updates here:

```text
factory/conveyor/C0/node_status
```

Example status payload:

```json
{
  "id": "C0",
  "state": "TX_WAIT_FOR_TX1_CLEAR",
  "state_elapsed_ms": 320,
  "s0": 1,
  "s1": 0,
  "has_tray": true
}
```

The ESP publishes `node_status` only when the conveyor state changes. It does not continuously publish this topic, which keeps WiFi and MQTT traffic low.

Important fields:

- `id`: conveyor ID.
- `state`: current conveyor state.
- `state_elapsed_ms`: milliseconds since this state started.
- `s0`, `s1`: raw sensor readings.
- `has_tray`: true when either conveyor sensor sees a tray.
- `error`: included only for error or stopped states.

The previous conveyor-specific MQTT topics will be reworked into this three-topic system instead of being kept as separate public topics.

## Commands And Expected Responses

### ack_test

Payload:

```json
{
  "command_id": "cmd_ack_001",
  "command": "ack_test"
}
```

Expected response:

```json
{
  "command_id": "cmd_ack_001",
  "command": "ack_test",
  "command_status": "receive",
  "message": "command received"
}
```

### tray_transmit

Payload:

```json
{
  "command_id": "cmd_tray_transmit_001",
  "command": "tray_transmit"
}
```

Expected initial response:

```json
{
  "command_id": "cmd_tray_transmit_001",
  "command": "tray_transmit",
  "command_status": "receive",
  "message": "command received"
}
```

Expected success response:

```json
{
  "command_id": "cmd_tray_transmit_001",
  "command": "tray_transmit",
  "command_status": "success",
  "message": "tray transmit complete"
}
```

Possible failure response:

```json
{
  "command_id": "cmd_tray_transmit_001",
  "command": "tray_transmit",
  "command_status": "failure",
  "message": "rejected: no_tray - no tray is present on the conveyor"
}
```

### tray_receive

Payload:

```json
{
  "command_id": "cmd_tray_receive_001",
  "command": "tray_receive"
}
```

Expected initial response:

```json
{
  "command_id": "cmd_tray_receive_001",
  "command": "tray_receive",
  "command_status": "receive",
  "message": "command received"
}
```

Expected success response:

```json
{
  "command_id": "cmd_tray_receive_001",
  "command": "tray_receive",
  "command_status": "success",
  "message": "tray receive complete"
}
```

Possible failure response:

```json
{
  "command_id": "cmd_tray_receive_001",
  "command": "tray_receive",
  "command_status": "failure",
  "message": "rejected: tray_present - tray is already present on the conveyor"
}
```

### stop

Payload:

```json
{
  "command_id": "cmd_stop_001",
  "command": "stop"
}
```

Expected response:

```json
{
  "command_id": "cmd_stop_001",
  "command": "stop",
  "command_status": "success",
  "message": "stop accepted"
}
```

### clear_error

Payload:

```json
{
  "command_id": "cmd_clear_error_001",
  "command": "clear_error"
}
```

Expected success response:

```json
{
  "command_id": "cmd_clear_error_001",
  "command": "clear_error",
  "command_status": "success",
  "message": "error cleared"
}
```

Possible failure response:

```json
{
  "command_id": "cmd_clear_error_001",
  "command": "clear_error",
  "command_status": "failure",
  "message": "rejected: no_error - conveyor is not in error or stopped state"
}
```

### get_commands

Payload:

```json
{
  "command_id": "cmd_get_commands_001",
  "command": "get_commands"
}
```

Expected response:

```json
{
  "command_id": "cmd_get_commands_001",
  "command": "get_commands",
  "command_status": "success",
  "message": "supported commands",
  "commands": ["ack_test", "tray_transmit", "tray_receive", "stop", "clear_error", "get_commands"]
}
```
