# BTS7960 Current Sensing And Motor Monitor Plan

## Context
- Repo target is ESP32-S3 ESP-IDF firmware for one conveyor motor `M0` using an IBT-2/BTS7960 driver.
- Existing BTS7960 control pins are `RPWM=GPIO15`, `LPWM=GPIO16`, `REN=GPIO6`, `LEN=GPIO7`.
- Existing encoder pins are `GPIO17/GPIO18`; `motor_pid_task` already updates `motor_t.position` and `motor_t.current_speed` every 20 ms and prints `EVENT ENCODER`/`EVENT PID` watch output.
- User wiring for current sense:
  - `LIS` is connected to ESP32-S3 `GPIO8`.
  - `RIS` is connected to ESP32-S3 `GPIO9`.
  - RIS/LIS are 5 V-side signals divided down with `2.2 kOhm` over `4.7 kOhm`, so reconstruct BTS7960 pin voltage with `(2200 + 4700) / 4700 = 1.468085`.
- Current conversion requested by user:
  - `k_ilis` nominal default is `8500` and must be runtime-editable.
  - IBT-2 `R_IS` is assumed to be `1000 ohm`.
  - `I_load = (V_is / R_is) * k_ilis`.
  - In integer millivolts/milliamps: `current_mA = bts_is_mV * k_ilis / R_is_ohms`.
- Plan mode did not create the branch or edit source files. Implementation agent should create the branch first.

## Branch
1. Inspect status before branching: `git status --short`.
2. Create a new branch from the current HEAD, for example: `git switch -c bts7960-current-monitor`.
3. Do not revert unrelated dirty worktree changes.

## Implementation Tasks
1. Add ADC/current-sense configuration in `main/config/config.h`:
   - `MOTOR_LIS_GPIO GPIO_NUM_8`.
   - `MOTOR_RIS_GPIO GPIO_NUM_9`.
   - ADC channel definitions for ESP32-S3 ADC1: `GPIO8 -> ADC_CHANNEL_7`, `GPIO9 -> ADC_CHANNEL_8`.
   - `BTS7960_CURRENT_DIVIDER_TOP_OHMS 2200`.
   - `BTS7960_CURRENT_DIVIDER_BOTTOM_OHMS 4700`.
   - `BTS7960_CURRENT_R_IS_OHMS 1000`.
   - `BTS7960_CURRENT_K_ILIS 8500`.
   - Current task defaults such as stack size, priority, sample period, and sample count.

2. Add runtime-editable `k_ilis` consistently with existing runtime config:
   - Add field `bts7960_k_ilis` or `k_ilis` to `runtime_config_t` in `main/config/runtime_config.c`.
   - Default from `BTS7960_CURRENT_K_ILIS`.
   - Add validation range. Recommended practical range: `1000..30000` to allow calibration without accepting nonsensical values.
   - Add NVS storage key, load, save, reset, `getconfig`, `setconfig`, and `runtime_config_print_all()` support.
   - Add accessor to `main/config/runtime_config.h`, e.g. `int runtime_config_bts7960_k_ilis(void);`.
   - Prefer command key `k_ilis` for the user-facing `getconfig k_ilis` / `setconfig k_ilis 8500` interface.

3. Extend shared motor state in `main/shared/app_state.h` and `main/shared/app_state.c`:
   - Add current sense pins/channels to `motor_t`: LIS/RIS GPIOs and ADC channels.
   - Add sampled current state fields, protected by `motor_mutex`:
     - `lis_adc_mv`, `ris_adc_mv`: calibrated ADC pin millivolts after attenuation/calibration.
     - `lis_bts_mv`, `ris_bts_mv`: reconstructed BTS7960 IS pin millivolts after divider compensation.
     - `lis_current_mA`, `ris_current_mA`.
     - `current_mA`: selected motor current draw; use the active direction side when PWM is nonzero, otherwise max of both sides.
     - Optional `current_sample_ok` boolean or error counter if ADC reads fail.
   - Initialize M0 with LIS GPIO8/RIS GPIO9 and ADC1 channel 7/8.
   - Add extern/prototype for current-sense task and setup functions.

