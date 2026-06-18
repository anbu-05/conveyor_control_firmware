const state = {
  connected: false,
  logLines: [],
  socket: null,
};

const el = {
  wsStatus: document.querySelector("#wsStatus"),
  mqttStatus: document.querySelector("#mqttStatus"),
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
};

function start() {
  bindEvents();
  connectWebSocket();
  loadSnapshot();
}

function bindEvents() {
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
}

async function loadSnapshot() {
  try {
    const snapshot = await fetchJson("/api/snapshot");
    renderSnapshot(snapshot);
  } catch (error) {
    appendLog(`Snapshot load failed: ${error.message}`);
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
    if (packet.event) {
      appendLog(packet.event.message);
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

async function sendCommand(command, value = null) {
  try {
    await postJson("/api/command", { command, value });
  } catch (error) {
    appendLog(`Command failed: ${command}: ${error.message}`);
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

function formatGain(value) {
  const parsed = Number.parseFloat(value);
  if (!Number.isFinite(parsed) || parsed < 0 || parsed > 100) {
    return null;
  }
  return parsed.toFixed(3);
}

function appendLog(line) {
  const stamp = new Date().toLocaleTimeString();
  state.logLines.push(`[${stamp}] ${line}`);
  if (state.logLines.length > 500) {
    state.logLines = state.logLines.slice(-500);
  }
  renderLog();
}

function renderLog() {
  el.logOutput.textContent = state.logLines.join("\n");
  el.logOutput.scrollTop = el.logOutput.scrollHeight;
}

start();
