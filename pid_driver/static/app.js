const SAMPLE_LIMIT = 600;

const state = {
  serialConnected: false,
  serialLogLines: [],
  commandQueue: [],
  commandWorkerRunning: false,
  commandInFlight: false,
  lastManualCommandAt: 0,
  statusSummary: {},
  statusCommands: [],
  statusMotorIds: [],
  pidMode: "",
  pollTimer: null,
  pollMetric: "",
  pollInFlight: false,
  awaitingPollMetric: "",
  latestPosition: null,
  latestSpeed: null,
  samples: [],
  socket: null,
};

const el = {
  wsStatus: document.querySelector("#wsStatus"),
  serialStatus: document.querySelector("#serialStatus"),
  serialConnectForm: document.querySelector("#serialConnectForm"),
  serialDisconnectButton: document.querySelector("#serialDisconnectButton"),
  serialPort: document.querySelector("#serialPort"),
  serialBaud: document.querySelector("#serialBaud"),
  serialStateCard: document.querySelector("#serialStateCard"),
  serialModeValue: document.querySelector("#serialModeValue"),
  serialReady: document.querySelector("#serialReady"),
  serialErrorValue: document.querySelector("#serialErrorValue"),
  serialMotorId: document.querySelector("#serialMotorId"),
  serialMotorInput: document.querySelector("#serialMotorInput"),
  serialMotorPwm: document.querySelector("#serialMotorPwm"),
  serialDirectionInput: document.querySelector("#serialDirectionInput"),
  serialDirectionValue: document.querySelector("#serialDirectionValue"),
  serialPositionValue: document.querySelector("#serialPositionValue"),
  serialSpeedValue: document.querySelector("#serialSpeedValue"),
  serialKpValue: document.querySelector("#serialKpValue"),
  serialKiValue: document.querySelector("#serialKiValue"),
  serialKdValue: document.querySelector("#serialKdValue"),
  statusAppName: document.querySelector("#statusAppName"),
  statusMachineId: document.querySelector("#statusMachineId"),
  statusTopicName: document.querySelector("#statusTopicName"),
  statusMqttUri: document.querySelector("#statusMqttUri"),
  statusMotorList: document.querySelector("#statusMotorList"),
  statusCommandList: document.querySelector("#statusCommandList"),
  clearSerialLogButton: document.querySelector("#clearSerialLogButton"),
  serialLogOutput: document.querySelector("#serialLogOutput"),
  refreshPidModeButton: document.querySelector("#refreshPidModeButton"),
  pidModePill: document.querySelector("#pidModePill"),
  pidModeSelect: document.querySelector("#pidModeSelect"),
  setPidModeButton: document.querySelector("#setPidModeButton"),
  serialPwmInput: document.querySelector("#serialPwmInput"),
  serialSetMotorButton: document.querySelector("#serialSetMotorButton"),
  serialStopMotorButton: document.querySelector("#serialStopMotorButton"),
  positionPanel: document.querySelector("#positionPanel"),
  speedPanel: document.querySelector("#speedPanel"),
  serialPositionInput: document.querySelector("#serialPositionInput"),
  serialSetPositionButton: document.querySelector("#serialSetPositionButton"),
  serialOffsetInput: document.querySelector("#serialOffsetInput"),
  serialSetOffsetButton: document.querySelector("#serialSetOffsetButton"),
  positionPollStatus: document.querySelector("#positionPollStatus"),
  positionPollInterval: document.querySelector("#positionPollInterval"),
  positionPollToggleButton: document.querySelector("#positionPollToggleButton"),
  serialSpeedInput: document.querySelector("#serialSpeedInput"),
  serialSetSpeedButton: document.querySelector("#serialSetSpeedButton"),
  speedPollStatus: document.querySelector("#speedPollStatus"),
  speedPollInterval: document.querySelector("#speedPollInterval"),
  speedPollToggleButton: document.querySelector("#speedPollToggleButton"),
  pidKpMilli: document.querySelector("#pidKpMilli"),
  pidKiMilli: document.querySelector("#pidKiMilli"),
  pidKdMilli: document.querySelector("#pidKdMilli"),
  setPidButton: document.querySelector("#setPidButton"),
  getPidButton: document.querySelector("#getPidButton"),
  clearGraphButton: document.querySelector("#clearGraphButton"),
  graphMetricLabel: document.querySelector("#graphMetricLabel"),
  graphSampleCount: document.querySelector("#graphSampleCount"),
  graphLatestValue: document.querySelector("#graphLatestValue"),
  signalGraph: document.querySelector("#signalGraph"),
  serialRawInput: document.querySelector("#serialRawInput"),
  serialRawButton: document.querySelector("#serialRawButton"),
};

