# Conveyor MQTT Topic System

This conveyor firmware supports the centralized MQTT topic pattern used by the backend.

The three main centralized topics are:

```text
factory/conveyor/C0/node-command
factory/conveyor/C0/result
factory/conveyor/C0/node-status
```

## Node Command Topic

Backend publishes conveyor commands here:

```text
factory/conveyor/C0/node-command
```

Command payload example:

```json
{"command_id":"cmd_123","type":"transmit"}
```

`command_id` is used to match backend requests with ESP result messages.

Supported conveyor command types:

```text
transmit
receive
stop
clear_error
get_commands
```

Command summary:

- `transmit`: starts a conveyor transmit job. The conveyor expects a tray to already be present and moves it toward the next station.
- `receive`: arms the conveyor to receive an incoming tray. The motor starts when the entry sensor detects the tray.
- `stop`: immediately stops the conveyor and puts the node into the stopped safety state. This firmware does not currently have a separate soft stop and emergency stop, so `stop` is the safety stop command.
- `clear_error`: clears an error or stopped state and returns the conveyor to idle when safe.
- `get_commands`: asks the ESP to publish the full list of supported command types on the result topic.

## Result Topic

ESP publishes command results here:

```text
factory/conveyor/C0/result
```

Example result flow:

```json
{"command_id":"cmd_123","command_status":"success","message":"received: command accepted"}
{"command_id":"cmd_123","command_status":"success","message":"transmit complete"}
```

`command_status` is only:

```text
success
failure
```

Conditions such as received or busy are described in the `message` field.

Example busy response:

```json
{"command_id":"cmd_124","command_status":"failure","message":"busy: job busy"}
```

Example `get_commands` response:

```json
{"command_id":"cmd_125","command_status":"success","message":"supported commands","commands":["transmit","receive","stop","clear_error","get_commands"]}
```

## Node Status Topic

ESP publishes conveyor state updates here:

```text
factory/conveyor/C0/node-status
```

Example status payload:

```json
{"id":"C0","state":"TX_WAIT_FOR_TX1_CLEAR","state_elapsed_ms":320,"s0":1,"s1":0,"has_tray":true}
```

The ESP publishes `node-status` only when the conveyor state changes. It does not continuously publish this topic, which keeps WiFi and MQTT traffic low.

Important fields:

- `id`: conveyor ID.
- `state`: current conveyor state.
- `state_elapsed_ms`: milliseconds since this state started.
- `s0`, `s1`: raw sensor readings.
- `has_tray`: true when either conveyor sensor sees a tray.
- `error`: included only for error or stopped states.

## Existing Extra Topics

The firmware still keeps the older conveyor-specific topics for direct use and compatibility:

```text
conveyor/C0/cmd
conveyor/C0/emergency
conveyor/all/emergency
conveyor/C0/feedback
conveyor/C0/tray
```

For centralized backend integration, use the three `factory/conveyor/C0/...` topics above.
