const CONFIG_LIMITS = {
  run_pwm: [0, 255],
  run_speed_counts_per_sec: [0, 100000],
  speed_kp_milli: [0, 100000],
  speed_kd_milli: [0, 100000],
  done_hold_ms: [0, 60000],
  tx_detect_timeout_ms: [1, 600000],
  tx_clear_timeout_ms: [1, 600000],
  rx_detect_timeout_ms: [1, 600000],
  rx_done_timeout_ms: [1, 600000],
  mqtt_status_period_ms: [100, 60000],
};

const state = {
  connected: false,
  serialConnected: false,
  logLines: [],
  serialLogLines: [],
  socket: null,
};

const el = {
  wsStatus: document.querySelector("#wsStatus"),
  mqttStatus: document.querySelector("#mqttStatus"),
  serialStatus: document.querySelector("#serialStatus"),
  mqttHost: document.querySelector("#mqttHost"),
  mqttPort: document.querySelector("#mqttPort"),
  conveyorId: document.querySelector("#conveyorId"),
  connectForm: document.querySelector("#connectForm"),
  disconnectButton: document.querySelector("#disconnectButton"),
  stateCard: document.querySelector("#stateCard"),
  stateValue: document.querySelector("#stateValue"),
  elapsedValue: document.querySelector("#elapsedValue"),
  errorValue: document.querySelector("#errorValue"),
  trayValue: document.querySelector("#trayValue"),
  s0Value: document.querySelector("#s0Value"),
  s1Value: document.querySelector("#s1Value"),
  directionValue: document.querySelector("#directionValue"),
  rssiValue: document.querySelector("#rssiValue"),
  kpValue: document.querySelector("#kpValue"),
  kdValue: document.querySelector("#kdValue"),
  directionSelect: document.querySelector("#directionSelect"),
  setDirectionButton: document.querySelector("#setDirectionButton"),
  kpInput: document.querySelector("#kpInput"),
  kdInput: document.querySelector("#kdInput"),
  setKpButton: document.querySelector("#setKpButton"),
  setKdButton: document.querySelector("#setKdButton"),
  clearLogButton: document.querySelector("#clearLogButton"),
  logOutput: document.querySelector("#logOutput"),

  serialPanel: document.querySelector("#serialPanel"),
  serialConnectForm: document.querySelector("#serialConnectForm"),
  serialDisconnectButton: document.querySelector("#serialDisconnectButton"),
  serialPort: document.querySelector("#serialPort"),
  serialBaud: document.querySelector("#serialBaud"),
  serialStateCard: document.querySelector("#serialStateCard"),
  serialJobState: document.querySelector("#serialJobState"),
  serialReady: document.querySelector("#serialReady"),
  serialErrorValue: document.querySelector("#serialErrorValue"),
  serialMotorPwm: document.querySelector("#serialMotorPwm"),
  serialCurrentSpeed: document.querySelector("#serialCurrentSpeed"),
  serialTargetSpeed: document.querySelector("#serialTargetSpeed"),
  serialEncoderCount: document.querySelector("#serialEncoderCount"),
  serialTrayValue: document.querySelector("#serialTrayValue"),
  serialDirectionValue: document.querySelector("#serialDirectionValue"),
  clearSerialLogButton: document.querySelector("#clearSerialLogButton"),
  serialLogOutput: document.querySelector("#serialLogOutput"),
  serialPwmInput: document.querySelector("#serialPwmInput"),
  serialDirectionInput: document.querySelector("#serialDirectionInput"),
  serialSetMotorButton: document.querySelector("#serialSetMotorButton"),
  serialStopMotorButton: document.querySelector("#serialStopMotorButton"),
  serialSpeedInput: document.querySelector("#serialSpeedInput"),
  serialSetSpeedButton: document.querySelector("#serialSetSpeedButton"),
  serialConfigKey: document.querySelector("#serialConfigKey"),
  serialConfigValue: document.querySelector("#serialConfigValue"),
  serialGetConfigButton: document.querySelector("#serialGetConfigButton"),
  serialSetConfigButton: document.querySelector("#serialSetConfigButton"),
  serialKpInput: document.querySelector("#serialKpInput"),
  serialKdInput: document.querySelector("#serialKdInput"),
  serialSetKpButton: document.querySelector("#serialSetKpButton"),
  serialSetKdButton: document.querySelector("#serialSetKdButton"),
  serialSensorWatch: document.querySelector("#serialSensorWatch"),
  serialEncoderWatch: document.querySelector("#serialEncoderWatch"),
  serialRawInput: document.querySelector("#serialRawInput"),
  serialRawButton: document.querySelector("#serialRawButton"),
};

function start() {
  bindEvents();
  connectWebSocket();
  loadSnapshot();
  loadSerialSnapshot();
}

