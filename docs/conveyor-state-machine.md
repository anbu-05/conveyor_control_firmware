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
2. The motor starts immediately in that direction using `CONVEYOR_RUN_PWM`.
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

Timeout constants are defined in `main/config/config.h`.

```text
CONVEYOR_TIMEOUT_TX_DETECT_MS
CONVEYOR_TIMEOUT_TX_CLEAR_MS
CONVEYOR_TIMEOUT_RX_DETECT_MS
CONVEYOR_TIMEOUT_RX_DONE_MS
```

Timeout behavior:

- TX detect timeout: motor stops, state becomes `ERROR`, error is
  `TX_DETECT_TIMEOUT`.
- TX clear timeout: motor stops, state becomes `ERROR`, error is
  `TX_CLEAR_TIMEOUT`.
- RX detect timeout: motor stops, state becomes `ERROR`, error is
  `RX_DETECT_TIMEOUT`.
- RX done timeout: motor stops, state becomes `ERROR`, error is
  `RX_DONE_TIMEOUT`.

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
