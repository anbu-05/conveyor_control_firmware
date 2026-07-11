# MQTT Layer Implementation Plan

## Goal
Implement `main/tasks/mqtt.c` as the backend-facing MQTT layer described by `docs/conveyor-mqtt-topic-system.md`, following the same style as `console.c`: one command table, one handler switch, private parsing/publishing internals, and a small public header.

Do not implement source changes in planning mode. This plan is for the implementation-capable agent.

## Fixed Decisions
- Build MQTT topics from `APP_MOTOR_MACHINE_ID`; the doc's `C0` examples are examples, not hard-coded values. Current firmware will publish/subscribe under `C1` because `config.h` sets `APP_MOTOR_MACHINE_ID "C1"`.
- Keep `main/tasks/mqtt.h` limited to `mqtt_init()` and `mqtt_task()` only. Do not add MQTT helper functions to the header.
- Implement initial MQTT commands only:
  - `ack_test`
  - `tray_receive`
  - `tray_transmit`
  - `get_commands`
- Do not implement `stop` or `clear_error` yet. They were examples from an older state-machine design and need a real safety/stopped-state API before becoming active MQTT commands.
- Use `cJSON` for JSON parsing and publishing.
- MQTT owns WiFi setup for this checkpoint.
- Use the normal ESP-IDF WiFi/NVS path: implement minimal `nvs_flash_init()` in `nvs_init()` because WiFi normally expects NVS. Comment clearly that this is the minimal WiFi-required NVS setup and future persistence will expand it.
- MQTT event callbacks must not block on tray jobs. The MQTT data callback parses/enqueues commands; `mqtt_task()` executes commands and may block on `statemachine_jobrx()` / `statemachine_jobtx()`.
- Add one private internal MQTT status publisher task so `node_status` can keep updating while `mqtt_task()` is blocked waiting for a tray job result. Do not expose this task in `mqtt.h`.
- If malformed/unknown commands include a usable `command_id`, publish `failure` with that ID. If no `command_id` exists, log and drop because the backend correlation ID is missing.
- Reject `tray_receive` / `tray_transmit` as busy when `statemachine_get_status()` is not `STATEMACHINE_STATUS_IDLE`.
- `get_commands` returns the core four commands with empty `required_params` and `optional_params` arrays.
- Node status is polling-based: the private status task polls `statemachine_get_status()` and tray-present sensors, publishing `node_status` only when backend status or `has_tray` changes.
- Comments should be block-level rationale comments for each edited logical block, not one comment per changed line.

## Files To Edit
- `main/tasks/mqtt.c`
- `main/tasks/mqtt.h` only for comment updates if needed; no new function declarations.
- `main/tasks/nvs.c`
- `main/CMakeLists.txt`
- Optionally `README.md` / `docs/progress.md` only if the implementation request includes documentation updates. Do not add docs unless requested by the implementation prompt.

## Component Requirements
Update `main/CMakeLists.txt` `REQUIRES` to include:
- `mqtt`
- `json`
- `nvs_flash`

Existing requirements already include WiFi/event/netif components.

## Topic Contract
Build these private topic strings in `mqtt.c` from `APP_MOTOR_MACHINE_ID`:
- Command: `factory/conveyor/<APP_MOTOR_MACHINE_ID>/command`
- Result: `factory/conveyor/<APP_MOTOR_MACHINE_ID>/result`
- Node status: `factory/conveyor/<APP_MOTOR_MACHINE_ID>/node_status`

Do not use `APP_MOTOR_TOPIC_NAME` as the only topic segment because the active spec needs three fixed topic names.

## Public Header Boundary
Keep `main/tasks/mqtt.h` as:
- `esp_err_t mqtt_init(void);`
- `void mqtt_task(void *arg);`

The implementation can add private `static` functions in `mqtt.c` only when they materially reduce duplicated code. Avoid many tiny helpers, but callback/task entrypoints required by ESP-IDF and FreeRTOS are acceptable.