function bindEvents() {
  document.querySelectorAll(".tab-button").forEach((button) => {
    button.addEventListener("click", () => switchTab(button.dataset.tab));
  });

  el.connectForm.addEventListener("submit", (event) => {
    event.preventDefault();
    connectMqtt();
  });
  el.disconnectButton.addEventListener("click", () => postJson("/api/disconnect", {}));

  document.querySelectorAll(".command[data-command]").forEach((button) => {
    button.addEventListener("click", () => sendCommand(button.dataset.command));
  });

  el.setDirectionButton.addEventListener("click", () => {
    sendCommand("set_direction", el.directionSelect.value);
  });
  el.setKpButton.addEventListener("click", () => sendGain("set_kp", el.kpInput.value));
  el.setKdButton.addEventListener("click", () => sendGain("set_kd", el.kdInput.value));
  el.clearLogButton.addEventListener("click", () => {
    state.logLines = [];
    renderLog();
  });

  el.serialConnectForm.addEventListener("submit", (event) => {
    event.preventDefault();
    connectSerial();
  });
  el.serialDisconnectButton.addEventListener("click", () => postJson("/api/serial/disconnect", {}));
  document.querySelectorAll(".serial-command[data-serial-command]").forEach((button) => {
    button.addEventListener("click", () => {
      const args = parseDatasetArgs(button.dataset.serialArgs || "");
      sendSerialCommand(button.dataset.serialCommand, args, button.dataset.confirm || "");
    });
  });

  el.serialSetMotorButton.addEventListener("click", setSerialMotor);
  el.serialSetSpeedButton.addEventListener("click", setSerialSpeed);
  el.serialGetConfigButton.addEventListener("click", () => {
    sendSerialCommand("getconfig", [el.serialConfigKey.value]);
  });
  el.serialSetConfigButton.addEventListener("click", setSerialConfig);
  el.serialSetKpButton.addEventListener("click", () => sendSerialGain("setkp", el.serialKpInput.value));
  el.serialSetKdButton.addEventListener("click", () => sendSerialGain("setkd", el.serialKdInput.value));
  el.serialRawButton.addEventListener("click", sendSerialRaw);
  el.clearSerialLogButton.addEventListener("click", () => {
    state.serialLogLines = [];
    renderSerialLog();
  });
}

function switchTab(tab) {
  document.querySelectorAll(".tab-button").forEach((button) => {
    button.classList.toggle("active", button.dataset.tab === tab);
  });
  document.querySelectorAll(".tab-panel").forEach((panel) => {
    panel.classList.toggle("active", panel.id === `${tab}Panel`);
  });
}

async function loadSnapshot() {
  try {
    const snapshot = await fetchJson("/api/snapshot");
    renderSnapshot(snapshot);
  } catch (error) {
    appendLog(`Snapshot load failed: ${error.message}`);
  }
}

async function loadSerialSnapshot() {
  try {
    const snapshot = await fetchJson("/api/serial/snapshot");
    renderSerialSnapshot(snapshot);
  } catch (error) {
    appendSerialLog("rx", `Serial snapshot load failed: ${error.message}`);
  }
}

function connectWebSocket() {
  const protocol = window.location.protocol === "https:" ? "wss" : "ws";
  const socket = new WebSocket(`${protocol}://${window.location.host}/ws`);
  state.socket = socket;

  socket.addEventListener("open", () => {
    setPill(el.wsStatus, "WebSocket connected", "good");
  });
  socket.addEventListener("message", (event) => {
    const packet = JSON.parse(event.data);
    if (packet.snapshot) {
      renderSnapshot(packet.snapshot);
    }
    if (packet.serial_snapshot) {
      renderSerialSnapshot(packet.serial_snapshot);
    }
    if (packet.event && packet.type !== "serial_event") {
      appendLog(packet.event.message);
    }
    if (packet.event && packet.type === "serial_event") {
      appendSerialLog(packet.event.direction, packet.event.message);
    }
  });
  socket.addEventListener("close", () => {
    setPill(el.wsStatus, "WebSocket reconnecting", "bad");
    setTimeout(connectWebSocket, 1000);
  });
  socket.addEventListener("error", () => {
    setPill(el.wsStatus, "WebSocket error", "bad");
  });
}

async function connectMqtt() {
  const body = {
    mqtt_host: el.mqttHost.value.trim() || "192.168.1.126",
    mqtt_port: Number.parseInt(el.mqttPort.value, 10) || 1883,
    conveyor_id: el.conveyorId.value.trim() || "C0",
  };
  try {
    const snapshot = await postJson("/api/connect", body);
    renderSnapshot(snapshot);
  } catch (error) {
    appendLog(`Connect failed: ${error.message}`);
  }
}

