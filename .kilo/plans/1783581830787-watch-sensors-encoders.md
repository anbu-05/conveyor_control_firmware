# Watch Sensors And Encoders Plan

## Goal
Add two ESP console commands:

- `watchsensors <motor_id> <0|1>`: enable/disable monitoring both sensor fields for the motor. When either sensor changes, print one line: `<sensor_name> <current value> <before change> <after change>`, e.g. `positive_sensor 0 1 0`.
- `watchencoders <motor_id> <0|1>`: enable/disable encoder monitoring for the motor. While enabled, print the current encoder count every compile-time-configured interval, default 100 ms.

## Confirmed Semantics
- The final `0|1` argument is enable/disable, not sensor selection.
- `watchsensors` monitors both `positive_sensor` and `negative_sensor` for the selected motor.
- Keep changes concise and avoid adding broad helper APIs.

## Affected Files
- `main/config/config.h`
- `main/tasks/console.c`

## Implementation Steps
1. Add one compile-time config in `main/config/config.h` near other periods:
   - `#define APP_MOTOR_ENCODER_WATCH_PERIOD_MS 100`
   - Optionally use the same period for polling sensor watch state to avoid another config unless the implementation needs a separate internal constant.
2. In `main/tasks/console.c`, extend `console_command_id_t` and `s_commands` with:
   - `watchsensors`, help: `Watch sensor changes: watchsensors <motor_id> <0|1>`
   - `watchencoders`, help: `Watch encoder count: watchencoders <motor_id> <0|1>`
3. Add small static watch state in `console.c`, local to the file:
   - One `motor_t *` or motor index for sensor watch and one for encoder watch.
   - Boolean enabled flags.
   - Previous positive/negative sensor values for change detection.
   - Previous/next tick value for encoder printing if needed.
   - Protect this watch state with a small mutex or reuse `motor_mutex` carefully; prefer a separate static `SemaphoreHandle_t s_console_watch_mutex` so command toggles do not hold `motor_mutex` while printing.
4. Add command handling in `handle_console_command()`:
   - Require exactly 3 args.
   - Parse arg 2 with existing `parse_int_arg()` and accept only `0` or `1`; otherwise print `ERR BAD_ARGS`.
   - Resolve `argv[1]` against `motors[i].id`; if missing, print `ERR ESP_ERR_NOT_FOUND` or use the existing style by returning `ERR <esp_err_to_name(ESP_ERR_NOT_FOUND)>`.
   - For enable:
     - Snapshot current sensor values under `motor_mutex` and store as previous values so enabling does not immediately print synthetic changes.
     - Store selected motor index and set the enabled flag.
     - Print `OK WATCHSENSORS motor=<id> enabled=1` or `OK WATCHENCODERS motor=<id> enabled=1`.
   - For disable:
     - Clear only that watch flag if it points at the requested motor, or clear unconditionally for this single-watch implementation.
     - Print `OK WATCHSENSORS motor=<id> enabled=0` or `OK WATCHENCODERS motor=<id> enabled=0`.
5. Add one small background task in `console.c`, e.g. `static void console_watch_task(void *arg)`, because `linenoise()` blocks and the console input loop cannot reliably print every 100 ms.
   - Start it from `console_init()` after successful command registration via `xTaskCreate()`.
   - Keep stack/priority local constants in `console.c`, for example stack 3072 and priority equal or lower than console input priority if available locally; do not edit `main.c` unless necessary.
   - Each loop:
     - Copy watch enabled flags and selected indices under `s_console_watch_mutex`.
     - Snapshot relevant motor fields under `motor_mutex`.
     - For sensor watch, compare current positive/negative values with stored previous values. For each changed field print exactly `<sensor_name> <current value> <before change> <after change>` and update previous values. Given the requested example, `current value` is the sampled current sensor value and should match `after change` on a detected transition.
     - For encoder watch, print the selected motor's `encoder_count` every `APP_MOTOR_ENCODER_WATCH_PERIOD_MS`. Use a concise line such as `encoder_count <count>` unless the implementation owner chooses to include the motor id for consistency; do not add extra formatting to sensor lines.
     - Call `fflush(stdout)` after watch prints to keep serial output timely.
     - Delay with `vTaskDelay(pdMS_TO_TICKS(APP_MOTOR_ENCODER_WATCH_PERIOD_MS))`.
6. Initialize `s_console_watch_mutex` in `console_init()` before starting the watch task. If allocation fails, return `ESP_ERR_NO_MEM`.

## Edge Cases
- Invalid arg count or non-integer enable flag: `ERR BAD_ARGS`.
- Enable flag outside `0|1`: `ERR BAD_ARGS`.
- Unknown motor id: existing console error style using `esp_err_to_name(ESP_ERR_NOT_FOUND)`.
- Disabling when not enabled should still return OK and leave watch disabled.
- Switching from one motor to another should replace the watched motor and reset previous sensor values from the new motor snapshot.
- Watch output may interleave with the `linenoise` prompt; acceptable for a diagnostics watch command unless a later task requires prompt redraw handling.

## Validation
1. Build the firmware, e.g. `idf.py build` in the repo root.
2. On device, run `status` and verify both new commands appear.
3. Run `watchsensors M0 1`, toggle each physical sensor, and verify only changes print:
   - `positive_sensor <current> <before> <after>`
   - `negative_sensor <current> <before> <after>`
4. Run `watchsensors M0 0` and verify sensor toggles no longer print.
5. Run `watchencoders M0 1` and verify `encoder_count <count>` prints roughly every 100 ms.
6. Run `watchencoders M0 0` and verify encoder prints stop.
7. Validate bad inputs: missing args, bad enable value, unknown motor id.
