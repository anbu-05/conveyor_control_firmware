# Conveyor MQTT Topic System

This document defines the minimal backend-facing MQTT contract for one conveyor ESP.

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
get_commands
flip_direction
```

Command summary:

- `ack_test`: connectivity check only. The ESP acknowledges that MQTT command handling is alive.
- `tray_transmit`: starts a conveyor transmit job. The conveyor expects a tray to already be present and moves it toward the next station.
- `tray_receive`: arms the conveyor to receive an incoming tray. The motor starts when the entry sensor detects the tray.
- `get_commands`: asks the ESP to publish the full list of supported commands and their parameters on the result topic.
- `flip_direction`: toggles the runtime conveyor direction map so the same firmware direction constants drive the opposite hardware side; while flipped, `tray_receive` waits at the upstream sensor and stops at the downstream sensor.

`tray_transmit` and `tray_receive` are used instead of plain `transmit` and `receive` so the command names do not conflict with `command_status: "received"`.

## Result Topic

ESP publishes command acknowledgements and command results here:

```text
factory/conveyor/C0/result
```

Each result includes the same `command_id` received in the command message.

Every valid command should first produce a `received` result so the backend knows the ESP received the command:

```json
{
  "command_id": "cmd_tray_transmit_001",
  "command_status": "received",
  "message": "command received"
}
```

The acknowledgement status is `received`, matching the shared factory contract used by the other ESP nodes. The conveyor motion command is named `tray_receive` to avoid confusion with this acknowledgement status.

Final command result example:

```json
{
  "command_id": "cmd_tray_transmit_001",
  "command_status": "success",
  "message": "tray transmit complete"
}
```

`command_status` values are:

```text
received
success
failure
```

`success` and `failure` describe the final command outcome. Conditions such as busy or rejected are described in the `message` field.

Command result flow:

```text
ack_test      -> received -> success
tray_transmit -> received -> success/failure
tray_receive  -> received -> success/failure
get_commands  -> received -> success/failure
flip_direction -> received -> success/failure
```

Example rejected-job response:

```json
{
  "command_id": "cmd_tray_transmit_002",
  "command_status": "failure",
  "message": "JOB_REJECTED"
}
```

Example `get_commands` response:

```json
{
  "command_id": "cmd_get_commands_001",
  "command_status": "success",
  "message": "supported commands",
  "commands": [
    {
      "command": "ack_test",
      "required_params": [],
      "optional_params": []
    },
    {
      "command": "tray_transmit",
      "required_params": [],
      "optional_params": []
    },
    {
      "command": "tray_receive",
      "required_params": [],
      "optional_params": []
    },
    {
      "command": "get_commands",
      "required_params": [],
      "optional_params": []
    },
    {
      "command": "flip_direction",
      "required_params": [],
      "optional_params": []
    }
  ]
}
```

The `get_commands` result returns every command implemented by the current firmware. `required_params` lists parameters that must be present in the command payload. `optional_params` lists parameters that may be omitted. Current firmware does not accept per-command movement parameters, so every implemented command returns empty arrays for both fields.

Parameter entries should use this shape:

```json
{
  "name": "example_param",
  "type": "number",
  "description": "Short description of the parameter"
}
```

Future optional parameter entries should also include `default`:

```json
{
  "name": "example_optional_param",
  "type": "number",
  "default": 1000,
  "min": 0,
  "max": 10000,
  "description": "Short description of the optional parameter"
}
```

`command_id` and `command` are common command envelope fields, so they do not need to be repeated as per-command parameters.

## Future Safety Commands

The MQTT contract intentionally stays smaller than the local serial console. Commands such as `stop` and `clear_error` are expected future safety commands, but the current firmware rejects them as unknown MQTT commands.

Movement parameters such as speed and timeout overrides are also future MQTT extensions. Current firmware uses compiled configuration values for tray jobs and does not accept per-command movement params.

## Node Status Topic

ESP publishes conveyor state updates here:

```text
factory/conveyor/C0/node_status
```

Example status payload:

```json
{
  "id": "C0",
  "status": "transmitting",
  "has_tray": true
}
```

The ESP publishes `node_status` only when the conveyor state changes. It does not continuously publish this topic, which keeps WiFi and MQTT traffic low.

Important fields:

- `id`: conveyor ID.
- `status`: simple machine-level conveyor status.
- `has_tray`: true when either conveyor sensor sees a tray.

Possible node statuses:

```text
idle
transmitting
receiving
unknown
```

Firmware internals such as raw sensor values, state machine names, elapsed state timing, and other debugging details should not be part of the main backend-facing `node_status` payload. These debugging details should move to serial debugging instead.

The previous conveyor-specific MQTT topics will be reworked into this three-topic system instead of being kept as separate public topics.