async function connectSerial() {
  const body = {
    port: el.serialPort.value.trim() || "/dev/ttyACM0",
    baud: Number.parseInt(el.serialBaud.value, 10) || 115200,
  };
  try {
    const snapshot = await postJson("/api/serial/connect", body);
    renderSerialSnapshot(snapshot);
  } catch (error) {
    appendSerialLog("rx", `Serial connect failed: ${error.message}`);
  }
}

async function sendCommand(command, value = null) {
  try {
    await postJson("/api/command", { command, value });
  } catch (error) {
    appendLog(`Command failed: ${command}: ${error.message}`);
  }
}

async function sendSerialCommand(command, args = [], confirmMessage = "") {
  if (confirmMessage && !window.confirm(confirmMessage)) {
    return;
  }
  try {
    await postJson("/api/serial/command", { command, args });
  } catch (error) {
    appendSerialLog("rx", `Command failed: ${command}: ${error.message}`);
  }
}

function sendGain(command, value) {
  const formatted = formatGain(value);
  if (formatted === null) {
    appendLog("Gain must be a decimal from 0.000 to 100.000");
    return;
  }
  sendCommand(command, formatted);
}

function sendSerialGain(command, value) {
  const formatted = formatGain(value);
  if (formatted === null) {
    appendSerialLog("rx", "Gain must be a decimal from 0.000 to 100.000");
    return;
  }
  sendSerialCommand(command, [formatted]);
}

function setSerialMotor() {
  const pwm = parseBoundedInt(el.serialPwmInput.value, 0, 255, "PWM");
  const direction = parseBoundedInt(el.serialDirectionInput.value, 0, 1, "direction");
  if (pwm === null || direction === null) return;
  sendSerialCommand(
    "setmotor",
    ["M0", String(pwm), String(direction)],
    `Set direct motor PWM ${pwm} direction ${direction}?`,
  );
}

function setSerialSpeed() {
  const speed = parseBoundedInt(el.serialSpeedInput.value, -100000, 100000, "speed");
  if (speed === null) return;
  const confirmMessage = speed === 0 ? "" : `Set direct speed target ${speed} counts/sec?`;
  sendSerialCommand("setspeed", ["M0", String(speed)], confirmMessage);
}

function setSerialConfig() {
  const key = el.serialConfigKey.value;
  const [low, high] = CONFIG_LIMITS[key];
  const value = parseBoundedInt(el.serialConfigValue.value, low, high, key);
  if (value === null) return;
  sendSerialCommand("setconfig", [key, String(value)]);
}

async function sendSerialRaw() {
  const line = el.serialRawInput.value.trim();
  if (!line) {
    appendSerialLog("rx", "Raw command line is required");
    return;
  }
  if (line.includes("\n") || line.includes("\r")) {
    appendSerialLog("rx", "Raw command must be a single line");
    return;
  }
  if (!window.confirm(`Send raw serial command: ${line}?`)) {
    return;
  }
  try {
    await postJson("/api/serial/raw", { line });
    el.serialRawInput.value = "";
  } catch (error) {
    appendSerialLog("rx", `Raw command failed: ${error.message}`);
  }
}

async function fetchJson(url) {
  const response = await fetch(url);
  if (!response.ok) {
    throw new Error(await response.text());
  }
  return response.json();
}

