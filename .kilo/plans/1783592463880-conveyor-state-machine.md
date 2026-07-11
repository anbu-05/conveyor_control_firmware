# Conveyor State Machine Result And Status Plan

## Goal
Update the state-machine API so callers get the final job outcome directly from `statemachine_jobrx()` / `statemachine_jobtx()`, and add one small checker API for live status polling.

This is not a push stream. The live view is polling-based: whoever wants updates calls `statemachine_get_status()` repeatedly while a job is running.

## Public API
Keep the API small. The public functions should be:

- `esp_err_t statemachine_init(void);`
- `void statemachine_task(void *arg);`
- `statemachine_result_t statemachine_jobrx(void);`
- `statemachine_result_t statemachine_jobtx(void);`
- `statemachine_status_t statemachine_get_status(void);`

Add public result enum in `main/statemachine/statemachine.h`:

- `STATEMACHINE_RESULT_RX_DONE`
- `STATEMACHINE_RESULT_TX_DONE`
- `STATEMACHINE_RESULT_TRAY_ALREADY_PRESENT`
- `STATEMACHINE_RESULT_TRAY_NOT_RECEIVED`
- `STATEMACHINE_RESULT_TRAY_TRANSFER_STUCK`
- `STATEMACHINE_RESULT_NO_TRAY_PRESENT`
- `STATEMACHINE_RESULT_TRAY_HANDOFF_STUCK`
- `STATEMACHINE_RESULT_EMERGENCY_STOP`
- `STATEMACHINE_RESULT_JOB_TIMEOUT`
- `STATEMACHINE_RESULT_JOB_REJECTED`

Add public status enum in `main/statemachine/statemachine.h`:

- `STATEMACHINE_STATUS_IDLE`
- `STATEMACHINE_STATUS_RECEIVE_WAITING_FOR_TRAY`
- `STATEMACHINE_STATUS_RECEIVE_MOVING_TRAY`
- `STATEMACHINE_STATUS_RECEIVE_TRAY_RECEIVED`
- `STATEMACHINE_STATUS_TRANSMIT_TRANSMITTING_TRAY`
- `STATEMACHINE_STATUS_TRANSMIT_TRAY_HANDED_OFF`

Do not expose private transition functions.

## Semantics
`statemachine_jobrx()` and `statemachine_jobtx()` should block until their queued job finishes, then return the terminal result.

`statemachine_get_status()` should return the current live state immediately without blocking.

Important behavior:

- If the queue does not exist, job calls return `STATEMACHINE_RESULT_JOB_REJECTED`.
- If the job request cannot be queued, job calls return `STATEMACHINE_RESULT_JOB_REJECTED`.
- If response synchronization cannot be created, job calls return `STATEMACHINE_RESULT_JOB_REJECTED`.
- If a job is accepted, the caller waits until that exact job completes and receives that exact job's result.
- Multiple queued callers must not receive each other's results.

## Implementation Shape
Use a queue item that includes both the requested job and a response handle:

```c
typedef struct {
    statemachine_job_t job;
    QueueHandle_t response_queue;
} statemachine_request_t;
```

Recommended job call flow:

1. `statemachine_jobrx()` / `statemachine_jobtx()` creates a one-item response queue for `statemachine_result_t`.
2. It sends a `statemachine_request_t` to the state-machine queue.
3. It waits on the response queue until the task sends the final result.
4. It deletes the response queue.
5. It returns the final result.

Recommended task flow:

1. Outer state starts as `STATEMACHINE_STATUS_IDLE`.
2. In idle, wait for a `statemachine_request_t`.
3. For receive request, run the receive state machine and capture its `statemachine_result_t`.
4. For transmit request, run the transmit state machine and capture its `statemachine_result_t`.
5. Send the result to the request's response queue.
6. Set status back to `STATEMACHINE_STATUS_IDLE`.

Use `portMAX_DELAY` while waiting for an accepted job's response because each job has its own internal timeout.

## Status Checker
Add one file-static live status variable in `statemachine.c`, for example:

```c
static volatile statemachine_status_t s_status = STATEMACHINE_STATUS_IDLE;
```

Update `s_status` at every real state transition:

- Idle queue wait: `STATEMACHINE_STATUS_IDLE`
- Receive waiting: `STATEMACHINE_STATUS_RECEIVE_WAITING_FOR_TRAY`
- Receive moving: `STATEMACHINE_STATUS_RECEIVE_MOVING_TRAY`
- Receive done state before acknowledgement: `STATEMACHINE_STATUS_RECEIVE_TRAY_RECEIVED`
- Transmit moving: `STATEMACHINE_STATUS_TRANSMIT_TRANSMITTING_TRAY`
- Transmit handed off before acknowledgement: `STATEMACHINE_STATUS_TRANSMIT_TRAY_HANDED_OFF`

`statemachine_get_status()` should simply return `s_status`.

This provides polling-based live updates without callbacks and without adding a second result API.

## Function Count Decisions
Keep these private functions:

- `read_tray_sensors()`: one protected sensor snapshot helper.
- `finish_job()`: one terminal stop/log helper.
- `start_moving_upstream()`: one movement helper that disables PID ownership and calls `set_motor()`.
- `run_receive_job()`: receive state machine, now returns `statemachine_result_t`.
- `run_transmit_job()`: transmit state machine, now returns `statemachine_result_t`.

