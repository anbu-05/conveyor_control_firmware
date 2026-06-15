# Conveyor Controller State Machine

The conveyor job controller lives in `main/conveyor/conveyor_job.c`.

It is the central owner of high-level conveyor behavior. MQTT and serial do
not directly implement TX/RX logic. They only submit commands to the conveyor
job queue.

## Physical Inputs

The conveyor has two BGS tray sensors:

```text
S0 = entry sensor
S1 = exit sensor
```

The sensors are active low:

```text
GPIO 0 = tray detected
GPIO 1 = no tray
```

The physical distance between `S0` and `S1` is smaller than the tray length.
Because of that, a tray on this conveyor should always trigger `S0`, `S1`, or
both. There is no valid physical state where a tray is on the conveyor and
both sensors are clear.

## Sensor Reference

```text
tray movement = S0 toward S1
```

The same two physical sensors are reused as logical TX/RX sensors:

```text
tx0 = S0
tx1 = S1
rx0 = S0
rx1 = S1
```

Derived tray presence:

```text
has_tray = S0 detected OR S1 detected
```

This derived status is owned by the conveyor job layer so both MQTT and serial
debug commands read the same interpretation.

## Commands

The state machine receives `conveyor_cmd_t` commands through a FreeRTOS queue.

Supported command types:

```text
CONVEYOR_CMD_START_TX
CONVEYOR_CMD_START_RX
CONVEYOR_CMD_EMERGENCY_STOP
CONVEYOR_CMD_CLEAR_ERROR
```

Only one TX/RX job can run at a time. If the conveyor is not `IDLE`, a new TX
or RX command is rejected as busy.

The state machine tracks how long the conveyor has been in the current state.
This timer resets whenever a new state is entered. MQTT feedback publishes it
as `state_elapsed_ms`.

## States

```text
IDLE
TX_WAIT_FOR_TX1_DETECT
TX_WAIT_FOR_TX1_CLEAR
RX_WAIT_FOR_RX0
RX_WAIT_FOR_RX1
TX_DONE
RX_DONE
ERROR
ESTOP
```

## TX Behavior

TX means this conveyor is transmitting a tray to another conveyor.

Start condition:

- State must be `IDLE`.
- Command must be `CONVEYOR_CMD_START_TX`.
- `has_tray` must be true.

If TX is requested with no tray present, the command is rejected:

```text
ERR NO_TRAY
```

Flow:

1. The motor starts immediately using runtime `run_speed_counts_per_sec`.
2. The state machine checks `tx1` / `S1`.
3. If `tx1` is already detecting the tray, state becomes
   `TX_WAIT_FOR_TX1_CLEAR`.
4. If `tx1` is clear, state becomes `TX_WAIT_FOR_TX1_DETECT`.
5. In `TX_WAIT_FOR_TX1_DETECT`, the conveyor keeps moving until `tx1`
   detects the tray.
6. After `tx1` detects, state becomes `TX_WAIT_FOR_TX1_CLEAR`.
7. In `TX_WAIT_FOR_TX1_CLEAR`, the conveyor keeps moving until `tx1` becomes
   clear again.
8. When `tx1` clears, the motor stops and state becomes `TX_DONE`.
9. After a short hold, `TX_DONE` automatically returns to `IDLE`.

The key TX rule is:

```text
Stop only when tx1 becomes clear after tx1 has detected the tray during this job.
```

This prevents early stopping when the tray starts near `tx0` / `S0` and has
not yet reached `tx1` / `S1`.

## RX Behavior

RX means this conveyor is receiving a tray from another conveyor.

Start condition:

- State must be `IDLE`.
- Command must be `CONVEYOR_CMD_START_RX`.
- `has_tray` must be false.

If RX is requested while a tray is already present, the command is rejected:

```text
ERR TRAY_PRESENT
```

Flow:

1. The motor stays stopped.
2. State becomes `RX_WAIT_FOR_RX0`.
3. In `RX_WAIT_FOR_RX0`, the conveyor waits until `rx0` / `S0` detects the tray.
4. When `rx0` detects, the motor starts.
5. State becomes `RX_WAIT_FOR_RX1`.
6. In `RX_WAIT_FOR_RX1`, the conveyor keeps moving until `rx1` / `S1` detects the
   tray.
