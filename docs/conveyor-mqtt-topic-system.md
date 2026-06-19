# Conveyor MQTT Topic System

This conveyor firmware supports the centralized MQTT topic pattern used by the backend.

The three main topics are:

```text
factory/cnc_1/autodoor/command
factory/cnc_1/autodoor/result
factory/cnc_1/autodoor/status
```

The topic names currently include `autodoor` because they come from the shared centralized architecture. The payloads are conveyor-specific.

## Command Topic

Backend publishes commands here:

```text
factory/cnc_1/autodoor/command
```

Command payload example:

```json
{"command_id":"cmd_123","type":"tx"}
```

Supported conveyor command types:

```text
tx
rx
emergency_stop
clear_error
```

`command_id` is used to match backend requests with ESP result messages.

## Result Topic

ESP publishes command results here:

```text
factory/cnc_1/autodoor/result
```

Example result flow:

```json
{"command_id":"cmd_123","status":"received","message":"command accepted"}
{"command_id":"cmd_123","status":"success","message":"tx complete"}
```

Possible result statuses:

```text
received
success
failure
busy
```

## Status Topic

ESP publishes conveyor state updates here:

```text
factory/cnc_1/autodoor/status
```

Example status payload:

```json
{"id":"C0","state":"TX_WAIT_FOR_TX1_CLEAR","state_elapsed_ms":320,"s0":1,"s1":0,"has_tray":true}
```

This topic is published once when MQTT connects and again whenever the conveyor state changes.

Important fields:

- `id`: conveyor ID.
- `state`: current conveyor state.
- `state_elapsed_ms`: milliseconds since this state started.
- `s0`, `s1`: raw sensor readings.
- `has_tray`: true when either conveyor sensor sees a tray.
- `error`: included only for error or emergency-stop states.

## Existing Extra Topics

The firmware still keeps the older conveyor-specific topics for direct use and compatibility:

```text
conveyor/C0/cmd
conveyor/C0/emergency
conveyor/all/emergency
conveyor/C0/feedback
conveyor/C0/tray
```

For centralized backend integration, use the three `factory/cnc_1/autodoor/...` topics above.
