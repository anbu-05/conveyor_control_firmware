# App State Ownership Cleanup Plan

## Goal

Clarify and clean up `app_state` access without hiding `motors[]` or introducing a second internal header.

`app_state` remains a shared storage warehouse. Each task/module owns a set of fields and exposes header functions for reading/writing the fields it owns. Tasks should use those owner APIs whenever practical, including from inside the owner module itself when that keeps behavior centralized.

## Chosen Direction

- Keep `main/shared/app_state.h` exposing `motors[]`, `motor_t`, and `motor_mutex`.
- Document that this is a C compromise: in C++ the storage could be made private/read-only through class boundaries, `const` views, friends, and RAII locking.
- Do not create `app_state_internal.h` or split public/private headers.
- Do not make `app_state` a large generic accessor API.
- Prefer owner-module APIs:
  - `hardware.h` owns hardware output and hardware feedback APIs.
  - `pid.h` owns PID control, targets, PID mode, gains, position/speed, and PID runtime reset APIs.
  - `statemachine.h` owns state-machine jobs/status.
- User-facing tasks such as `console.c` and `mqtt.c` should call owner-module APIs rather than editing state directly, except where a direct read/write is intentionally kept and clearly documented.

## Ownership Map

### `app_state.c/.h`

Role: storage warehouse.

Owns:
- Allocation/static storage of `motors[]`.
- Creation of `motor_mutex`.
- Initial default values.

Does not own:
- High-level behavior.
- Broad `get_*` / `set_*` APIs for every field.

### `hardware.c/.h`

Owns:
- Motor hardware initialization.
- PWM/direction output state: `pwm`, `direction`.
- Encoder/sensor publishing: `encoder_count`, `current_position`, `speed`, `upstream_sensor`, `downstream_sensor`.
- Hardware config fields: GPIOs, LEDC channels, PCNT handles.

Existing APIs:
- `hardware_init()`
- `set_motor()`
- `stop_motor()`
- `hardware_task()`

Add APIs:
- `esp_err_t hardware_get_motor_id(int index, const char **out_motor_id);`
- `esp_err_t hardware_get_sensors(const char *motor_id, int *out_upstream_sensor, int *out_downstream_sensor);`
- Optional if needed by callers: `esp_err_t hardware_get_feedback(const char *motor_id, int *out_position, int *out_speed);`

Implementation note:
- `hardware.c` may still directly use `motors[]` because it owns hardware state.
- Inside `hardware.c`, keep using `set_motor()` and `stop_motor()` where that centralizes output behavior.

### `pid.c/.h`

Owns:
- PID enable/control ownership: `PID_control`.
- PID mode: `pid_mode`.
- Targets: `target_position`, `target_speed`.
- PID gains: `kp`, `ki`, `kd`.
- PID runtime memory: `integral`, `previous_error`, `has_previous_error`.
- Position/speed public APIs, even though hardware publishes the raw measurements.

Existing APIs:
- `motor_pid_init()`
- `motor_pid_task()`
- `set_position()` / `get_position()`
- `set_speed()` / `get_speed()`
- `set_offset()`
- `set_pid_gains()` / `get_pid_gains()`

Add APIs:
- `esp_err_t pid_set_control(const char *motor_id, bool enabled);`
- `esp_err_t pid_get_control(const char *motor_id, bool *out_enabled);`
- `esp_err_t pid_set_mode(const char *motor_id, motor_pid_mode_t mode);`
- `esp_err_t pid_get_mode(const char *motor_id, motor_pid_mode_t *out_mode);`
- Optional private/static helper in `pid.c`: `reset_pid_memory_locked(motor_t *motor);`

Implementation note:
- Move duplicated PID-control reset logic from `console.c` and `statemachine.c` into `pid_set_control()`.
- Move PID-mode reset logic from `console.c` into `pid_set_mode()`.
- `set_position()` and `set_speed()` should continue selecting their mode and resetting PID memory.

### `statemachine.c/.h`

Owns:
- Tray job flow.
- State-machine status and results.

Should stop directly touching:
- `motors[0].upstream_sensor`
- `motors[0].downstream_sensor`
- `motors[0].PID_control`
- PID memory fields.

Use instead:
- `hardware_get_motor_id(0, &motor_id)` for configured motor id.
- `hardware_get_sensors(motor_id, &upstream, &downstream)` for tray sensors.
- `pid_set_control(motor_id, false)` before raw state-machine motion.
- `set_motor(motor_id, ...)` and `stop_motor(motor_id)` for raw output.

### `console.c`

Role: user-facing command adapter.

Should avoid direct ownership of PID/hardware fields. It should parse CLI input, call owner APIs, and print results.

Replace direct touches:
- `set_console_pid_control()` should be removed or converted into a thin wrapper around `pid_set_control()`.
- `setposition` should use `pid_get_control()` if the explicit pre-check is retained, or rely on `set_position()` returning `ESP_ERR_INVALID_STATE`.
- `get_pidmode` should call `pid_get_mode()`.
- `set_pidmode` should call `pid_set_mode()`.
- `getsensors` should call `hardware_get_sensors()`.
- `stop` and status loops should call `hardware_get_motor_id()` instead of reading `motors[i].id` directly.

Allowed direct reads if kept intentionally:
- Very simple motor-id iteration can remain direct if desired, but prefer `hardware_get_motor_id()` for consistency.

### `mqtt.c`

Role: user/backend-facing messaging adapter.

Replace direct touches:
- `get_has_tray()` should call `hardware_get_motor_id(0, &motor_id)` and `hardware_get_sensors(motor_id, &upstream, &downstream)`.

### `main.c`

Replace direct touches:
- Use `hardware_get_motor_id(i, &motor_id)` before calling `hardware_init()`, `motor_pid_init()`, and `xTaskCreate(motor_pid_task, ..., motor_id, ...)`.

