# Speed Controller Implementation Plan

## Goal
Add a selectable PID controller mode so the same PID task can control either position or speed. Add `motor_t.speed` and `motor_t.target_speed`, publish speed from `hardware_task()`, rename the PID-enable flag to `motor_t.PID_control`, and expose minimal console/API support with `setspeed` and `getspeed` for commissioning.

## Decisions
- Rename the old PID-enable field to `motor_t.PID_control` everywhere because the flag means “PID owns motor output” rather than “position mode”.
- Keep `motor_t.PID_control` as the PID-enable/manual gate, because console raw commands and the state machine rely on it to stop PID from overwriting raw `set_motor()` output.
- Add a separate PID mode enum for position vs speed, rather than overloading `PID_control`.
- Change `motor_pid_init()` to accept a mode argument and store it in `motor_t`.
- Speed units should be encoder counts per second, computed from position deltas over `APP_MOTOR_CONTROL_PERIOD_MS`.
- Prefer deriving speed from the same offset-corrected `current_position` value for uniformity. If the implementation is simpler at the exact edit site, computing directly from the raw position value before/after offset is acceptable because a constant offset cancels out in the delta.
- Add `setspeed <motor_id> <speed>` and `getspeed <motor_id>` console commands, backed by new minimal PID API functions.
- Preserve the project preference for simple rigid communication: no fallback parsing, retries, or malformed-data recovery.
- When editing code, add comments near changed blocks explaining the reason for the change, not obvious line-by-line descriptions.

## Implementation Tasks
1. Update shared motor state in `main/shared/app_state.h` and `main/shared/app_state.c`.
   - Rename the old PID-enable struct field to `PID_control`.
   - Update all initializers and references to use `.PID_control`.
   - Add `int speed;` and `int target_speed;` near the position fields because they are encoder-derived control inputs/targets.
   - Add a PID mode field, using a small enum such as `MOTOR_PID_MODE_POSITION` and `MOTOR_PID_MODE_SPEED`.
   - Initialize the default motor with `speed = 0`, `target_speed = 0`, `PID_control = true`, and default mode `MOTOR_PID_MODE_POSITION` unless `main.c` is intentionally switched to speed mode for bring-up.

2. Update `hardware_task()` in `main/tasks/hardware.c`.
   - Track previous position per motor in a static local array inside `hardware_task()` to avoid adding extra persistent fields to `motor_t` unless needed.
   - After reading PCNT and computing the latest position, compute speed as `(position - previous_position) * 1000 / APP_MOTOR_CONTROL_PERIOD_MS`.
   - Prefer using the same offset-corrected `current_position` value used by the rest of the application; direct raw-count delta is acceptable if it keeps the block simpler because offset does not affect speed delta.
   - Publish `motors[i].speed` under `motor_mutex` in the same protected block as `current_position`.
   - Initialize previous-position tracking on the first valid sample so startup does not report a large artificial speed spike.

3. Update PID public API in `main/tasks/pid.h`.
   - Change `motor_pid_init(const char *motor_id)` to `motor_pid_init(const char *motor_id, motor_pid_mode_t mode)` or use the enum from `app_state.h` if defined there.
   - Add only the needed public speed functions: `set_speed(const char *motor_id, int target_speed)` and `get_speed(const char *motor_id, int *out_speed)`.
   - Keep the header minimal; do not expose internal snapshot or helper functions.

4. Update PID implementation in `main/tasks/pid.c`.
   - Rename all old PID-enable references to `PID_control`.
   - Extend `pid_snapshot_t` with `speed`, `target_speed`, `PID_control`, and PID mode.
   - Update `read_pid_snapshot()` to copy those fields under the existing mutex.
   - Update `motor_pid_init()` to validate/store the requested mode and reset PID memory.
   - Add `set_speed()` mirroring `set_position()` semantics: validate motor id, require PID control enabled, publish `target_speed`, reset PID history if needed to avoid stale position-mode integral/derivative state affecting speed mode.
   - Add `get_speed()` mirroring `get_position()` and returning the hardware-published speed snapshot.
   - In `motor_pid_task()`, compute raw error from either `target_position - current_position` or `target_speed - speed` based on mode.
   - Apply `RUNTIME_CONFIG_POSITION_TOLERANCE_COUNTS` only in position mode; speed mode should initially use no deadband unless a speed tolerance config is explicitly added later.
   - Keep the rest of the PID math and signed PWM output path unchanged.
   - When PID is disabled via `PID_control == false`, continue clearing PID memory and not writing motor output.

5. Update startup in `main/main.c`.
   - Change the existing `motor_pid_init(motors[i].id)` call to pass the intended startup mode.
   - Recommended default for safety/backward compatibility is `MOTOR_PID_MODE_POSITION`, preserving current behavior until a later explicit switch to speed mode is requested.

6. Update console in `main/tasks/console.c`.
   - Rename internal variables and helper comments from position-control wording to PID-control wording where they refer to the ownership flag.
   - Add command IDs and command table entries for `setspeed` and `getspeed`.
   - Implement `setspeed <motor_id> <speed>` by parsing the target speed and calling `set_speed()`.
   - Implement `getspeed <motor_id>` by calling `get_speed()` and printing a stable response such as `OK SPEED motor=%s speed=%d`.
   - Keep existing `setposition` and `getposition` behavior intact while using `pid_control` for PID ownership.
   - Rename the user-facing PID ownership command to `pid_control` so it matches the backing field semantics.

7. Update raw/manual paths.
   - In the console helper that enables/disables PID ownership and in `start_moving_upstream()`, write `PID_control = false`.
   - Reset `target_speed` to `0` when disabling PID ownership so speed-mode state does not restart with stale intent.
   - Do not change `set_motor()` or `stop_motor()` APIs.

## Validation
1. Build the firmware with the project’s normal ESP-IDF build command.
2. Confirm there are no remaining old PID-enable field or command references.
3. Confirm existing position commands still compile and retain their current output tokens.
4. Use console commissioning flow after flashing:
   - `getspeed M0` should report the current measured counts/sec.
   - `setspeed M0 <value>` should set `target_speed` and allow PID to drive toward that speed when the PID mode is speed.
   - `setmotor M0 <pwm> <dir>` should still disable PID ownership and apply raw output.
   - `stopmotor M0` and `stop` should still cut PWM and disable PID ownership.
5. Observe that speed sign follows encoder position delta sign. If the sign is reversed for the physical conveyor direction, correct the encoder action configuration or invert the speed computation in one explicit place after confirming hardware behavior.

## Risks
- Speed PID gains will likely need different tuning from position PID gains. Reusing `kp`, `ki`, and `kd` is mechanically simple but may not be operationally safe without retuning.
- Integer counts/sec at low encoder rates may be noisy. Do not add filtering in this iteration unless requested; keep the implementation direct and testable first.
- Renaming the user-facing command to `pid_control` breaks old commissioning scripts but keeps command naming aligned with the firmware field semantics.