7. When `rx1` detects, the motor stops immediately.
8. State becomes `RX_DONE`.
9. After a short hold, `RX_DONE` automatically returns to `IDLE`.

The key RX rule is:

```text
Stop immediately when rx1 detects the tray.
```

There is no extra encoder movement after `rx1` in the current version.

## Emergency Stop

Emergency stop can come from:

- Serial `stop`
- Serial `estop`
- MQTT `{"type":"emergency_stop"}`
- MQTT emergency topic payload `STOP`

Effect:

- Stops all motors.
- Stores error text `ESTOP`.
- Sets state to `ESTOP`.

The conveyor stays in `ESTOP` until `clearerror` or MQTT `clear_error` is
processed.

## Clear Error

Clear error is accepted in these states:

```text
ERROR
ESTOP
```

Effect:

- Clears the stored error text.
- Returns state to `IDLE`.

## Lost Tray Errors

The tray-length assumption lets the state machine detect some faults before a
normal timeout expires.

During TX:

- In `TX_WAIT_FOR_TX1_DETECT`, the conveyor expects the tray to stay visible
  on at least one sensor until `tx1` detects.
- If `has_tray` becomes false first, the motor stops and state becomes
  `ERROR`.
- Error text becomes `TX_LOST_TRAY`.

During RX:

- In `RX_WAIT_FOR_RX1`, the conveyor has already seen the tray at `rx0`.
- The tray should stay visible on at least one sensor until `rx1` detects.
- If `has_tray` becomes false first, the motor stops and state becomes
  `ERROR`.
- Error text becomes `RX_LOST_TRAY`.

`TX_WAIT_FOR_TX1_CLEAR` is different. In that state, `tx1` clearing is the
normal TX completion condition.

## Timeouts

Timeout defaults are defined in `main/config/config.h`, then loaded into
runtime config at boot. Serial `setconfig` can edit and save the runtime
timeout values in NVS.

```text
tx_detect_timeout_ms
tx_clear_timeout_ms
rx_detect_timeout_ms
rx_done_timeout_ms
```

These timeout values are not compile-time-only after boot. `config.h` gives
the default values, but the active values come from runtime config. Use
`getconfig` to see them and `setconfig` to change them while the conveyor is
`IDLE`.

Example:

```text
getconfig tx_detect_timeout_ms
setconfig tx_detect_timeout_ms 7000
```

### TX Detect Timeout

Config key:

```text
tx_detect_timeout_ms
```

Used in state:

```text
TX_WAIT_FOR_TX1_DETECT
```

TX means this conveyor is pushing a tray out to another conveyor. During TX,
the motor starts immediately. The state machine then waits for `tx1` / `S1` to detect
the tray.

The timer starts when the TX job enters `TX_WAIT_FOR_TX1_DETECT`.

This timeout means:

```text
The tray did not reach tx1 in time.
```

For every TX job, `tx1` is `S1`.

Common causes:

- The motor did not move.
- The fixed motor direction is wrong for this wiring.
- The tray is jammed before reaching `tx1`.
- The sensor mapping is wrong.
- The timeout value is too short for the conveyor speed.

On expiry:

- Motor stops.
- State becomes `ERROR`.
- Error text becomes `TX_DETECT_TIMEOUT`.

If both sensors clear before `tx1` detects, the state machine reports
`TX_LOST_TRAY` instead of waiting for this timeout.

### TX Clear Timeout

Config key:

```text
tx_clear_timeout_ms
```

Used in state:

```text
TX_WAIT_FOR_TX1_CLEAR
```

After `tx1` detects the tray, TX does not stop immediately. It keeps moving
until `tx1` becomes clear again. This confirms the tray has passed the handoff
sensor instead of just touching it.

The timer starts when the TX job enters `TX_WAIT_FOR_TX1_CLEAR`.

This can happen in two ways:

- `tx1` was already detecting when the TX job started.
- `tx1` became detected during `TX_WAIT_FOR_TX1_DETECT`.

This timeout means:

```text
The tray reached tx1, but tx1 did not clear in time.
```