function start() {
  bindEvents();
  connectWebSocket();
  loadSerialSnapshot();
  drawGraph();
}

function bindEvents() {
  el.serialConnectForm.addEventListener("submit", (event) => {
    event.preventDefault();
    connectSerial();
  });
  el.serialDisconnectButton.addEventListener("click", () => disconnectSerial());
  document.querySelectorAll(".serial-command[data-serial-command]").forEach((button) => {
    button.addEventListener("click", () => {
      // Combine the selected motor with fixed button args so pid_control can send M0 plus the requested 0/1 state.
      const args = button.dataset.needsMotor ? [currentMotorId(), ...parseDatasetArgs(button.dataset.serialArgs || "")] : parseDatasetArgs(button.dataset.serialArgs || "");
      sendSerialCommand(button.dataset.serialCommand, args);
    });
  });
  el.refreshPidModeButton.addEventListener("click", () => refreshPidMode());
  el.setPidModeButton.addEventListener("click", setPidModeCommand);
  el.serialSetMotorButton.addEventListener("click", setSerialMotor);
  el.serialStopMotorButton.addEventListener("click", () => sendSerialCommand("stopmotor", [currentMotorId()]));
  el.serialSetPositionButton.addEventListener("click", setSerialPosition);
  el.serialSetOffsetButton.addEventListener("click", setSerialOffset);
  el.serialSetSpeedButton.addEventListener("click", setSerialSpeed);
  el.positionPollToggleButton.addEventListener("click", () => togglePoll("position"));
  el.speedPollToggleButton.addEventListener("click", () => togglePoll("speed"));
  [el.positionPollInterval, el.speedPollInterval].forEach((input) => {
    input.addEventListener("change", () => restartPollIfActive(input));
    input.addEventListener("input", () => restartPollIfActive(input));
  });
  el.setPidButton.addEventListener("click", setPidGains);
  el.getPidButton.addEventListener("click", () => sendSerialCommand("getpid", [currentMotorId()]));
  el.clearGraphButton.addEventListener("click", clearGraph);
  el.serialRawButton.addEventListener("click", sendSerialRaw);
  el.clearSerialLogButton.addEventListener("click", () => {
    state.serialLogLines = [];
    renderSerialLog();
  });
  window.addEventListener("resize", drawGraph);
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
  socket.addEventListener("open", () => setPill(el.wsStatus, "WebSocket connected", "good"));
  socket.addEventListener("message", (event) => {
    const packet = JSON.parse(event.data);
    if (packet.serial_snapshot) {
      renderSerialSnapshot(packet.serial_snapshot);
    }
    if (packet.event && packet.type === "serial_event") {
      appendSerialLog(packet.event.direction, packet.event.message);
      applySerialEventData(packet.event.data);
    }
  });
  socket.addEventListener("close", () => {
    setPill(el.wsStatus, "WebSocket reconnecting", "bad");
    setTimeout(connectWebSocket, 1000);
  });
  socket.addEventListener("error", () => setPill(el.wsStatus, "WebSocket error", "bad"));
}

function applySerialEventData(data) {
  if (!data) return;
  if ((data.type === "status" || data.type === "status_ok") && data.values) {
    state.statusSummary = { ...state.statusSummary, ...data.values };
  } else if (data.type === "command_info" && data.text) {
    const commandName = data.command || data.text.split(" ", 1)[0];
    if (!state.statusCommands.some((existing) => existing.split(" ", 1)[0] === commandName)) {
      state.statusCommands.push(data.text);
    }
  } else if (data.type === "motor_info" && data.motor_id) {
    if (!state.statusMotorIds.includes(data.motor_id)) {
      state.statusMotorIds.push(data.motor_id);
    }
  } else if (data.type === "pidmode" && data.mode) {
    setPidMode(data.mode);
  }

  // Graph samples are accepted only from the expected poll response, not from manual set commands.
  if (data.type === "position" && state.awaitingPollMetric === "position") {
    const value = Number.parseInt(data.pos ?? data.position, 10);
    if (Number.isFinite(value)) appendGraphSample("position", value);
    state.awaitingPollMetric = "";
  } else if (data.type === "speed" && state.awaitingPollMetric === "speed") {
    const value = Number.parseInt(data.speed, 10);
    if (Number.isFinite(value)) appendGraphSample("speed", value);
    state.awaitingPollMetric = "";
  }
  renderStatusSummary(state.statusSummary, state.statusMotorIds, state.statusCommands);
}