4. Add current-sense task, preferably a new `main/tasks/current_sense_task.c`:
   - Use ESP-IDF oneshot ADC APIs: include `esp_adc/adc_oneshot.h` and `esp_adc/adc_cali.h` / line fitting or curve fitting depending on ESP-IDF availability.
   - Create one ADC1 oneshot unit.
   - Configure LIS/RIS channels with 12-bit width and an attenuation suitable for up to about 3.41 V at the ADC pin. Use `ADC_ATTEN_DB_12` on ESP32-S3 unless the project’s ESP-IDF version recommends the newer equivalent.
   - Calibrate to millivolts when calibration is available; fall back to raw-to-mV approximation only if calibration setup fails, and keep code simple.
   - Sample both channels periodically. Recommended default: every `20 ms` or `50 ms`; use a small average such as 4 samples per channel to reduce noise.
   - Convert:
     - `bts_mv = adc_mv * (2200 + 4700) / 4700`.
     - `current_mA = bts_mv * runtime_config_bts7960_k_ilis() / 1000`.
   - Determine `motor->current_mA` based on direction/PWM snapshot:
     - If `pwm > 0 && direction == 1`, use `ris_current_mA`.
     - If `pwm > 0 && direction == 0`, use `lis_current_mA`.
     - If stopped, use `max(ris_current_mA, lis_current_mA)` so leakage/fault readings are still visible.
   - Update all current fields under `motor_mutex`.
   - Do not print continuously from this task by default; serial output should be controlled by microrl watch/monitor commands.

5. Wire the new task into build/startup:
   - Add `tasks/current_sense_task.c` to `main/CMakeLists.txt`.
   - Add `esp_adc` / `esp_driver_adc` dependency as required by the ESP-IDF version. Existing CMake currently has no ADC dependency.
   - In `app_main`, call an ADC/current setup function if separated from the task.
   - Start `current_sense_task` after `configure_runtime_config()` and before or near `motor_pid_task`; pass `&motors[0]` if task is per-motor.

6. Add microrl commands in `main/tasks/command_task.c`:
   - Add `getcurrent M0` one-shot command.
   - Output recommended machine-readable format:
     - `CURRENT M0 <current_mA> <ris_current_mA> <lis_current_mA> <ris_bts_mv> <lis_bts_mv> <ris_adc_mv> <lis_adc_mv> <k_ilis>`.
   - Add a continuous monitor command for encoder position, speed, and motor current. Recommended name:
     - `watchmotor M0 on`
     - `watchmotor M0 off`
   - Output recommended format every existing watch period (`ENCODER_WATCH_DELAY_MS`, currently 100 ms):
     - `EVENT MOTOR M0 pos=<position> speed=<current_speed> current_mA=<current_mA> ris_mA=<ris_current_mA> lis_mA=<lis_current_mA> pwm=<pwm> dir=<direction>`.
   - Implement watch state in `app_state`: `motor_watch_enabled` and `motor_watch_motor`, similar to `encoder_watch_enabled`/`pid_watch_enabled`.
   - Emit monitor output from `motor_pid_task` at the same 100 ms watch cadence, because it already has stable timing and fresh position/speed. Read current fields under `motor_mutex` into locals before printing.
   - Keep existing `watchencoder` unchanged for backward compatibility.

7. Optional but useful: extend `getmotor M0` to include current only if it does not break consumers. Safer default is to leave `getmotor` unchanged and add `getcurrent`/`watchmotor` as new commands.

8. Update docs/status files:
   - `docs/serial-debug-commands.md`: document `getcurrent`, `watchmotor`, `getconfig k_ilis`, `setconfig k_ilis 8500`, output fields, and divider/current formulas.
   - `README.md`: update hardware wiring with LIS GPIO8, RIS GPIO9, resistor divider, shared ground, and warning to verify ADC pin voltage stays within ESP32-S3 limits.
   - `project.md`: update current state, motor struct fields, command list, and architecture notes.

## Risks And Notes
- ESP32-S3 GPIO8/GPIO9 are ADC1-capable. Implementation must verify the board variant does not use them for another onboard function.
- ADC input range must never exceed the selected attenuation range or absolute ESP32-S3 ADC pin voltage. The user divider targets about `3.41 V` from a `5 V` IS signal, which is near the top of typical ADC use; calibration/attenuation matters.
- BTS7960 IS outputs may show diagnosis/fault behavior and may not be perfectly linear at very low current. Keep raw/scaled voltages visible in `getcurrent` for calibration/debugging.
- Integer math should use `int64_t` for conversion intermediates to avoid overflow when multiplying millivolts by `k_ilis`.
- Serial output should remain machine-readable and line-oriented with `\r\n`, matching current command style.

## Validation
1. Build: `idf.py build`.
2. Flash/monitor when hardware is available.
3. Verify startup still prints `READY conveyor`.
4. Run `getconfig k_ilis`; expect `CONFIG k_ilis 8500` by default.
5. Run `setconfig k_ilis 8500`; expect `OK SETCONFIG k_ilis 8500`.
6. Run `getcurrent M0`; verify ADC mV and reconstructed BTS mV are plausible with motor stopped.
7. Run `watchmotor M0 on`; verify lines include position, speed, and current about every 100 ms.
8. Run motor in both directions using `setmotor M0 <pwm> <dir>` or `setspeed M0 <speed>`; verify active side switches between RIS for direction `1` and LIS for direction `0`.
9. Compare `current_mA` against an external meter and tune `k_ilis` with `setconfig k_ilis <value>` if needed.
10. Run `watchmotor M0 off`; verify continuous monitor output stops.