## Suggested Private Data
In `mqtt.c`, keep static/private state only:
- MQTT client handle.
- MQTT command queue handle.
- MQTT status task handle or started flag.
- Connected flag.
- Command/result/status topic strings.
- Last published backend status and last `has_tray` value.

Suggested command queue item:
```c
typedef enum {
    MQTT_COMMAND_ACK_TEST,
    MQTT_COMMAND_TRAY_RECEIVE,
    MQTT_COMMAND_TRAY_TRANSMIT,
    MQTT_COMMAND_GET_COMMANDS,
} mqtt_command_id_t;

typedef struct {
    mqtt_command_id_t command;
    char command_id[MQTT_COMMAND_ID_MAX_LEN];
} mqtt_command_t;
```

Use a command table similar to `console.c`:
```c
static const mqtt_command_entry_t s_commands[] = {
    {"ack_test", MQTT_COMMAND_ACK_TEST},
    {"tray_receive", MQTT_COMMAND_TRAY_RECEIVE},
    {"tray_transmit", MQTT_COMMAND_TRAY_TRANSMIT},
    {"get_commands", MQTT_COMMAND_GET_COMMANDS},
};
```

## WiFi/MQTT Lifecycle
`mqtt_init()` should:
1. Create the private MQTT command queue.
2. Build topic strings.
3. Initialize default netif and event loop if they are not already initialized. Handle `ESP_ERR_INVALID_STATE` as non-fatal for event loop creation.
4. Create default WiFi station netif.
5. Initialize WiFi station mode with `APP_MOTOR_WIFI_SSID` and `APP_MOTOR_WIFI_PASS`.
6. Register WiFi/IP event handler(s).
7. Configure the MQTT client with `APP_MOTOR_MQTT_URI` and `APP_MOTOR_MQTT_CLIENT_ID`.
8. Register the MQTT event handler.
9. Start WiFi.
10. Start one private MQTT status publisher task, or lazily start it from `mqtt_task()` once. Keep this private to `mqtt.c`.

WiFi event handling:
- On station start/disconnect, attempt/re-attempt connect.
- On got IP, start the MQTT client if not already started.

MQTT event handling:
- On connect, mark connected and subscribe to command topic.
- On disconnect, mark disconnected.
- On data, parse the JSON payload and enqueue a private `mqtt_command_t` if valid.

## Command Parsing Behavior
Expected command payload:
```json
{
  "command_id": "cmd_tray_transmit_001",
  "command": "tray_transmit"
}
```

Parsing rules:
- `command_id` must be a string and fit the fixed command-id buffer.
- `command` must be a string and match `s_commands[]`.
- Ignore unsupported optional parameters in this checkpoint; current state-machine APIs do not accept movement params.
- For valid parsed commands, publish `received` only after enqueue succeeds.
- If enqueue fails and command_id exists, publish `failure` with message like `rejected: mqtt command queue full`.
- If command is unknown but command_id exists, publish `failure` with message like `unknown command`.
- If command_id is missing/invalid, log and do not publish a result.

## Result Publishing
Publish to `result` topic using the spec fields:
```json
{
  "command_id": "...",
  "command_status": "received|success|failure",
  "message": "..."
}
```

Suggested mappings:
- `ack_test`: `received`, then `success` with `ack test ok`.
- `tray_receive`: `received`, then `success` if `STATEMACHINE_RESULT_RX_DONE`; otherwise `failure` with the state-machine result token.
- `tray_transmit`: `received`, then `success` if `STATEMACHINE_RESULT_TX_DONE`; otherwise `failure` with the state-machine result token.
- `get_commands`: `received`, then `success` with a `commands` array for the core four commands.

Keep result enum string mapping private in `mqtt.c`; do not add a state-machine string API.

## MQTT Command Task Loop
`mqtt_task()` should run the command handler loop:
1. Wait for private MQTT command queue items.
2. Handle each command through one switch, similar to `console.c`.
3. Publish final results.

For tray commands:
- Before calling `statemachine_jobrx()` / `statemachine_jobtx()`, check `statemachine_get_status()`.
- If not idle, publish final `failure` with `rejected: busy - conveyor job already active`.
- If idle, call the blocking state-machine job function and publish success/failure based on the returned result.