async function connectSerial() {
  const body = {
    port: el.serialPort.value.trim() || "/dev/ttyACM0",
    baud: Number.parseInt(el.serialBaud.value, 10) || 115200,
  };
  try {
    const snapshot = await postJson("/api/serial/connect", body);
    renderSerialSnapshot(snapshot);
    // Query mode once after connect so the UI exposes only commands matching the firmware controller mode.
    await postSerialCommand("get_pidmode", [currentMotorId()]);
  } catch (error) {
    appendSerialLog("rx", `Serial connect failed: ${error.message}`);
  }
}

async function disconnectSerial() {
  stopPoll();
  await postJson("/api/serial/disconnect", {});
}

async function refreshPidMode() {
  if (!state.serialConnected) {
    appendSerialLog("rx", "Connect serial before refreshing PID mode");
    return;
  }
  await sendSerialCommand("get_pidmode", [currentMotorId()]);
}

function setPidModeCommand() {
  if (!state.serialConnected) {
    appendSerialLog("rx", "Connect serial before setting PID mode");
    return;
  }
  // The firmware returns OK PIDMODE, so the existing parser remains the single UI mode update path.
  sendSerialCommand("set_pidmode", [currentMotorId(), el.pidModeSelect.value]);
}

async function sendSerialCommand(command, args = []) {
  try {
    await postSerialCommand(command, args);
  } catch (error) {
    appendSerialLog("rx", `Command failed: ${command}: ${error.message}`);
  }
}

async function postSerialCommand(command, args = []) {
  return enqueueSerialCommand(command, args, { source: "manual" });
}

function enqueueSerialCommand(command, args = [], options = {}) {
  return new Promise((resolve, reject) => {
    state.commandQueue.push({ command, args, source: options.source || "manual", resolve, reject });
    processSerialCommandQueue();
  });
}

async function processSerialCommandQueue() {
  if (state.commandWorkerRunning) return;
  state.commandWorkerRunning = true;
  try {
    while (state.commandQueue.length > 0) {
      const item = state.commandQueue.shift();
      // Manual commands wait for the current poll write so operator actions do not interleave with graph reads.
      while (item.source !== "poll" && state.pollInFlight) {
        await sleep(20);
      }
      if (item.source !== "poll") {
        state.lastManualCommandAt = Date.now();
      }
      state.commandInFlight = true;
      try {
        const result = await postJson("/api/serial/command", { command: item.command, args: item.args });
        item.resolve(result);
      } catch (error) {
        item.reject(error);
      } finally {
        state.commandInFlight = false;
        await sleep(item.source === "poll" ? 120 : 180);
      }
    }
  } finally {
    state.commandWorkerRunning = false;
  }
}

function setSerialMotor() {
  const pwm = parseBoundedInt(el.serialPwmInput.value, 0, 255, "PWM");
  const direction = parseBoundedInt(el.serialDirectionInput.value, 0, 1, "direction");
  if (pwm === null || direction === null) return;
  sendSerialCommand("setmotor", [currentMotorId(), String(pwm), String(direction)]);
}

function setSerialPosition() {
  if (state.pidMode !== "position") {
    appendSerialLog("rx", "Position command blocked because PID mode is not position");
    return;
  }
  const position = parseInteger(el.serialPositionInput.value, "position");
  if (position === null) return;
  sendSerialCommand("setposition", [currentMotorId(), String(position)]);
}

function setSerialOffset() {
  const offset = parseInteger(el.serialOffsetInput.value, "offset");
  if (offset === null) return;
  sendSerialCommand("setoffset", [currentMotorId(), String(offset)]);
}

function setSerialSpeed() {
  if (state.pidMode !== "speed") {
    appendSerialLog("rx", "Speed command blocked because PID mode is not speed");
    return;
  }
  const speed = parseInteger(el.serialSpeedInput.value, "speed");
  if (speed === null) return;
  sendSerialCommand("setspeed", [currentMotorId(), String(speed)]);
}

function setPidGains() {
  const kp = parseBoundedInt(el.pidKpMilli.value, 0, 1000000, "KP milli");
  const ki = parseBoundedInt(el.pidKiMilli.value, 0, 1000000, "KI milli");
  const kd = parseBoundedInt(el.pidKdMilli.value, 0, 1000000, "KD milli");
  if (kp === null || ki === null || kd === null) return;
  sendSerialCommand("setpid", [currentMotorId(), String(kp), String(ki), String(kd)]);
}

