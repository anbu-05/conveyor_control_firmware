# MQTT / State Machine Cleanup Plan

## Goal

Make MQTT a small, production-facing input path that mirrors console’s core state-machine flow without inheriting console’s debug surface.

Console remains the powerful local debug interface. MQTT remains the backend-facing operational interface for:

- `ack_test`
- `tray_receive`
- `tray_transmit`
- `get_commands`

The main architectural cleanup is to move tray-job acceptance/busy rejection into `statemachine.c`, then simplify MQTT so it only parses commands, queues MQTT work, calls public state-machine APIs, and publishes results.

## Current Issues

### 1. MQTT owns a busy check that should belong to the state machine

Current MQTT flow:

- MQTT receives `tray_receive` or `tray_transmit`.
- `mqtt_task()` checks `statemachine_get_status() != STATEMACHINE_STATUS_IDLE`.
- If busy, MQTT publishes failure.
- Otherwise MQTT calls `statemachine_jobrx()` or `statemachine_jobtx()`.

Problem:

- Console does not perform this check.
- The state machine queue is the real owner of job scheduling.
- `statemachine_get_status()` is only a live status signal, not a reliable job-admission lock.
- A queued job can exist while status is still `IDLE`, so the MQTT check can be stale.

Desired flow:

- MQTT and console both call the same public state-machine API.
- `statemachine_jobrx()` / `statemachine_jobtx()` return `STATEMACHINE_RESULT_JOB_REJECTED` if a job is already active or already queued.
- MQTT formats that result for the backend.
- Console prints that result for debug.

### 2. MQTT manually scans malformed JSON for `command_id`

Current code includes:

- `extract_command_id_from_payload()`
- special malformed JSON handling in `handle_mqtt_data()`

Problem:

- Once JSON is malformed, the backend has violated the command contract.
- The scanner is partial JSON parsing by hand.
- It rejects escaped strings and supports only a narrow shape.
- It adds complexity for little value.

Desired behavior:

- If `cJSON_ParseWithLength()` fails, log and drop the payload.
- Do not publish a correlated failure result for malformed JSON.
- Keep correlated failures only for valid JSON with a usable `command_id`.

### 3. MQTT docs and implementation need to stay honest

Current implementation supports four MQTT commands.

The broader MQTT topic doc mentions future commands like:

- `stop`
- `clear_error`
- movement params

Decision:

- Keep MQTT command set intentionally small for current real-use backend control.
- Do not mirror console debug commands into MQTT.
- Update docs separately to mark future commands clearly, or remove them from the current contract.

## Proposed Code Changes

## Phase 1: Remove malformed JSON command_id recovery

### File

`main/tasks/mqtt.c`

### Changes

1. Remove `#include <ctype.h>`.

Reason:

- It is only used by `extract_command_id_from_payload()`.
- Removing the helper removes this dependency.

2. Delete `extract_command_id_from_payload()` entirely.

Reason:

- Malformed JSON should be logged and dropped.
- Avoid partial manual JSON parsing.

3. Simplify malformed JSON handling in `handle_mqtt_data()`.

Current behavior:

```c
if (payload == NULL) {
    if (extract_command_id_from_payload(event->data, event->data_len,
                                        command.command_id, sizeof(command.command_id))) {
        publish_result(command.command_id, "failure", "malformed JSON");
    } else {
        ESP_LOGW(TAG, "dropping malformed MQTT command without usable command_id");
    }
    return;
}
```

New behavior:

```c
if (payload == NULL) {
    ESP_LOGW(TAG, "dropping malformed MQTT command");
    return;
}
```

Comment to include near changed block:

```c
/* Malformed JSON has no trustworthy command envelope, so log it instead of hand-parsing a partial command_id. */
```

Expected result:

- Less code.
- Less fragile parsing.
- MQTT failures remain correlated only when the command JSON is valid enough to contain `command_id`.

## Phase 2: Move busy rejection into statemachine.c

### File

`main/statemachine/statemachine.c`

### Current behavior

`statemachine_jobrx()` and `statemachine_jobtx()` create a response queue and attempt to send a request into `s_job_queue`.

They reject only when:

- `s_job_queue == NULL`
- response queue allocation fails
- `xQueueSend()` fails

But queue length is currently 4, so multiple jobs may be queued.

### Desired behavior

Only one tray job should be accepted at a time.

The state machine should reject a new job when either:

- a job is actively running
- a job is already queued and waiting to run

### Implementation Option A: Queue length 1 plus active flag

Recommended.

1. Change:

```c
#define STATEMACHINE_QUEUE_LEN 4
```