Do not perform node-status polling in this same task, because tray job calls block until completion.

## Private MQTT Status Task
Create a private static FreeRTOS task entrypoint in `mqtt.c`, for example `mqtt_status_task(void *arg)`.

This task should:
1. Poll `statemachine_get_status()` and tray-present sensors at a concise private interval.
2. Derive backend `status` and `has_tray`.
3. Publish `node_status` only when backend status or `has_tray` changed.
4. Skip publish when MQTT is disconnected.

This keeps live status updates moving even while `mqtt_task()` is blocked inside `statemachine_jobrx()` / `statemachine_jobtx()`.

## Node Status Publishing
Read tray presence from `motors[0].downstream_sensor` and `motors[0].upstream_sensor` under `motor_mutex`.

Derive backend `status` from `statemachine_get_status()`:
- `STATEMACHINE_STATUS_IDLE` -> `idle`
- `STATEMACHINE_STATUS_RECEIVE_*` -> `receiving`
- `STATEMACHINE_STATUS_TRANSMIT_*` -> `transmitting`
- Unknown/default -> `unknown`

Publish payload:
```json
{
  "id": "<APP_MOTOR_MACHINE_ID>",
  "status": "idle|receiving|transmitting|unknown",
  "has_tray": true
}
```

Do not include raw sensor values or internal state enum names in `node_status`.

## NVS Plan
Update `main/tasks/nvs.c` from no-op to minimal WiFi-required NVS initialization:
- Include `nvs_flash.h`.
- Call `nvs_flash_init()`.
- If it returns no-free-pages/new-version errors, erase and initialize again.
- Keep comments clear that this is only the minimal NVS setup needed by WiFi and will be expanded later for persistence/config storage.

## Comment Requirements For Implementation
For every logical block edited for this MQTT prompt, add a concise rationale comment explaining why that block exists or why that code path is touched.

Examples:
- Before topic construction: explain topics are built from `APP_MOTOR_MACHINE_ID` so firmware can be retargeted by config.
- Before command table: explain MQTT follows console's table/switch style for easy command additions.
- Before command queue enqueue: explain callbacks avoid blocking on tray movement.
- Before NVS init: explain WiFi needs normal ESP-IDF NVS initialization and future persistence will expand it.

Avoid one comment per line.

## Validation Plan
- Run `git diff --check`.
- Build with ESP-IDF if the local environment allows it without destructive cleanup.
- If build is blocked by the existing Python path mismatch requiring `idf.py fullclean`, report that and do not run cleanup unless explicitly approved.
- Static checks:
  - `mqtt.h` exposes only `mqtt_init()` and `mqtt_task()`.
  - `mqtt.c` contains the core four command table entries.
  - No blocking `statemachine_jobrx()` / `statemachine_jobtx()` calls occur inside the MQTT event callback.
  - Topics are built from `APP_MOTOR_MACHINE_ID`.
  - `node_status` publishes only backend status and `has_tray`.
- Runtime/manual checks:
  - Boot connects to WiFi and broker.
  - MQTT subscribes to `factory/conveyor/<id>/command`.
  - `ack_test` publishes `received` then `success`.
  - `tray_receive` publishes `received` then final success/failure.
  - `tray_transmit` publishes `received` then final success/failure.
  - `get_commands` publishes `received` then success with core four commands.
  - Invalid JSON with command_id publishes failure; invalid JSON without command_id logs/drops.
  - `node_status` publishes on state/tray changes while tray jobs are running.

## Risks
- WiFi/NVS setup changes touch boot behavior. Keep NVS initialization minimal and documented.
- The private MQTT status task adds one more task, but avoids expanding the public API or making state-machine jobs non-blocking.
- Optional command parameters in the doc are intentionally ignored until state-machine APIs accept speed/timeout parameters.
- Since `stop` and `clear_error` are intentionally omitted, backend callers must not rely on those commands until the safety/stopped-state layer exists.