async function postJson(url, body) {
  const response = await fetch(url, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  if (!response.ok) {
    const error = await response.json().catch(() => ({ detail: response.statusText }));
    throw new Error(error.detail || response.statusText);
  }
  return response.json();
}

function renderSnapshot(snapshot) {
  state.connected = Boolean(snapshot.connected);
  el.mqttHost.value = snapshot.mqtt_host || "192.168.1.126";
  el.mqttPort.value = snapshot.mqtt_port || 1883;
  el.conveyorId.value = snapshot.conveyor_id || "C0";
  setPill(el.mqttStatus, state.connected ? "MQTT connected" : "MQTT disconnected", state.connected ? "good" : "bad");

  el.stateValue.textContent = snapshot.state || "UNKNOWN";
  el.elapsedValue.textContent = snapshot.state_elapsed_ms == null ? "- ms" : `${snapshot.state_elapsed_ms} ms`;
  el.errorValue.textContent = snapshot.error || "No active error";
  el.trayValue.textContent = trayText(snapshot.has_tray);
  el.s0Value.textContent = sensorText(snapshot.s0);
  el.s1Value.textContent = sensorText(snapshot.s1);
  el.directionValue.textContent = snapshot.direction || "UNKNOWN";
  el.rssiValue.textContent = snapshot.rssi == null ? "-" : `${snapshot.rssi} dBm`;
  el.kpValue.textContent = snapshot.speed_kp || "-";
  el.kdValue.textContent = snapshot.speed_kd || "-";

  el.stateCard.classList.toggle("error", Boolean(snapshot.error) || snapshot.state === "ERROR" || snapshot.state === "ESTOP");
  document.querySelectorAll(".command").forEach((button) => {
    button.disabled = !state.connected;
  });
}

function renderSerialSnapshot(snapshot) {
  state.serialConnected = Boolean(snapshot.connected);
  el.serialPort.value = snapshot.port || "/dev/ttyACM0";
  el.serialBaud.value = snapshot.baud || 115200;
  setPill(el.serialStatus, state.serialConnected ? "Serial connected" : "Serial disconnected", state.serialConnected ? "good" : "bad");

  el.serialJobState.textContent = snapshot.job_state || "UNKNOWN";
  el.serialReady.textContent = snapshot.ready ? "ready" : "not ready";
  el.serialErrorValue.textContent = snapshot.last_error || "No serial errors";
  el.serialStateCard.classList.toggle("error", Boolean(snapshot.last_error));

  const motor = snapshot.motor || {};
  const encoder = snapshot.encoder || {};
  const tray = snapshot.tray || {};
  el.serialMotorPwm.textContent = valueText(motor.pwm);
  el.serialCurrentSpeed.textContent = valueText(motor.current_speed ?? encoder.speed);
  el.serialTargetSpeed.textContent = valueText(motor.target_speed);
  el.serialEncoderCount.textContent = valueText(encoder.count ?? motor.position);
  el.serialTrayValue.textContent = trayText(tray.has_tray);
  el.serialDirectionValue.textContent = snapshot.direction || "UNKNOWN";

  if (snapshot.config && snapshot.config[el.serialConfigKey.value] !== undefined) {
    el.serialConfigValue.value = snapshot.config[el.serialConfigKey.value];
  }
  if (snapshot.config?.speed_kp) {
    el.serialKpInput.value = snapshot.config.speed_kp;
  }
  if (snapshot.config?.speed_kd) {
    el.serialKdInput.value = snapshot.config.speed_kd;
  }

  setPill(el.serialSensorWatch, snapshot.sensor_watch ? "Sensors on" : "Sensors off", snapshot.sensor_watch ? "good" : "muted");
  setPill(el.serialEncoderWatch, snapshot.encoder_watch ? "Encoder on" : "Encoder off", snapshot.encoder_watch ? "good" : "muted");

  document.querySelectorAll(".serial-command, #serialSetMotorButton, #serialSetSpeedButton, #serialSetConfigButton, #serialGetConfigButton, #serialSetKpButton, #serialSetKdButton, #serialRawButton").forEach((button) => {
    button.disabled = !state.serialConnected;
  });
}

function setPill(node, text, mode) {
  node.textContent = text;
  node.classList.remove("good", "bad", "muted");
  node.classList.add(mode || "muted");
}

function trayText(value) {
  if (value === null || value === undefined) return "Unknown";
  return value ? "Present" : "Empty";
}

function sensorText(value) {
  if (value === null || value === undefined) return "-";
  if (value === 0) return "0 detected";
  if (value === 1) return "1 clear";
  return String(value);
}

function valueText(value) {
  if (value === null || value === undefined || value === "") return "-";
  return String(value);
}

function formatGain(value) {
  const parsed = Number.parseFloat(value);
  if (!Number.isFinite(parsed) || parsed < 0 || parsed > 100) {
    return null;
  }
  return parsed.toFixed(3);
}

function parseBoundedInt(value, low, high, label) {
  const parsed = Number.parseInt(value, 10);
  if (!Number.isInteger(parsed) || parsed < low || parsed > high) {
    appendSerialLog("rx", `${label} must be an integer from ${low} to ${high}`);
    return null;
  }
  return parsed;
}

function parseDatasetArgs(value) {
  if (!value) return [];
  return value.split(",").map((part) => part.trim()).filter(Boolean);
}

function appendLog(line) {
  const stamp = new Date().toLocaleTimeString();
  state.logLines.push(`[${stamp}] ${line}`);
  if (state.logLines.length > 500) {
    state.logLines = state.logLines.slice(-500);
  }
  renderLog();
}

function appendSerialLog(direction, line) {
  const stamp = new Date().toLocaleTimeString();
  const marker = direction === "tx" ? ">" : "<";
  state.serialLogLines.push(`[${stamp}] ${marker} ${line}`);
  if (state.serialLogLines.length > 1000) {
    state.serialLogLines = state.serialLogLines.slice(-1000);
  }
  renderSerialLog();
}

function renderLog() {
  el.logOutput.textContent = state.logLines.join("\n");
  el.logOutput.scrollTop = el.logOutput.scrollHeight;
}

function renderSerialLog() {
  el.serialLogOutput.textContent = state.serialLogLines.join("\n");
  el.serialLogOutput.scrollTop = el.serialLogOutput.scrollHeight;
}

start();