to:

```c
#define STATEMACHINE_QUEUE_LEN 1
```

Reason:

- The conveyor should not backlog real movement commands.
- Backend should send one job, observe completion/status, then send the next job.

2. Add a private static flag:

```c
static volatile bool s_job_active;
```

Reason:

- Queue length 1 rejects queued backlog, but does not by itself tell producers whether the state-machine task has already dequeued and started a job.
- `s_job_active` covers the actively running case.

3. In `statemachine_task()`, set `s_job_active = true` immediately after receiving a request and before running the job.

Sketch:

```c
if (xQueueReceive(s_job_queue, &request, portMAX_DELAY) != pdTRUE) {
    continue;
}

s_job_active = true;
statemachine_result_t result = request.job == STATEMACHINE_JOB_RECEIVE ? run_receive_job() : run_transmit_job();
...
s_status = STATEMACHINE_STATUS_IDLE;
s_job_active = false;
```

Important cleanup note:

- Ensure `s_job_active = false` always happens after a job completes.
- Current state-machine job execution is synchronous and returns a result, so this is straightforward.

4. In `statemachine_jobrx()` and `statemachine_jobtx()`, reject before creating the response queue if a job is active or already queued.

Use FreeRTOS queue inspection:

```c
if (s_job_active || uxQueueMessagesWaiting(s_job_queue) > 0) {
    return STATEMACHINE_RESULT_JOB_REJECTED;
}
```

Reason:

- This prevents unnecessary response queue creation.
- It rejects both currently-running jobs and already-queued jobs.

5. Race consideration

There is still a small race if two producers call `statemachine_jobrx()` / `statemachine_jobtx()` at the same time and both observe no active/queued job before either sends.

For this codebase, that may be acceptable because:

- Console is human-paced.
- MQTT task serializes MQTT commands through its own queue.
- The state-machine queue length of 1 still rejects the second `xQueueSend()` if both race.

The final authority remains `xQueueSend()`. The pre-check is an early rejection optimization, not the only guard.

6. Keep final rejection guard:

```c
if (xQueueSend(s_job_queue, &request, 0) != pdTRUE) {
    vQueueDelete(request.response_queue);
    return STATEMACHINE_RESULT_JOB_REJECTED;
}
```

Reason:

- This remains the real atomic queue-admission check.

### Alternative Option B: Admission mutex

More rigorous but more code.

Add a `SemaphoreHandle_t s_admission_mutex` and guard active/queued checks plus `xQueueSend()`.

Not recommended right now unless simultaneous command sources become a real issue.

Reason:

- More moving parts.
- Current setup does not need perfect multi-producer fairness.
- Queue length 1 plus final `xQueueSend()` rejection is simple and practical.

## Phase 3: Simplify MQTT tray command handling

### File

mqtt.c

### Current behavior

Both tray commands do:

```c
if (statemachine_get_status() != STATEMACHINE_STATUS_IDLE) {
    publish_result(command.command_id, "failure", "rejected: busy - conveyor job already active");
    break;
}

result = statemachine_jobrx();
```

and similarly for transmit.

### Desired behavior

Remove the MQTT-side status check.

New receive flow:

```c
result = statemachine_jobrx();
if (result == STATEMACHINE_RESULT_RX_DONE) {
    publish_result(command.command_id, "success", "tray receive complete");
} else {
    publish_result(command.command_id, "failure", statemachine_result_text(result));
}
```

New transmit flow:

```c
result = statemachine_jobtx();
if (result == STATEMACHINE_RESULT_TX_DONE) {
    publish_result(command.command_id, "success", "tray transmit complete");
} else {
    publish_result(command.command_id, "failure", statemachine_result_text(result));
}
```

Reason:

- MQTT becomes closer to console.
- State-machine API owns job rejection.
- MQTT result publishing remains backend-specific.

Optional message decision:

- Keep using `JOB_REJECTED` as the failure message for rejected jobs.
- Or map `STATEMACHINE_RESULT_JOB_REJECTED` to `"rejected: busy - conveyor job already active"` for backend readability.

Recommendation:

- Keep `statemachine_result_text(result)` for consistency now.
- If backend needs prettier messages later, add a separate MQTT result-message mapper.

## Phase 4: Keep mqtt_status_task

### File

mqtt.c

Do not remove:

- `mqtt_status_task()`
- `publish_node_status_if_changed()`
- `s_status_task_handle`

Reason:

- `mqtt_task()` blocks while waiting for `statemachine_jobrx()` / `statemachine_jobtx()`.
- The backend still needs `node_status` updates during receive/transmit jobs.
- This is protocol behavior, not command-dispatch complexity.