For every TX job, `tx1` is `S1`.

Common causes:

- The tray is stuck at the handoff side.
- The receiving conveyor did not pull the tray away.
- The `tx1` / `S1` sensor is stuck in the detected state.
- The motor stopped mechanically even though PWM is still commanded.
- The timeout value is too short for the tray length and speed.

On expiry:

- Motor stops.
- State becomes `ERROR`.
- Error text becomes `TX_CLEAR_TIMEOUT`.

### RX Detect Timeout

Config key:

```text
rx_detect_timeout_ms
```

Used in state:

```text
RX_WAIT_FOR_RX0
```

RX means this conveyor is waiting to receive a tray from another conveyor. At
the start of RX, this conveyor does not move. It waits for `rx0` / `S0` to detect the
incoming tray.

The timer starts when the RX job enters `RX_WAIT_FOR_RX0`.

This timeout means:

```text
The incoming tray did not reach rx0 in time.
```

For every RX job, `rx0` is `S0`.

Common causes:

- The transmitting conveyor did not send a tray.
- The tray stopped before entering this conveyor.
- The `rx0` / `S0` sensor did not detect the tray.
- The timeout value is too short for the upstream handoff delay.

On expiry:

- Motor is commanded stopped.
- State becomes `ERROR`.
- Error text becomes `RX_DETECT_TIMEOUT`.

### RX Done Timeout

Config key:

```text
rx_done_timeout_ms
```

Used in state:

```text
RX_WAIT_FOR_RX1
```

After `rx0` detects the incoming tray, the RX motor starts. The conveyor then
moves the tray until `rx1` detects it. When `rx1` detects the tray, RX stops
immediately and goes to `RX_DONE`.

The timer starts when the RX job enters `RX_WAIT_FOR_RX1`.

This timeout means:

```text
The tray entered at rx0, but did not reach rx1 in time.
```

For every RX job, `rx1` is `S1`.

Common causes:

- The tray jammed between `rx0` and `rx1`.
- The motor did not move after `rx0` detected.
- The fixed motor direction is wrong for this wiring.
- The `rx1` / `S1` sensor did not detect the tray.
- The timeout value is too short for the conveyor length and speed.

On expiry:

- Motor stops.
- State becomes `ERROR`.
- Error text becomes `RX_DONE_TIMEOUT`.

If both sensors clear before `rx1` detects, the state machine reports
`RX_LOST_TRAY` instead of waiting for this timeout.

## Timeout Tuning

Set each timeout longer than the normal expected movement time, with some
margin.

If a timeout is too short:

- Good transfers may fail with false `ERROR` states.

If a timeout is too long:

- Real jams or missed trays take longer to report.

The TX clear timeout may need to be longer than the TX detect timeout if long
trays take more time to completely clear the handoff sensor.

## Motor Boundary

The state machine does not write GPIO or LEDC hardware directly.

It calls shared motor helpers:

```text
start_motor("M0")
stop_motor("M0")
stop_all_motors()
```

`start_motor("M0")` uses the fixed `CONVEYOR_MOTOR_FORWARD_DIRECTION` and the
runtime `run_speed_counts_per_sec` value. The state machine only requests
run/stop for `M0`. Normal TX/RX completion uses `stop_motor("M0")` so the
motor PID task ramps target speed to zero. Errors and emergency stops still
use `stop_all_motors()` for an immediate stop.

The `motor_pid_task` reads encoder PCNT, calculates speed, updates the simple
speed controller, and writes the actual BTS7960 enable plus RPWM/LPWM LEDC
hardware. Direction `1` drives RPWM, direction `0` drives LPWM, and both
enable pins are low when PWM is zero.
The speed controller estimates a base PWM from the target speed, trims it with
P/D correction, and slews the actual PWM toward that request.

## Status Output

Every state change prints a serial job event:

```text
EVENT JOB C0 TX_WAIT_FOR_TX1_DETECT
EVENT JOB C0 TX_WAIT_FOR_TX1_CLEAR
EVENT JOB C0 TX_DONE
EVENT JOB C0 IDLE
```

The same state is also available through MQTT feedback when MQTT is connected.
