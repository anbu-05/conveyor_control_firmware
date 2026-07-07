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

const STRING_CONFIG_LIMITS = {
  wifi_ssid: [1, 31],
  wifi_pass: [1, 63],
  conveyor_id: [1, 31],
  mqtt_broker_uri: [1, 127],
  mqtt_topic_cmd: [1, 95],
  mqtt_topic_emergency: [1, 95],
  mqtt_topic_feedback: [1, 95],
  mqtt_topic_all_emergency: [1, 95],
  mqtt_topic_tray: [1, 95],
};

const state = {
  connected: false,
  serialConnected: false,
  logLines: [],
  serialLogLines: [],
  lastSerialConfig: {},
  renderedConveyorIds: "",
  socket: null,
};

const el = {
  wsStatus: document.querySelector("#wsStatus"),
  mqttStatus: document.querySelector("#mqttStatus"),
  serialStatus: document.querySelector("#serialStatus"),
  mqttHost: document.querySelector("#mqttHost"),
  mqttPort: document.querySelector("#mqttPort"),
  conveyorIdA: document.querySelector("#conveyorIdA"),
  conveyorIdB: document.querySelector("#conveyorIdB"),
  connectForm: document.querySelector("#connectForm"),
  disconnectButton: document.querySelector("#disconnectButton"),
  conveyorCards: document.querySelector("#conveyorCards"),
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
  serialConfigHint: document.querySelector("#serialConfigHint"),
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

  el.conveyorCards.addEventListener("click", (event) => {
    const button = event.target.closest("button[data-command]");
    if (!button) return;
    const card = button.closest("[data-conveyor-id]");
    const conveyorId = card?.dataset.conveyorId || null;
    const command = button.dataset.command;
    if (command === "set_direction") {
      sendCommand(command, card.querySelector("[data-role='direction']").value, conveyorId);
    } else if (command === "set_kp") {
      sendGain(command, card.querySelector("[data-role='kp']").value, conveyorId);
    } else if (command === "set_kd") {
      sendGain(command, card.querySelector("[data-role='kd']").value, conveyorId);
    } else {
      sendCommand(command, null, conveyorId);
    }
  });

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
  el.serialConfigKey.addEventListener("change", updateSerialConfigInput);
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
  const conveyorIds = [el.conveyorIdA.value.trim() || "C0", el.conveyorIdB.value.trim() || "C1"]
    .filter((value, index, values) => value && values.indexOf(value) === index);
  const body = {
    mqtt_host: el.mqttHost.value.trim() || "192.168.1.126",
    mqtt_port: Number.parseInt(el.mqttPort.value, 10) || 1883,
    conveyor_id: conveyorIds[0] || "C0",
    conveyor_ids: conveyorIds,
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

async function sendCommand(command, value = null, conveyorId = null) {
  try {
    await postJson("/api/command", { command, value, conveyor_id: conveyorId });
  } catch (error) {
    appendLog(`Command failed: ${conveyorId ? `${conveyorId} ` : ""}${command}: ${error.message}`);
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

function sendGain(command, value, conveyorId) {
  const formatted = formatGain(value);
  if (formatted === null) {
    appendLog("Gain must be a decimal from 0.000 to 100.000");
    return;
  }
  sendCommand(command, formatted, conveyorId);
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
  if (STRING_CONFIG_LIMITS[key]) {
    const value = parseConfigString(el.serialConfigValue.value, key);
    if (value === null) return;
    sendSerialCommand("setconfig", [key, value]);
    return;
  }

  const [low, high] = CONFIG_LIMITS[key];
  const value = parseBoundedInt(el.serialConfigValue.value, low, high, key);
  if (value === null) return;
  sendSerialCommand("setconfig", [key, String(value)]);
}

function updateSerialConfigInput() {
  const key = el.serialConfigKey.value;
  if (STRING_CONFIG_LIMITS[key]) {
    const [low, high] = STRING_CONFIG_LIMITS[key];
    el.serialConfigValue.type = "text";
    el.serialConfigValue.removeAttribute("min");
    el.serialConfigValue.removeAttribute("max");
    el.serialConfigValue.maxLength = high;
    el.serialConfigValue.placeholder = `${low}-${high} chars, no spaces`;
    el.serialConfigHint.textContent = `${key}: ${low}-${high} characters. Spaces are not allowed by serial setconfig.`;
  } else {
    const [low, high] = CONFIG_LIMITS[key];
    el.serialConfigValue.type = "number";
    el.serialConfigValue.min = low;
    el.serialConfigValue.max = high;
    el.serialConfigValue.removeAttribute("maxLength");
    el.serialConfigValue.placeholder = `${low}-${high}`;
    el.serialConfigHint.textContent = `${key}: integer from ${low} to ${high}.`;
  }

  if (state.lastSerialConfig && state.lastSerialConfig[key] !== undefined) {
    el.serialConfigValue.value = state.lastSerialConfig[key];
  }
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
  const conveyorIds = snapshot.conveyor_ids || Object.keys(snapshot.conveyors || {});
  el.conveyorIdA.value = conveyorIds[0] || "C0";
  el.conveyorIdB.value = conveyorIds[1] || "C1";
  setPill(el.mqttStatus, state.connected ? "MQTT connected" : "MQTT disconnected", state.connected ? "good" : "bad");

  renderConveyorCards(snapshot);

  document.querySelectorAll(".command").forEach((button) => {
    button.disabled = !state.connected;
  });
}

function renderConveyorCards(snapshot) {
  const conveyorIds = snapshot.conveyor_ids || Object.keys(snapshot.conveyors || {});
  const conveyors = snapshot.conveyors || {};
  const conveyorSignature = conveyorIds.join("\u001f");

  if (state.renderedConveyorIds !== conveyorSignature) {
    el.conveyorCards.innerHTML = conveyorIds.map((conveyorId) => conveyorCardHtml(conveyorId, conveyors[conveyorId] || {})).join("");
    state.renderedConveyorIds = conveyorSignature;
  }

  conveyorIds.forEach((conveyorId) => updateConveyorCard(conveyorId, conveyors[conveyorId] || {}));
}

function conveyorCardHtml(conveyorId, conveyor) {
  const hasError = Boolean(conveyor.error) || conveyor.state === "ERROR" || conveyor.state === "ESTOP";
  return `
    <section class="card conveyor-card" data-conveyor-id="${escapeHtml(conveyorId)}">
      <div class="conveyor-card-header">
        <h2>${escapeHtml(conveyorId)}</h2>
        <span class="pill ${hasError ? "bad" : "muted"}" data-field="statePill">${escapeHtml(conveyor.state || "UNKNOWN")}</span>
      </div>
      <div class="state-card ${hasError ? "error" : ""}" data-field="stateCard">
        <div class="state-body">
          <span class="state-big" data-field="state">${escapeHtml(conveyor.state || "UNKNOWN")}</span>
          <span class="state-elapsed" data-field="elapsed">${conveyor.state_elapsed_ms == null ? "- ms" : `${conveyor.state_elapsed_ms} ms`}</span>
        </div>
        <div class="error-line" data-field="error">${escapeHtml(conveyor.error || "No active error")}</div>
      </div>
      <div class="metrics-row conveyor-metrics">
        <div class="metric"><span class="metric-label">Tray</span><strong data-field="tray">${trayText(conveyor.has_tray)}</strong></div>
        <div class="metric"><span class="metric-label">S0</span><strong data-field="s0">${sensorText(conveyor.s0)}</strong></div>
        <div class="metric"><span class="metric-label">S1</span><strong data-field="s1">${sensorText(conveyor.s1)}</strong></div>
        <div class="metric"><span class="metric-label">Direction</span><strong data-field="directionText">${escapeHtml(conveyor.direction || "UNKNOWN")}</strong></div>
        <div class="metric"><span class="metric-label">RSSI</span><strong data-field="rssi">${conveyor.rssi == null ? "-" : `${conveyor.rssi} dBm`}</strong></div>
        <div class="metric"><span class="metric-label">KP / KD</span><strong data-field="gains">${escapeHtml(conveyor.speed_kp || "-")} / ${escapeHtml(conveyor.speed_kd || "-")}</strong></div>
      </div>
      <div class="cmd-grid conveyor-command-grid">
        <button class="command btn-cmd" data-command="tx">TX</button>
        <button class="command btn-cmd" data-command="rx">RX</button>
        <button class="command btn-cmd" data-command="clear_error">Clear Error</button>
        <button class="command btn-cmd" data-command="get_direction">Get Direction</button>
        <button class="command btn-cmd" data-command="get_rssi">Get RSSI</button>
        <button class="command btn-danger" data-command="emergency_stop">Emergency Stop</button>
      </div>
      <div class="tune-row">
        <label class="tune-label">Direction</label>
        <select data-role="direction">
          <option value="s0tos1" ${conveyor.direction === "s0tos1" ? "selected" : ""}>S0 to S1</option>
          <option value="s1tos0" ${conveyor.direction === "s1tos0" ? "selected" : ""}>S1 to S0</option>
        </select>
        <button class="command btn-cmd" data-command="set_direction" type="button">Set</button>
      </div>
      <div class="tune-row">
        <label class="tune-label">KP</label>
        <input data-role="kp" inputmode="decimal" value="${escapeHtml(conveyor.speed_kp || "0.010")}" />
        <button class="command btn-cmd" data-command="set_kp" type="button">Set</button>
      </div>
      <div class="tune-row">
        <label class="tune-label">KD</label>
        <input data-role="kd" inputmode="decimal" value="${escapeHtml(conveyor.speed_kd || "0.010")}" />
        <button class="command btn-cmd" data-command="set_kd" type="button">Set</button>
      </div>
      <button class="command btn-ghost" data-command="reset_gains" type="button">Reset Gains</button>
    </section>
  `;
}

function updateConveyorCard(conveyorId, conveyor) {
  const card = el.conveyorCards.querySelector(`[data-conveyor-id='${cssEscape(conveyorId)}']`);
  if (!card) return;

  const hasError = Boolean(conveyor.error) || conveyor.state === "ERROR" || conveyor.state === "ESTOP";
  const stateText = conveyor.state || "UNKNOWN";
  const statePill = card.querySelector("[data-field='statePill']");
  const stateCard = card.querySelector("[data-field='stateCard']");
  const directionSelect = card.querySelector("[data-role='direction']");
  const kpInput = card.querySelector("[data-role='kp']");
  const kdInput = card.querySelector("[data-role='kd']");

  setText(card, "state", stateText);
  setText(card, "elapsed", conveyor.state_elapsed_ms == null ? "- ms" : `${conveyor.state_elapsed_ms} ms`);
  setText(card, "error", conveyor.error || "No active error");
  setText(card, "tray", trayText(conveyor.has_tray));
  setText(card, "s0", sensorText(conveyor.s0));
  setText(card, "s1", sensorText(conveyor.s1));
  setText(card, "directionText", conveyor.direction || "UNKNOWN");
  setText(card, "rssi", conveyor.rssi == null ? "-" : `${conveyor.rssi} dBm`);
  setText(card, "gains", `${conveyor.speed_kp || "-"} / ${conveyor.speed_kd || "-"}`);

  if (statePill) {
    statePill.textContent = stateText;
    statePill.classList.toggle("bad", hasError);
    statePill.classList.toggle("muted", !hasError);
  }
  if (stateCard) {
    stateCard.classList.toggle("error", hasError);
  }

  if (directionSelect && document.activeElement !== directionSelect && ["s0tos1", "s1tos0"].includes(conveyor.direction)) {
    directionSelect.value = conveyor.direction;
  }
  if (kpInput && document.activeElement !== kpInput && conveyor.speed_kp) {
    kpInput.value = conveyor.speed_kp;
  }
  if (kdInput && document.activeElement !== kdInput && conveyor.speed_kd) {
    kdInput.value = conveyor.speed_kd;
  }
}

function setText(root, field, value) {
  const node = root.querySelector(`[data-field='${field}']`);
  if (node && node.textContent !== String(value)) {
    node.textContent = value;
  }
}

function renderSerialSnapshot(snapshot) {
  state.serialConnected = Boolean(snapshot.connected);
  state.lastSerialConfig = snapshot.config || {};
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

function escapeHtml(value) {
  return String(value).replace(/[&<>'"]/g, (char) => ({
    "&": "&amp;",
    "<": "&lt;",
    ">": "&gt;",
    "'": "&#39;",
    '"': "&quot;",
  })[char]);
}

function cssEscape(value) {
  if (window.CSS && typeof window.CSS.escape === "function") {
    return window.CSS.escape(value);
  }
  return String(value).replace(/[^a-zA-Z0-9_-]/g, "\\$&");
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

function parseConfigString(value, label) {
  const parsed = value.trim();
  const [low, high] = STRING_CONFIG_LIMITS[label];
  if (parsed.length < low || parsed.length > high) {
    appendSerialLog("rx", `${label} must be ${low} to ${high} characters`);
    return null;
  }
  if (/\s/.test(parsed)) {
    appendSerialLog("rx", `${label} must not contain spaces`);
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
updateSerialConfigInput();