## Implementation Steps

1. Update comments in `app_state.h`.
   - Describe `app_state` as shared storage.
   - State that `motors[]` is exposed as a C compromise.
   - State the convention: modules should use owner APIs for behavior and cross-module access.
   - Mention that C++ could enforce this boundary more strongly.

2. Add hardware read APIs to `hardware.h` and `hardware.c`.
   - `hardware_get_motor_id()` validates index and returns `motors[index].id`.
   - `hardware_get_sensors()` resolves motor id, locks `motor_mutex`, copies both sensor fields, unlocks.
   - Optionally add `hardware_get_feedback()` only if needed during cleanup.

3. Add PID control/mode APIs to `pid.h` and `pid.c`.
   - `pid_set_control()` validates motor id, locks, sets `PID_control`, resets `target_position` to `current_position`, clears `target_speed`, resets PID memory, unlocks.
   - `pid_get_control()` validates, locks, reads `PID_control`, unlocks.
   - `pid_set_mode()` validates mode, locks, sets `pid_mode`, resets PID memory, unlocks.
   - `pid_get_mode()` validates, locks, reads `pid_mode`, unlocks.
   - Keep reset behavior consistent with existing code.

4. Refactor `console.c`.
   - Replace `set_console_pid_control()` logic with `pid_set_control()`.
   - Replace direct `PID_control` read with `pid_get_control()` or remove the redundant check and rely on `set_position()` / `set_speed()` errors.
   - Replace direct `pid_mode` read/write with `pid_get_mode()` / `pid_set_mode()`.
   - Replace direct sensor reads with `hardware_get_sensors()`.
   - Replace direct motor-id loop reads with `hardware_get_motor_id()` where practical.

5. Refactor `statemachine.c`.
   - Replace direct sensor snapshot reads with `hardware_get_sensors()`.
   - Replace direct PID disable/reset with `pid_set_control(motor_id, false)`.
   - Replace `motors[0].id` reads with a small local helper that calls `hardware_get_motor_id(0, &motor_id)`.
   - Continue using `set_motor()` / `stop_motor()` for output.

6. Refactor `mqtt.c`.
   - Replace direct sensor reads with `hardware_get_sensors()`.
   - Avoid including `shared/app_state.h` unless needed for constants/types.

7. Refactor `main.c`.
   - Use `hardware_get_motor_id()` when initializing hardware/PID and when passing PID task args.

8. Keep owner-module direct access.
   - `hardware.c` may directly access hardware-owned state.
   - `pid.c` may directly access PID-owned state.
   - `app_state.c` owns initialization.
   - Do not attempt to make direct access impossible in this iteration.

## Validation Plan

1. Build the firmware with the normal ESP-IDF command used by the project.
2. Search direct `motors` usage after refactor.
   - Expected remaining files:
     - `main/shared/app_state.c`
     - `main/shared/app_state.h`
     - `main/tasks/hardware.c`
     - `main/tasks/pid.c`
   - Possible acceptable remaining reads:
     - `main/main.c` only if motor-id iteration is intentionally left direct.
3. Search direct `motor_mutex` usage after refactor.
   - Expected remaining files:
     - `main/shared/app_state.c`
     - `main/tasks/hardware.c`
     - `main/tasks/pid.c`
4. Confirm serial commands still behave:
   - `status`
   - `pid_control M0 1`
   - `setposition M0 <value>`
   - `setspeed M0 <value>`
   - `get_pidmode M0`
   - `set_pidmode M0 position|speed`
   - `getsensors M0`
   - `stop`
   - `stopmotor M0`
5. Confirm state-machine jobs still run:
   - `jobrx`
   - `jobtx`
   - `get_smstatus`
6. Confirm MQTT tray presence still publishes correctly after replacing direct sensor reads.

## Risks

- Returning `const char *` motor ids from `hardware_get_motor_id()` exposes pointers into `motors[]`, but this matches current task-arg usage and is acceptable for static app lifetime.
- Keeping `motors[]` public means direct writes remain possible. This is intentionally accepted for now and documented as a C limitation.
- Moving PID reset logic into `pid_set_control()` and `pid_set_mode()` must preserve current reset semantics exactly.
- `console.c` has some user-facing behavior duplication around `PID_CONTROL_DISABLED`; preserve existing output tokens.

## LinkedIn/Screenshot Notes

Short explanation:

In this firmware, `app_state` started as shared storage: every task could include the same header, grab the mutex, and touch `motors[]` directly. That is simple in C, but it blurs ownership. The PID task owns PID state, the hardware task owns sensor/output state, and the state machine owns tray flow, but the compiler does not know any of that.

The cleanup direction is not to build a giant `app_state_get_everything()` layer. Instead, `app_state` stays as the storage warehouse, while each owning module exposes behavior-oriented APIs. `hardware.h` exposes hardware reads/writes, `pid.h` exposes PID reads/writes, and user-facing code like the console and MQTT adapters call those APIs instead of poking fields directly.

Screenshot block 1:

```text
C lets every task see the warehouse shelves.
The architecture says: only the owner should move the boxes.
The compiler says: I have no idea what you mean.
```

Screenshot block 2:

```text
app_state = storage
hardware.c = owns sensors, encoder, PWM output
pid.c = owns PID enable, mode, targets, gains, memory
statemachine.c = owns tray flow, calls hardware/PID APIs
console.c + mqtt.c = user-facing adapters, not state owners
```

C++ note:

C++ would make this boundary much easier to enforce. `motors` could be private, readers could receive const views or snapshots, writers could be limited to friend/owner classes, and mutex handling could use RAII locks. In C, the best practical version is convention plus narrow module APIs: expose the storage if we must, but route behavior through the module that owns the field.
