# Autodoor Rewrite Checkpoint Completion Status

Updated: 2026-07-02 18:10 IST
Branch: `code-optimization`
Latest local commit before checkpoint 3 work: `41fd813 Rewrite autodoor firmware skeleton`

## Checkpoint 1: Baseline Snapshot And Contract Notes

Status: completed and committed in `41fd813`.

Recorded in:

- `.kilo/plans/1782890319986-checkpoint-1-baseline.md`

Notes:

- ESP32-S3 target confirmed from `sdkconfig`.
- Original `cnc2` active machine defaults captured.
- Original `cnc1` reference defaults captured.
- Rewrite contract and comment requirements captured from the plan.

## Checkpoint 2: Replace File Structure And Build Skeleton

Status: completed and committed in `41fd813`.

Implemented:

- Replaced old monolithic firmware files with the planned module layout under `autodoor/main/`.
- Added skeleton modules for config, shared state, explicit `statemachine`, console, MQTT, limit switches, encoder, motor, PID, safety, and store.
- Updated `autodoor/main/CMakeLists.txt` to list the new sources and include dirs.
- Added `main.c` startup sequence.
- Added console bring-up with `READY autodoor` output.
- Used `statemachine.*` from the start; no `door_job` naming remains.

Validation:

- `idf.py build` succeeded with ESP-IDF 6.0.1 and explicit ESP32-S3 toolchain path.
- `git grep`/search found no stale `door_job` references in `autodoor/main`.

## Checkpoint 3: Config And Runtime Config

Status: completed and committed in `e51ccaf`.

Implemented:

- Kept compile-time config in `autodoor/main/config/config.h` for:
  - machine identity
  - WiFi/MQTT identity
  - motor, limit, and encoder pins
  - direction levels
  - PWM/control timing
- Added runtime defaults as `#define`s in `autodoor/main/config/runtime_config.c`.
- Implemented `runtime_config_t` with editable runtime values.
- Implemented simple enum-key runtime config access:
  - `runtime_config_get()`
  - `runtime_config_set()`
- Implemented one-value default/NVS helpers:
  - `runtime_config_load_default()`
  - `runtime_config_load_nvs()`
  - `runtime_config_store_nvs()`
- Implemented all-value default/NVS helpers:
  - `runtime_config_load_defaults()`
  - `runtime_config_load_all_nvs()`
  - `runtime_config_store_all_nvs()`
- Wired startup to load runtime defaults first, then overlay stored NVS values.

Validation:

- `git diff --check` passes.
- `idf.py build` could not run in the current shell because `idf.py` is not on `PATH`.

Checkpoint 3 files:

- `autodoor/main/config/config.h`
- `autodoor/main/config/runtime_config.c`
- `autodoor/main/config/runtime_config.h`
- `autodoor/main/main.c`

## Checkpoint 4/5: Combined Hardware Controller

Status: committed.

Implemented:

- Replaced the old motor/encoder/limit-switch split with `hardware_task.c/.h`.
- Added simplified `door_motor_t` in `app_state.c/.h` for motor output, encoder position, target fields, position/speed control flags, pinouts, PCNT handle, and open/close limit-switch GPIO levels.
- Exposed `door_motor` directly and uses `motor_mutex` as the required guard for consistency-sensitive multi-field reads/writes instead of adding field-specific app-state setter helpers.
- Wired startup to call `hardware_init()` and create `hardware_task()` from `main.c` instead of separate motor, encoder, and limit-switch startup functions.
- Added explicit hardware init functions:
  - `hardware_motor_init()`
  - `hardware_encoder_init()`
  - `hardware_limit_switch_init()`
- Configured motor direction GPIO and LEDC PWM output in the combined hardware task module.
- Configured ESP32-S3 PCNT quadrature counting and stores the latest count in the motor snapshot.
- Configured open/close physical limit-switch GPIO inputs and stores their direct GPIO levels in the motor snapshot.
- Added raw motor functions:
  - `set_motor()`
  - `stop_motor()`

Validation:

- `git diff --check` passes.
- `idf.py build` could not run in the current shell because `idf.py` is not on `PATH`.

## Checkpoint 6: PID Motor Control Task

Status: implemented locally, not committed yet.

Finalized plan:

- Implement simple position PID control in `motor_pid_task.c` without adding a PID library dependency.
- Add live float `kp`, `ki`, and `kd` fields plus `position_offset` to `door_motor_t`.
- Keep raw `encoder_count` as PCNT output and publish `current_position = encoder_count + position_offset`.
- Use existing runtime-config PID defaults/NVS values only as startup inputs; convert milli-unit config values to live float gains in `motor_pid_init()`.
- Keep `setk()` live-only. Config/NVS persistence remains owned by `runtime_config` and future console/config code.
- Expose `set_position()`, `get_position()`, `set_offset()`, and `setk()`.
- Use `PID_control` as the user-owned PID enable flag. PID and helper APIs read it but never change it.
- Add `RUNTIME_CONFIG_MAX_SPEED_COUNTS_PER_SEC` with a default of 20000 counts/sec for linear speed-to-PWM scaling.
- Treat `target_speed` and PID output as signed encoder counts/sec, not raw PWM.
- Convert speed to PWM linearly at the motor-output boundary with `pwm = abs(speed_counts_per_sec) * max_pwm / max_speed_counts_per_sec`.
- When `PID_control` is false, pass signed `target_speed` through the speed-to-PWM mapper instead of running PID.
- When `PID_control` is true, PID computes signed speed in counts/sec and only the final helper maps speed to PWM.
- Map positive position error to `AUTODOOR_OPEN_DIR_LEVEL` and negative error to `AUTODOOR_CLOSE_DIR_LEVEL`.
- PID loop uses `set_motor()` only. `stop_motor()` remains for direct user/safety control, not normal PID control.
- PID currently runs every `AUTODOOR_CONTROL_PERIOD_MS`, which is compile-time configured to 10 ms / 100 Hz in `config.h`.

Validation:

- `git diff --check` passes.
- No stale speed-control field references remain.
- `motor_pid_task.c` reads `PID_control` but does not write it.
- `motor_pid_task.c` does not call `stop_motor()`.
- PID math is in signed encoder counts/sec and PWM conversion is isolated to the final motor-output helper.
- `idf.py build` could not run in the current shell because `idf.py` is not on `PATH`.

## Known Repository State

- Local commit `41fd813` exists but push failed because GitHub rejected write access to `git@github.com:Antropi-Robotics/autodoor.git`.
- Local commit `e51ccaf` contains the simplified checkpoint 3 runtime config work.
- Local commit `3c83043` contains the combined checkpoint 4/5 hardware controller.
- Local commit `0ba14de` contains direct `door_motor` access under `motor_mutex`.
- Build-generated `autodoor/sdkconfig` churn was already included in `41fd813`.