## Phase 5: Documentation alignment

### File

conveyor-mqtt-topic-system.md

This can be a separate commit/change, but should happen soon.

### Changes

1. Make the current command list match firmware:

```text
ack_test
tray_transmit
tray_receive
get_commands
```

2. Move `stop` and `clear_error` to a “Future safety commands” section.

Reason:

- Safety behavior is still placeholder-level.
- MQTT should not advertise commands that firmware rejects as unknown.

3. Remove or mark movement params as future.

Reason:

- Current firmware does not accept per-command movement params.
- Backend currently only cares about sending RX and TX jobs.

4. Keep node status section.

Reason:

- Current MQTT implementation publishes the documented `id`, `status`, and `has_tray` fields.

## Validation Plan

### Build

Use ESP-IDF build through VS Code ESP-IDF tooling.

Expected result:

- No compile errors after removing `<ctype.h>` and `extract_command_id_from_payload()`.
- No warnings about unused includes or symbols.

### Manual serial checks

Use console to verify state-machine behavior remains useful for debug:

```text
jobrx
jobtx
getstatus
```

Expected result:

- Console still calls state-machine APIs directly.
- If a job is already active/queued, console receives `JOB_REJECTED`.

### MQTT checks

Publish valid MQTT commands:

```json
{
  "command_id": "cmd_ack_001",
  "command": "ack_test"
}
```

Expected result:

```json
{
  "command_id": "cmd_ack_001",
  "command_status": "received",
  "message": "command received"
}
```

then:

```json
{
  "command_id": "cmd_ack_001",
  "command_status": "success",
  "message": "ack test ok"
}
```

Publish tray command:

```json
{
  "command_id": "cmd_rx_001",
  "command": "tray_receive"
}
```

Expected result:

- First `received`.
- Final `success` or `failure`.
- If another job is already active/queued, final failure should be `JOB_REJECTED` or the chosen backend-friendly rejection message.

Publish malformed JSON:

```json
{"command_id": "cmd_bad_001", "command":
```

Expected result:

- Firmware logs malformed MQTT command.
- No MQTT result is published.

Publish valid JSON missing command:

```json
{
  "command_id": "cmd_missing_001"
}
```

Expected result:

```json
{
  "command_id": "cmd_missing_001",
  "command_status": "failure",
  "message": "missing command"
}
```

Publish valid JSON unknown command:

```json
{
  "command_id": "cmd_unknown_001",
  "command": "stop"
}
```

Expected result for current checkpoint:

```json
{
  "command_id": "cmd_unknown_001",
  "command_status": "failure",
  "message": "unknown command"
}
```

### Node status checks

During a tray receive/transmit job, observe:

```json
{
  "id": "C1",
  "status": "receiving",
  "has_tray": false
}
```

or:

```json
{
  "id": "C1",
  "status": "transmitting",
  "has_tray": true
}
```

Expected result:

- Status publishes when state or tray presence changes.
- Status still updates while `mqtt_task()` is blocked on a tray job.

## Expected Net Diff

### mqtt.c

Remove:

- `#include <ctype.h>`
- `extract_command_id_from_payload()`
- MQTT-side `statemachine_get_status()` busy checks

Simplify:

- malformed JSON branch in `handle_mqtt_data()`

Keep:

- command table
- command queue
- MQTT event handling
- status task
- result publishing
- `statemachine_result_text()`

### statemachine.c

Change:

- state-machine queue length from 4 to 1
- add private active-job flag
- reject new jobs when active or already queued

Keep:

- public API unchanged
- header unchanged
- console unchanged

### conveyor-mqtt-topic-system.md

Align current MQTT contract with implemented real-use commands.

## Risks

### Race between producers

Queue length 1 and `xQueueSend(..., 0)` still protect against queue backlog. `s_job_active` protects against active job submission. There may be a small observation race before send, but the final queue send remains authoritative.

### Backend expectations for malformed JSON

If backend expects correlated malformed JSON failures, removing recovery changes behavior. Current decision is that malformed JSON should be dropped because it has no trustworthy envelope.

### Console behavior changes

Console may now get `JOB_REJECTED` for overlapping jobs if state-machine admission becomes stricter. That is acceptable because console is debug-powerful, not allowed to violate state-machine ownership.

## Recommendation

Implement in this order:

1. Remove malformed JSON recovery from MQTT.
2. Build.
3. Move job admission into state machine.
4. Remove MQTT busy checks.
5. Build again.
6. Update MQTT docs to match the current real-use command set.