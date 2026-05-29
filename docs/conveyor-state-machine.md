# Conveyor Controller State Machine

The conveyor job controller lives in `main/conveyor/conveyor_job.c`.

It is the central owner of high-level conveyor behavior. MQTT and serial do
not directly implement TX/RX logic. They only submit commands to the conveyor
job queue.

## Physical Inputs

The conveyor has two BGS tray sensors:

```text
S0 = left sensor
S1 = right sensor
```

The sensors are active low:

```text
GPIO 0 = tray detected
GPIO 1 = no tray
```

## Direction Reference

```text
right = move from S0 toward S1
left  = move from S1 toward S0
```

The same two physical sensors are reused as logical TX/RX sensors depending on
the job direction.

For direction `right`:

```text
tx_1 = S0
tx_2 = S1
rx_1 = S0
rx_2 = S1
```

For direction `left`:

```text
tx_1 = S1
tx_2 = S0
rx_1 = S1
rx_2 = S0
```

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

## States

```text
IDLE
TX_WAIT_FOR_TX2_DETECT
TX_WAIT_FOR_TX2_CLEAR
RX_WAIT_FOR_RX1
RX_WAIT_FOR_RX2
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
- Direction must be `left` or `right`.

Flow:

1. The state machine sets the job direction.
2. The motor starts immediately in that direction using runtime `run_pwm`.
3. The state machine checks `tx_2`.
4. If `tx_2` is already detecting the tray, state becomes
   `TX_WAIT_FOR_TX2_CLEAR`.
5. If `tx_2` is clear, state becomes `TX_WAIT_FOR_TX2_DETECT`.
6. In `TX_WAIT_FOR_TX2_DETECT`, the conveyor keeps moving until `tx_2`
   detects the tray.
7. After `tx_2` detects, state becomes `TX_WAIT_FOR_TX2_CLEAR`.
8. In `TX_WAIT_FOR_TX2_CLEAR`, the conveyor keeps moving until `tx_2` becomes
   clear again.
9. When `tx_2` clears, the motor stops and state becomes `TX_DONE`.
10. After a short hold, `TX_DONE` automatically returns to `IDLE`.

The key TX rule is:

```text
Stop only when tx_2 becomes clear after tx_2 has detected the tray during this job.
```

This prevents early stopping when the tray starts near `tx_1` and has not yet
reached `tx_2`.

## RX Behavior

RX means this conveyor is receiving a tray from another conveyor.

Start condition:

- State must be `IDLE`.
- Command must be `CONVEYOR_CMD_START_RX`.
- Direction must be `left` or `right`.

Flow:

1. The state machine sets the job direction.
2. The motor stays stopped.
3. State becomes `RX_WAIT_FOR_RX1`.
4. In `RX_WAIT_FOR_RX1`, the conveyor waits until `rx_1` detects the tray.
5. When `rx_1` detects, the motor starts in the receive direction.
6. State becomes `RX_WAIT_FOR_RX2`.
7. In `RX_WAIT_FOR_RX2`, the conveyor keeps moving until `rx_2` detects the
   tray.
8. When `rx_2` detects, the motor stops immediately.
9. State becomes `RX_DONE`.
10. After a short hold, `RX_DONE` automatically returns to `IDLE`.

The key RX rule is:

```text
Stop immediately when rx_2 detects the tray.
```

There is no extra encoder movement after `rx_2` in the current version.

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
TX_WAIT_FOR_TX2_DETECT
```

TX means this conveyor is pushing a tray out to another conveyor. During TX,
the motor starts immediately. The state machine then waits for `tx_2` to detect
the tray.

The timer starts when the TX job enters `TX_WAIT_FOR_TX2_DETECT`.

This timeout means:

```text
The tray did not reach tx_2 in time.
```

For a `right` TX job, `tx_2` is `S1`.

For a `left` TX job, `tx_2` is `S0`.

Common causes:

- There was no tray on the conveyor.
- The motor did not move.
- The motor direction is wrong.
- The tray is jammed before reaching `tx_2`.
- The sensor mapping is wrong.
- The timeout value is too short for the conveyor speed.

On expiry:

- Motor stops.
- State becomes `ERROR`.
- Error text becomes `TX_DETECT_TIMEOUT`.

### TX Clear Timeout

Config key:

```text
tx_clear_timeout_ms
```

Used in state:

```text
TX_WAIT_FOR_TX2_CLEAR
```

After `tx_2` detects the tray, TX does not stop immediately. It keeps moving
until `tx_2` becomes clear again. This confirms the tray has passed the handoff
sensor instead of just touching it.

The timer starts when the TX job enters `TX_WAIT_FOR_TX2_CLEAR`.

This can happen in two ways:

- `tx_2` was already detecting when the TX job started.
- `tx_2` became detected during `TX_WAIT_FOR_TX2_DETECT`.

This timeout means:

```text
The tray reached tx_2, but tx_2 did not clear in time.
```

For a `right` TX job, `tx_2` is `S1`.

For a `left` TX job, `tx_2` is `S0`.

Common causes:

- The tray is stuck at the handoff side.
- The receiving conveyor did not pull the tray away.
- The `tx_2` sensor is stuck in the detected state.
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
RX_WAIT_FOR_RX1
```

RX means this conveyor is waiting to receive a tray from another conveyor. At
the start of RX, this conveyor does not move. It waits for `rx_1` to detect the
incoming tray.

The timer starts when the RX job enters `RX_WAIT_FOR_RX1`.

This timeout means:

```text
The incoming tray did not reach rx_1 in time.
```

For a `right` RX job, `rx_1` is `S0`.

For a `left` RX job, `rx_1` is `S1`.

Common causes:

- The transmitting conveyor did not send a tray.
- The wrong RX direction was selected.
- The tray stopped before entering this conveyor.
- The `rx_1` sensor did not detect the tray.
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
RX_WAIT_FOR_RX2
```

After `rx_1` detects the incoming tray, the RX motor starts. The conveyor then
moves the tray until `rx_2` detects it. When `rx_2` detects the tray, RX stops
immediately and goes to `RX_DONE`.

The timer starts when the RX job enters `RX_WAIT_FOR_RX2`.

This timeout means:

```text
The tray entered at rx_1, but did not reach rx_2 in time.
```

For a `right` RX job, `rx_2` is `S1`.

For a `left` RX job, `rx_2` is `S0`.

Common causes:

- The tray jammed between `rx_1` and `rx_2`.
- The motor did not move after `rx_1` detected.
- The motor direction is wrong.
- The `rx_2` sensor did not detect the tray.
- The timeout value is too short for the conveyor length and speed.

On expiry:

- Motor stops.
- State becomes `ERROR`.
- Error text becomes `RX_DONE_TIMEOUT`.

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
move_main_motor(direction, pwm)
stop_all_motors()
```

The `motor_controller_task` is still the only task that writes the actual
direction GPIO and LEDC PWM hardware.

## Status Output

Every state change prints a serial job event:

```text
EVENT JOB C0 TX_WAIT_FOR_TX2_DETECT right
EVENT JOB C0 TX_WAIT_FOR_TX2_CLEAR right
EVENT JOB C0 TX_DONE right
EVENT JOB C0 IDLE right
```

The same state is also available through MQTT feedback when MQTT is connected.