function togglePoll(metric) {
  if (state.pollTimer && state.pollMetric === metric) {
    stopPoll();
    return;
  }
  startPoll(metric);
}

function startPoll(metric) {
  if (!state.serialConnected) {
    appendSerialLog("rx", "Connect serial before starting polling");
    renderPollState();
    return;
  }
  if (state.pidMode !== metric) {
    appendSerialLog("rx", `${metric} polling blocked because PID mode is ${state.pidMode || "unknown"}`);
    renderPollState();
    return;
  }
  const input = metric === "position" ? el.positionPollInterval : el.speedPollInterval;
  // Keep the UI floor at 1 ms as requested while the serial queue still throttles actual writes safely.
  const interval = parseBoundedInt(input.value, 1, 60000, `${metric} poll interval`);
  if (interval === null) return;
  stopPoll();
  state.pollMetric = metric;
  state.pollTimer = window.setInterval(pollOnce, interval);
  appendSerialLog("rx", `${metric} polling started every ${interval} ms`);
  renderPollState();
  pollOnce();
}

function restartPollIfActive(input) {
  if (!state.pollTimer) return;
  if ((state.pollMetric === "position" && input === el.positionPollInterval) || (state.pollMetric === "speed" && input === el.speedPollInterval)) {
    startPoll(state.pollMetric);
  }
}

function stopPoll() {
  const wasRunning = Boolean(state.pollTimer);
  if (state.pollTimer) {
    window.clearInterval(state.pollTimer);
    state.pollTimer = null;
  }
  state.pollInFlight = false;
  state.awaitingPollMetric = "";
  if (wasRunning) appendSerialLog("rx", "Polling stopped");
  renderPollState();
}

