You are building a modular conveyor transfer system with 4 conveyor units. Each conveyor has its own ESP, motor, encoder, and two BGS tray sensors. The ESP controls only its own conveyor, and the server coordinates transfers through MQTT.

Each conveyor can act as either:

Transmitter, meaning it pushes a tray out to another conveyor.
Receiver, meaning it accepts a tray from another conveyor.
The same two physical sensors are reused in both roles. Their names change logically depending on the conveyor’s role and transfer direction.

Physical Setup

Each conveyor has two BGS sensors placed near the two ends, but not exactly at the physical edges. The sensors are offset inward.

The tray has a fixed length.

Two important geometry rules make the transfer possible:

On a single conveyor, the distance between the two sensors is greater than the tray length.
Between two facing conveyors, the distance from transmitter-side tx_2 to receiver-side rx_1 is less than the tray length.
That second rule is important. It means that by the time the tray leaves tx_2, the tray should already have reached rx_1 on the receiver.

Sensor Electrical Behavior

The BGS sensors are active low:

Tray present    -> GPIO low
No tray present -> GPIO high / 3.3V
So in firmware, a detected tray means the sensor input reads 0.

Logical Sensor Names

For a receiver:

rx_1 is the sensor on the incoming side.
rx_2 is the other sensor, farther along the receiving direction.
For a transmitter:

tx_2 is the sensor closest to the receiver’s rx_1.
tx_1 is the sensor farther away from the receiver.
Because the conveyor can move in both directions, the firmware cannot permanently label one physical sensor as rx_1 or tx_2. It needs to map the two physical sensors based on the current transfer direction.

Example:

Transfer left -> right:
receiver rx_1 = left sensor
receiver rx_2 = right sensor

transmitter tx_1 = left/back sensor
transmitter tx_2 = right/front sensor
For the opposite direction, that mapping reverses.

Receiver Behavior

When the server tells a conveyor to act as a receiver:

Receiver becomes armed and waits.
It does not move immediately.
When rx_1 detects the tray, receiver starts moving in the receive direction.
It keeps moving until rx_2 detects the tray.
As soon as rx_2 detects the tray, receiver stops immediately.
There is no extra encoder movement after rx_2. The rx_2 trigger itself is the stopping condition.

Transmitter Behavior

When the server tells a conveyor to act as a transmitter:

Transmitter starts moving in the transmit direction.
The tray may initially be detected by tx_2, or it may only be detected by tx_1.
If tx_2 is already detecting the tray, the transmitter is already in the final handoff phase.
If tx_2 is not detecting the tray yet, the transmitter keeps moving until tx_2 detects the tray.
After tx_2 has detected the tray at least once, the transmitter keeps moving.
When tx_2 becomes undetected again, the transmitter stops.
So the real TX stop rule is:

Stop when tx_2 becomes clear after tx_2 has detected the tray during this job.
That avoids stopping too early when the tray starts near tx_1 and has not reached tx_2 yet.

Transfer Sequence

For a complete handoff:

Server selects one conveyor as TX and one as RX.
Server sends both conveyors their job commands.
TX starts moving the tray toward RX.
RX waits until rx_1 detects the tray.
Because of the tray length and sensor spacing, RX should detect the tray before TX stops.
RX starts pulling the tray in.
TX stops after tx_2 clears.
RX stops when rx_2 detects.
Both conveyors report done or error back to the server later through MQTT.
Firmware Shape

The conveyor logic should be a state machine.

Likely TX states:

IDLE
TX_WAIT_FOR_TX2_DETECT
TX_WAIT_FOR_TX2_CLEAR
TX_DONE
TX_ERROR
Likely RX states:

IDLE
RX_WAIT_FOR_RX1
RX_WAIT_FOR_RX2
RX_DONE
RX_ERROR
The motor-control layer should stay separate. Since you plan to implement PID speed control, the tray-transfer logic should not directly think in PWM long-term. It should issue simple commands like:

move at velocity V in direction D
stop
Then the motor/PID layer handles the actual PWM and encoder feedback.

Timeouts

Timeouts should be configurable. Useful timeout points are:

TX waiting for tx_2 to first detect.
TX waiting for tx_2 to clear.
RX waiting for rx_1 to detect.
RX moving while waiting for rx_2 to detect.
If any timeout expires, the conveyor should stop and enter an error state.

Still Open

The main things still to define later are:

Physical GPIO pins for the two sensors.
How the command says role and direction.
MQTT topic structure and payload format.
Transfer speed value.
Timeout values and how they are configured.
Exact done/error messages back to the server.