Remove these private helpers:

- `submit_job()`: inline queue submission in `statemachine_jobrx()` and `statemachine_jobtx()`.
- `timeout_elapsed()`: inline timeout comparisons using a local `TickType_t now` in each state block.
- `result_name()`: no longer needed if `finish_job()` receives the result enum and logs with a `switch`, or if each terminal block logs explicitly.

Keep `statemachine_result_t`, but move it to the public header because callers now receive it.

## State Machine Behavior
Receive:

1. `statemachine_jobrx()` queues a receive request and waits for the final result.
2. Task sets status to `STATEMACHINE_STATUS_RECEIVE_WAITING_FOR_TRAY`.
3. If either sensor detects a tray immediately, return `STATEMACHINE_RESULT_TRAY_ALREADY_PRESENT`.
4. Wait for downstream detection.
5. If downstream detection times out, return `STATEMACHINE_RESULT_TRAY_NOT_RECEIVED`.
6. Start moving upstream.
7. Set status to `STATEMACHINE_STATUS_RECEIVE_MOVING_TRAY`.
8. Wait for upstream detection.
9. If upstream detection times out, stop motor and return `STATEMACHINE_RESULT_TRAY_TRANSFER_STUCK`.
10. Set status to `STATEMACHINE_STATUS_RECEIVE_TRAY_RECEIVED`.
11. Stop motor and return `STATEMACHINE_RESULT_RX_DONE`.
12. If whole-job timeout fires in any active phase, stop motor if needed and return `STATEMACHINE_RESULT_JOB_TIMEOUT`.

Transmit:

1. `statemachine_jobtx()` queues a transmit request and waits for the final result.
2. If neither sensor detects a tray immediately, return `STATEMACHINE_RESULT_NO_TRAY_PRESENT`.
3. Start moving upstream.
4. Set status to `STATEMACHINE_STATUS_TRANSMIT_TRANSMITTING_TRAY`.
5. Wait until downstream and upstream sensors both clear.
6. If handoff timeout fires, stop motor and return `STATEMACHINE_RESULT_TRAY_HANDOFF_STUCK`.
7. Set status to `STATEMACHINE_STATUS_TRANSMIT_TRAY_HANDED_OFF`.
8. Stop motor and return `STATEMACHINE_RESULT_TX_DONE`.
9. If whole-job timeout fires in any active phase, stop motor and return `STATEMACHINE_RESULT_JOB_TIMEOUT`.

After the task sends any terminal result back to the waiting caller, set status to `STATEMACHINE_STATUS_IDLE`.

## Console Changes
Update console `jobrx` and `jobtx` commands:

- They now block until completion.
- They print the final result returned by the state-machine API.
- They can optionally print the current status before/after the call, but do not add a console streaming loop unless explicitly requested.

If human-readable result strings are needed in console output, implement a small console-local switch or print stable numeric enum values. Do not add another state-machine API function just for string conversion.

## Naming Boundary
Use upstream/downstream notation for all tray/job/state-machine concepts.

Do not use S0/S1, positive/negative, forward/reverse, or raw `dir` wording in state-machine variables, logs, comments, helper names, or operator-facing job commands.

Keep low-level motor-driver terminology where it already belongs:

- `set_motor(motor_id, pwm, direction)` remains the low-level hardware API.
- `motor_t.direction` remains low-level shared motor output state.
- `APP_MOTOR_POSITIVE_DIR_LEVEL` and `APP_MOTOR_NEGATIVE_DIR_LEVEL` remain low-level electrical direction constants for hardware/PID compatibility.
- `rpwm/lpwm/ren/len` remain BTS7960 electrical pin names.
- Console `setmotor <motor_id> <pwm> <dir>` remains a low-level diagnostic escape hatch.

## Comment Rules
Keep comments concise:

- Each remaining function gets one short purpose comment.
- Each major state block gets one short comment.
- Add comments before project function calls only:
  - `read_tray_sensors()`
  - `start_moving_upstream()`
  - `finish_job()`
  - `run_receive_job()`
  - `run_transmit_job()`
  - `set_motor()` and `stop_motor()` inside helpers
- Do not comment library/FreeRTOS calls unless the reason is not obvious.

## Validation Plan
- Run `git diff --check`.
- Confirm public API is exactly init/task/jobrx/jobtx/get_status plus public result/status types.
- Confirm `statemachine_jobrx()` and `statemachine_jobtx()` return `statemachine_result_t`, not `bool`.
- Confirm every accepted queued request receives exactly one response.
- Confirm `s_status` returns to `STATEMACHINE_STATUS_IDLE` after every terminal result.
- Confirm removed private helpers are gone:
  - `submit_job`
  - `timeout_elapsed`
  - `result_name`
- Confirm `main/statemachine/*` still has no `S0`, `S1`, positive/negative conveyor naming, or removed generic event API references.
- Try `idf.py build` only if the ESP-IDF environment is available without destructive cleanup.
- Do not run `idf.py fullclean` unless explicitly approved.

## Risks
- Blocking job calls must not be called from the state-machine task itself, or they would deadlock. Document this in the header comment.
- A polling status checker is not a true push stream. Consumers must poll periodically for live updates.
- Dynamic response queue creation can fail; return `STATEMACHINE_RESULT_JOB_REJECTED` in that case.
- Multiple callers require per-request response queues so results do not get mixed.