async function pollOnce() {
  if (!state.serialConnected || !state.pollMetric || state.pollInFlight || state.commandInFlight || state.commandQueue.length > 0) return;
  if (state.pidMode !== state.pollMetric) {
    stopPoll();
    return;
  }
  if (Date.now() - state.lastManualCommandAt < 250) return;
  state.pollInFlight = true;
  state.awaitingPollMetric = state.pollMetric;
  try {
    await enqueueSerialCommand(state.pollMetric === "position" ? "getposition" : "getspeed", [currentMotorId()], { source: "poll" });
  } catch (error) {
    appendSerialLog("rx", `${state.pollMetric} poll failed: ${error.message}`);
    stopPoll();
  } finally {
    state.pollInFlight = false;
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
  if (!response.ok) throw new Error(await response.text());
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

function renderSerialSnapshot(snapshot) {
  const wasConnected = state.serialConnected;
  state.serialConnected = Boolean(snapshot.connected);
  el.serialPort.value = snapshot.port || "/dev/ttyACM0";
  el.serialBaud.value = snapshot.baud || 115200;
  setPill(el.serialStatus, state.serialConnected ? "Serial connected" : "Serial disconnected", state.serialConnected ? "good" : "bad");

  const motor = snapshot.motor || {};
  const position = snapshot.position || {};
  const speed = snapshot.speed || {};
  const pid = snapshot.pid || {};
  const status = snapshot.status || {};
  state.statusSummary = { ...state.statusSummary, ...status };
  if (Array.isArray(snapshot.commands)) state.statusCommands = snapshot.commands;
  if (Array.isArray(snapshot.motor_ids)) state.statusMotorIds = snapshot.motor_ids;

  const motorId = snapshot.motor_id || "M0";
  el.serialMotorId.textContent = motorId;
  if (document.activeElement !== el.serialMotorInput) el.serialMotorInput.value = motorId;

  el.serialReady.textContent = snapshot.ready ? "ready" : "not ready";
  el.serialErrorValue.textContent = snapshot.last_error || "No serial errors";
  el.serialStateCard.classList.toggle("error", Boolean(snapshot.last_error));
  el.serialMotorPwm.textContent = valueText(motor.pwm);
  el.serialDirectionValue.textContent = valueText(motor.direction);

  const latestPosition = position.pos ?? position.position;
  if (Number.isFinite(latestPosition)) state.latestPosition = latestPosition;
  state.latestSpeed = Number.isFinite(speed.speed) ? speed.speed : state.latestSpeed;
  el.serialPositionValue.textContent = valueText(latestPosition);
  el.serialSpeedValue.textContent = valueText(speed.speed);
  renderPidGains(pid);
  if (pid.mode) setPidMode(pid.mode);

  if (!state.serialConnected && wasConnected) stopPoll();
  if (!state.serialConnected) stopPoll();
  renderStatusSummary(state.statusSummary, state.statusMotorIds, state.statusCommands);
  renderControls();
}

function setPidMode(mode) {
  const nextMode = mode === "position" || mode === "speed" ? mode : "";
  if (state.pidMode && state.pidMode !== nextMode) stopPoll();
  state.pidMode = nextMode;
  el.serialModeValue.textContent = nextMode ? nextMode.toUpperCase() : "UNKNOWN";
  if (nextMode && document.activeElement !== el.pidModeSelect) el.pidModeSelect.value = nextMode;
  setPill(el.pidModePill, nextMode ? `Mode ${nextMode}` : "Mode unknown", nextMode ? "good" : "bad");
  renderControls();
  drawGraph();
}

function renderPidGains(pid) {
  el.serialKpValue.textContent = valueText(pid.kp_milli);
  el.serialKiValue.textContent = valueText(pid.ki_milli);
  el.serialKdValue.textContent = valueText(pid.kd_milli);
  if (Number.isFinite(pid.kp_milli) && document.activeElement !== el.pidKpMilli) el.pidKpMilli.value = pid.kp_milli;
  if (Number.isFinite(pid.ki_milli) && document.activeElement !== el.pidKiMilli) el.pidKiMilli.value = pid.ki_milli;
  if (Number.isFinite(pid.kd_milli) && document.activeElement !== el.pidKdMilli) el.pidKdMilli.value = pid.kd_milli;
}

function renderStatusSummary(status, motorIds, commands) {
  el.statusAppName.textContent = valueText(status.app_name);
  el.statusMachineId.textContent = valueText(status.machine_id);
  el.statusTopicName.textContent = valueText(status.topic_name);
  el.statusMqttUri.textContent = valueText(status.mqtt_uri);
  el.statusMotorList.innerHTML = motorIds.length ? motorIds.map((motorId) => `<span class="chip">${escapeHtml(motorId)}</span>`).join("") : "-";
  el.statusCommandList.textContent = commands.length ? commands.join("\n") : "Run status to load firmware commands.";
}

function renderControls() {
  const connected = state.serialConnected;
  document.querySelectorAll(".serial-command, #serialDisconnectButton, #refreshPidModeButton, #setPidModeButton, #serialSetMotorButton, #serialStopMotorButton, #setPidButton, #getPidButton, #serialRawButton").forEach((button) => {
    button.disabled = !connected;
  });
  el.pidModeSelect.disabled = !connected;
  const positionEnabled = connected && state.pidMode === "position";
  const speedEnabled = connected && state.pidMode === "speed";
  el.positionPanel.classList.toggle("mode-disabled", !positionEnabled);
  el.speedPanel.classList.toggle("mode-disabled", !speedEnabled);
  [el.serialSetPositionButton, el.serialSetOffsetButton, el.positionPollToggleButton, el.positionPollInterval].forEach((node) => {
    node.disabled = !positionEnabled;
  });
  [el.serialSetSpeedButton, el.speedPollToggleButton, el.speedPollInterval].forEach((node) => {
    node.disabled = !speedEnabled;
  });
  renderPollState();
}

function renderPollState() {
  const running = Boolean(state.pollTimer);
  const positionRunning = running && state.pollMetric === "position";
  const speedRunning = running && state.pollMetric === "speed";
  setPill(el.positionPollStatus, positionRunning ? `Position poll ${el.positionPollInterval.value || 100} ms` : "Position poll off", positionRunning ? "good" : "muted");
  setPill(el.speedPollStatus, speedRunning ? `Speed poll ${el.speedPollInterval.value || 100} ms` : "Speed poll off", speedRunning ? "good" : "muted");
  el.positionPollToggleButton.textContent = positionRunning ? "Stop Poll" : "Start Poll";
  el.speedPollToggleButton.textContent = speedRunning ? "Stop Poll" : "Start Poll";
}

function appendGraphSample(metric, value) {
  if (state.samples.length && state.samples[state.samples.length - 1].metric !== metric) {
    state.samples = [];
  }
  state.samples.push({ metric, value, t: Date.now() });
  // Keep only recent samples because this graph redraws on every polled response and can become heavy.
  if (state.samples.length > SAMPLE_LIMIT) state.samples = state.samples.slice(-SAMPLE_LIMIT);
  drawGraph();
}

function clearGraph() {
  state.samples = [];
  drawGraph();
}

function drawGraph() {
  const canvas = el.signalGraph;
  const rect = canvas.getBoundingClientRect();
  const ratio = window.devicePixelRatio || 1;
  const width = Math.max(320, Math.floor(rect.width || canvas.width));
  const height = Math.max(180, Math.floor(rect.height || canvas.height));
  if (canvas.width !== width * ratio || canvas.height !== height * ratio) {
    canvas.width = width * ratio;
    canvas.height = height * ratio;
  }
  const ctx = canvas.getContext("2d");
  ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
  ctx.clearRect(0, 0, width, height);
  ctx.fillStyle = "#1a1a1a";
  ctx.fillRect(0, 0, width, height);
  ctx.strokeStyle = "#333333";
  ctx.lineWidth = 1;
  ctx.strokeRect(0.5, 0.5, width - 1, height - 1);

  const samples = state.samples;
  const metric = samples[0]?.metric || state.pidMode || "";
  el.graphMetricLabel.textContent = metric ? `Graph ${metric}` : "No metric";
  el.graphSampleCount.textContent = `${samples.length} sample${samples.length === 1 ? "" : "s"}`;
  el.graphLatestValue.textContent = samples.length ? `Latest ${samples[samples.length - 1].value}` : "Latest -";
  if (!samples.length) {
    ctx.fillStyle = "#8a8a8a";
    ctx.fillText("Start position or speed polling to graph samples", 18, 28);
    return;
  }

  const values = samples.map((sample) => sample.value);
  let min = Math.min(...values);
  let max = Math.max(...values);
  if (min === max) {
    min -= 1;
    max += 1;
  }
  const padLeft = 52;
  const padRight = 12;
  const padTop = 20;
  const padBottom = 28;
  const plotWidth = width - padLeft - padRight;
  const plotHeight = height - padTop - padBottom;
  ctx.strokeStyle = "#444444";
  ctx.beginPath();
  ctx.moveTo(padLeft, padTop);
  ctx.lineTo(padLeft, padTop + plotHeight);
  ctx.lineTo(padLeft + plotWidth, padTop + plotHeight);
  ctx.stroke();
  ctx.fillStyle = "#8a8a8a";
  ctx.fillText(String(max), 8, padTop + 4);
  ctx.fillText(String(min), 8, padTop + plotHeight);

  // Canvas keeps the graph dependency-free while still handling high-frequency poll redraws.
  ctx.strokeStyle = "#a78bfa";
  ctx.lineWidth = 2;
  ctx.beginPath();
  samples.forEach((sample, index) => {
    const x = padLeft + (samples.length === 1 ? plotWidth : (index / (samples.length - 1)) * plotWidth);
    const y = padTop + plotHeight - ((sample.value - min) / (max - min)) * plotHeight;
    if (index === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  });
  ctx.stroke();
}

function currentMotorId() {
  return el.serialMotorInput.value.trim() || "M0";
}

function setPill(node, text, mode) {
  node.textContent = text;
  node.classList.remove("good", "bad", "muted");
  node.classList.add(mode || "muted");
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

function parseInteger(value, label) {
  const parsed = Number.parseInt(value, 10);
  if (!Number.isInteger(parsed)) {
    appendSerialLog("rx", `${label} must be an integer`);
    return null;
  }
  return parsed;
}

function parseBoundedInt(value, low, high, label) {
  const parsed = parseInteger(value, label);
  if (parsed === null) return null;
  if (parsed < low || parsed > high) {
    appendSerialLog("rx", `${label} must be an integer from ${low} to ${high}`);
    return null;
  }
  return parsed;
}

function parseDatasetArgs(value) {
  if (!value) return [];
  return value.split(",").map((part) => part.trim()).filter(Boolean);
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function appendSerialLog(direction, line) {
  const stamp = new Date().toLocaleTimeString();
  const marker = direction === "tx" ? ">" : "<";
  state.serialLogLines.push(`[${stamp}] ${marker} ${line}`);
  if (state.serialLogLines.length > 1000) state.serialLogLines = state.serialLogLines.slice(-1000);
  renderSerialLog();
}

function renderSerialLog() {
  el.serialLogOutput.textContent = state.serialLogLines.join("\n");
  el.serialLogOutput.scrollTop = el.serialLogOutput.scrollHeight;
}

start();
