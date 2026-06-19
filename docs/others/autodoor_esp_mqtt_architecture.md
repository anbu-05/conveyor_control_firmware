# Auto Door ESP MQTT Architecture

This document defines the minimal MQTT contract for one CNC auto door ESP.

Example device:

```text
factory/cnc_1/autodoor
```

## Topics

### 1. Command Topic

```text
factory/cnc_1/autodoor/command
```

ESP subscribes to this topic.

Used by the Python backend to send commands to the auto door ESP.

Supported commands:

```text
ack_test
open
close
stop
```

## 2. Result Topic

```text
factory/cnc_1/autodoor/result
```

ESP publishes to this topic.

Used by the ESP to send the result of a specific command back to the Python backend.

Each result should include the same `command_id` received in the command message.

Possible result statuses:

```text
received
success
failure
    timeout
    error 
busy
```

Behavior:

```text
ack_test -> received
open     -> received -> success/failure
close    -> received -> success/failure
stop     -> received -> success/failure
```

Each result message should include:
```
command_id: Same command_id received from the backend.

status: success/failure.

message: Short human-readable detail about the result.

Example:

{
  "command_id": "cmd_123",
  "status": "failure",
  "message": "open limit switch not reached"
}
```
## 3. Status Topic

```text
factory/cnc_1/autodoor/status
```

ESP publishes to this topic.

Used by the ESP to publish the latest auto door state whenever the state changes.

Possible door statuses:

```text
opened
closed
stopped
unknown
error
```

## Direction Summary

```text
Python backend publishes:
factory/cnc_1/autodoor/command

Python backend subscribes:
factory/cnc_1/autodoor/result
factory/cnc_1/autodoor/status

ESP subscribes:
factory/cnc_1/autodoor/command

ESP publishes:
factory/cnc_1/autodoor/result
factory/cnc_1/autodoor/status
```
