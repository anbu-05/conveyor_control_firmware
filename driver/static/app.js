const CONFIG_LIMITS = {
  pid_kp_milli: [0, 100000],
  pid_ki_milli: [0, 100000],
  pid_kd_milli: [0, 100000],
  max_pwm: [0, 255],
  max_speed_counts_per_sec: [0, 100000],
  position_tolerance_counts: [0, 100000],
};

const state = {
  serialConnected: false,
  serialLogLines: [],
  commandQueue: [],
  commandWorkerRunning: false,
  commandInFlight: false,
  lastManualCommandAt: 0,
  lastSerialConfig: {},
  latestPosition: null,
  statusSummary: {},
  statusCommands: [],
  statusMotorIds: [],
  positionPollTimer: null,
  positionPollInFlight: false,
  pidTune: {
    running: false,
    abort: false,
    startPosition: null,
    outboundTarget: null,
    lastPosition: null,
    history: [],
  },
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
  serialJobState: document.querySelector("#serialJobState"),
  serialReady: document.querySelector("#serialReady"),
  serialErrorValue: document.querySelector("#serialErrorValue"),
  serialMotorId: document.querySelector("#serialMotorId"),
  serialMotorInput: document.querySelector("#serialMotorInput"),
  serialMotorPwm: document.querySelector("#serialMotorPwm"),
  serialDirectionValue: document.querySelector("#serialDirectionValue"),
  serialPositionValue: document.querySelector("#serialPositionValue"),
  serialTrayValue: document.querySelector("#serialTrayValue"),
  serialUpstreamSensor: document.querySelector("#serialUpstreamSensor"),
  serialDownstreamSensor: document.querySelector("#serialDownstreamSensor"),
  serialPositionControl: document.querySelector("#serialPositionControl"),
  serialPidPill: document.querySelector("#serialPidPill"),
  statusCommandCount: document.querySelector("#statusCommandCount"),
  statusAppName: document.querySelector("#statusAppName"),
  statusMachineId: document.querySelector("#statusMachineId"),
  statusTopicName: document.querySelector("#statusTopicName"),
  statusMqttUri: document.querySelector("#statusMqttUri"),
  statusMotorList: document.querySelector("#statusMotorList"),
  statusCommandList: document.querySelector("#statusCommandList"),
  clearSerialLogButton: document.querySelector("#clearSerialLogButton"),
  serialLogOutput: document.querySelector("#serialLogOutput"),
  serialPwmInput: document.querySelector("#serialPwmInput"),
  serialDirectionInput: document.querySelector("#serialDirectionInput"),
  serialSetMotorButton: document.querySelector("#serialSetMotorButton"),
  serialStopMotorButton: document.querySelector("#serialStopMotorButton"),
  serialPositionControlOnButton: document.querySelector("#serialPositionControlOnButton"),
  serialPositionControlOffButton: document.querySelector("#serialPositionControlOffButton"),
  serialPositionInput: document.querySelector("#serialPositionInput"),
  serialSetPositionButton: document.querySelector("#serialSetPositionButton"),
  serialOffsetInput: document.querySelector("#serialOffsetInput"),
  serialSetOffsetButton: document.querySelector("#serialSetOffsetButton"),
  positionPollStatus: document.querySelector("#positionPollStatus"),
  positionPollInterval: document.querySelector("#positionPollInterval"),
  positionPollToggleButton: document.querySelector("#positionPollToggleButton"),
  serialConfigKey: document.querySelector("#serialConfigKey"),
  serialConfigValue: document.querySelector("#serialConfigValue"),
  serialConfigHint: document.querySelector("#serialConfigHint"),
  serialGetConfigButton: document.querySelector("#serialGetConfigButton"),
  serialSetConfigButton: document.querySelector("#serialSetConfigButton"),
  serialResetConfigButton: document.querySelector("#serialResetConfigButton"),
  pidTuneKpMilli: document.querySelector("#pidTuneKpMilli"),
  pidTuneKiMilli: document.querySelector("#pidTuneKiMilli"),
  pidTuneKdMilli: document.querySelector("#pidTuneKdMilli"),
  pidTuneStepCounts: document.querySelector("#pidTuneStepCounts"),
  pidTunePollMs: document.querySelector("#pidTunePollMs"),
  pidTuneTolerance: document.querySelector("#pidTuneTolerance"),
  pidTuneTimeoutMs: document.querySelector("#pidTuneTimeoutMs"),
  pidTuneDisableAfter: document.querySelector("#pidTuneDisableAfter"),
  pidTuneApplyButton: document.querySelector("#pidTuneApplyButton"),
  pidTuneRunButton: document.querySelector("#pidTuneRunButton"),
  pidTuneAbortButton: document.querySelector("#pidTuneAbortButton"),
  pidTuneStartValue: document.querySelector("#pidTuneStartValue"),
  pidTuneTargetValue: document.querySelector("#pidTuneTargetValue"),
  pidTuneCurrentValue: document.querySelector("#pidTuneCurrentValue"),
  pidTuneResultValue: document.querySelector("#pidTuneResultValue"),
  pidTuneHistory: document.querySelector("#pidTuneHistory"),
  serialRawInput: document.querySelector("#serialRawInput"),
  serialRawButton: document.querySelector("#serialRawButton"),
};

function start() {
  bindEvents();
  connectWebSocket();
  loadSerialSnapshot();
  updateSerialConfigInput();
}

function bindEvents() {
  el.serialConnectForm.addEventListener("submit", (event) => {
    event.preventDefault();
    connectSerial();
  });
  el.serialDisconnectButton.addEventListener("click", () => disconnectSerial());

  document.querySelectorAll(".serial-command[data-serial-command]").forEach((button) => {
    button.addEventListener("click", () => {
      const args = button.classList.contains("needs-motor") ? [currentMotorId()] : parseDatasetArgs(button.dataset.serialArgs || "");
      sendSerialCommand(button.dataset.serialCommand, args);
    });
  });

  el.serialSetMotorButton.addEventListener("click", setSerialMotor);
  el.serialStopMotorButton.addEventListener("click", () => sendSerialCommand("stopmotor", [currentMotorId()]));
  el.serialPositionControlOnButton.addEventListener("click", () => sendSerialCommand("positioncontrol", [currentMotorId(), "1"]));
  el.serialPositionControlOffButton.addEventListener("click", () => sendSerialCommand("positioncontrol", [currentMotorId(), "0"]));
  el.serialSetPositionButton.addEventListener("click", setSerialPosition);
  el.serialSetOffsetButton.addEventListener("click", setSerialOffset);
  el.positionPollToggleButton.addEventListener("click", togglePositionPoll);
  el.positionPollInterval.addEventListener("change", () => {
    if (state.positionPollTimer) restartPositionPoll();
  });
  el.positionPollInterval.addEventListener("input", () => {
    if (state.positionPollTimer) restartPositionPoll();
  });
  document.addEventListener("click", (event) => {
    const button = event.target.closest("#positionPollToggleButton");
    if (button && button !== el.positionPollToggleButton) {
      togglePositionPoll();
    }
  });
  el.serialGetConfigButton.addEventListener("click", () => sendSerialCommand("getconfig", [el.serialConfigKey.value]));
  el.serialConfigKey.addEventListener("change", updateSerialConfigInput);
  el.serialSetConfigButton.addEventListener("click", setSerialConfig);
  el.serialResetConfigButton.addEventListener("click", () => {
    sendSerialCommand("resetconfig", [el.serialConfigKey.value]);
  });
  el.pidTuneApplyButton.addEventListener("click", () => applyPidTuneGains());
  el.pidTuneRunButton.addEventListener("click", runPidTuneStepReturn);
  el.pidTuneAbortButton.addEventListener("click", abortPidTune);
  el.serialRawButton.addEventListener("click", sendSerialRaw);
  el.clearSerialLogButton.addEventListener("click", () => {
    state.serialLogLines = [];
    renderSerialLog();
  });
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
  socket.addEventListener("error", () => {
    setPill(el.wsStatus, "WebSocket error", "bad");
  });
}

function applySerialEventData(data) {
  if (!data) return;
  if (data.type === "status" && data.values) {
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
  } catch (error) {
    appendSerialLog("rx", `Serial connect failed: ${error.message}`);
  }
}

async function disconnectSerial() {
  stopPositionPoll();
  await postJson("/api/serial/disconnect", {});
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
      while (item.source !== "poll" && state.positionPollInFlight) {
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
  sendSerialCommand(
    "setmotor",
    [currentMotorId(), String(pwm), String(direction)],
  );
}

function setSerialPosition() {
  const position = parseInteger(el.serialPositionInput.value, "position");
  if (position === null) return;
  sendSerialCommand("setposition", [currentMotorId(), String(position)]);
}

function setSerialOffset() {
  const offset = parseInteger(el.serialOffsetInput.value, "offset");
  if (offset === null) return;
  sendSerialCommand("setoffset", [currentMotorId(), String(offset)]);
}

function togglePositionPoll() {
  if (state.positionPollTimer) {
    stopPositionPoll();
    return;
  }
  startPositionPoll();
}

function startPositionPoll() {
  if (!state.serialConnected) {
    appendSerialLog("rx", "Connect serial before starting position polling");
    renderPositionPollState();
    return;
  }
  const interval = parseBoundedInt(el.positionPollInterval.value, 50, 60000, "position poll interval");
  if (interval === null) return;
  stopPositionPoll();
  state.positionPollTimer = window.setInterval(pollPositionOnce, interval);
  appendSerialLog("rx", `Position polling started every ${interval} ms`);
  renderPositionPollState();
  pollPositionOnce();
}

function restartPositionPoll() {
  if (!state.positionPollTimer) return;
  startPositionPoll();
}

function stopPositionPoll() {
  const wasRunning = Boolean(state.positionPollTimer);
  if (state.positionPollTimer) {
    window.clearInterval(state.positionPollTimer);
    state.positionPollTimer = null;
  }
  state.positionPollInFlight = false;
  if (wasRunning) {
    appendSerialLog("rx", "Position polling stopped");
  }
  renderPositionPollState();
}

async function pollPositionOnce() {
  if (!state.serialConnected || state.positionPollInFlight || state.commandInFlight || state.commandQueue.length > 0) return;
  if (Date.now() - state.lastManualCommandAt < 250) return;
  state.positionPollInFlight = true;
  try {
    await enqueueSerialCommand("getposition", [currentMotorId()], { source: "poll" });
  } catch (error) {
    appendSerialLog("rx", `Position poll failed: ${error.message}`);
    stopPositionPoll();
  } finally {
    state.positionPollInFlight = false;
  }
}

function setSerialConfig() {
  const key = el.serialConfigKey.value;
  const [low, high] = CONFIG_LIMITS[key];
  const value = parseBoundedInt(el.serialConfigValue.value, low, high, key);
  if (value === null) return;
  sendSerialCommand("setconfig", [key, String(value)]);
}

async function applyPidTuneGains() {
  const gains = readPidTuneGains();
  if (!gains) return null;

  try {
    await postSerialCommand("setconfig", ["pid_kp_milli", String(gains.kp)]);
    await postSerialCommand("setconfig", ["pid_ki_milli", String(gains.ki)]);
    await postSerialCommand("setconfig", ["pid_kd_milli", String(gains.kd)]);
    state.lastSerialConfig.pid_kp_milli = String(gains.kp);
    state.lastSerialConfig.pid_ki_milli = String(gains.ki);
    state.lastSerialConfig.pid_kd_milli = String(gains.kd);
    setPidTuneResult(`Gains applied ${gains.kp}/${gains.ki}/${gains.kd}`);
    return gains;
  } catch (error) {
    appendSerialLog("rx", `PID gain apply failed: ${error.message}`);
    setPidTuneResult("Gain apply failed");
    throw error;
  }
}

async function runPidTuneStepReturn() {
  if (state.pidTune.running) return;
  const gains = readPidTuneGains();
  const stepCounts = parseInteger(el.pidTuneStepCounts.value, "step counts");
  const pollMs = parseBoundedInt(el.pidTunePollMs.value, 50, 60000, "poll ms");
  const tolerance = parseBoundedInt(el.pidTuneTolerance.value, 0, 1000000, "settle tolerance");
  const timeoutMs = parseBoundedInt(el.pidTuneTimeoutMs.value, 100, 600000, "timeout ms");
  if (!gains || stepCounts === null || pollMs === null || tolerance === null || timeoutMs === null) return;
  const motorId = currentMotorId();
  const startedAt = Date.now();
  state.pidTune.running = true;
  state.pidTune.abort = false;
  state.pidTune.startPosition = null;
  state.pidTune.outboundTarget = null;
  setPidTuneResult("Starting");
  updatePidTuneButtons();

  try {
    await applyPidTuneGains();
    await postSerialCommand("positioncontrol", [motorId, "1"]);
    setPidTuneResult("Reading start position");
    const start = await getFreshPosition(motorId, pollMs, timeoutMs);
    state.pidTune.startPosition = start;
    state.pidTune.outboundTarget = start + stepCounts;
    renderPidTuneStatus();

    await postSerialCommand("setposition", [motorId, String(state.pidTune.outboundTarget)]);
    setPidTuneResult("Moving out");
    const outbound = await waitForPositionNear(motorId, state.pidTune.outboundTarget, tolerance, timeoutMs, pollMs);
    if (!outbound.ok) throw new Error(outbound.reason);

    await postSerialCommand("setposition", [motorId, String(start)]);
    setPidTuneResult("Returning");
    const returned = await waitForPositionNear(motorId, start, tolerance, timeoutMs, pollMs);
    if (!returned.ok) throw new Error(returned.reason);

    if (el.pidTuneDisableAfter.checked) {
      await postSerialCommand("positioncontrol", [motorId, "0"]);
    }
    appendPidTuneHistory({ gains, start, target: state.pidTune.outboundTarget, final: returned.position, result: "ok", elapsedMs: Date.now() - startedAt });
    setPidTuneResult("Complete");
  } catch (error) {
    await safeStopPidTune(motorId);
    const result = state.pidTune.abort ? "aborted" : error.message;
    appendPidTuneHistory({ gains, start: state.pidTune.startPosition, target: state.pidTune.outboundTarget, final: state.pidTune.lastPosition, result, elapsedMs: Date.now() - startedAt });
    setPidTuneResult(result);
  } finally {
    state.pidTune.running = false;
    state.pidTune.abort = false;
    updatePidTuneButtons();
  }
}

async function abortPidTune() {
  state.pidTune.abort = true;
  setPidTuneResult("Aborting");
  await safeStopPidTune(currentMotorId());
}

async function safeStopPidTune(motorId) {
  try {
    await postSerialCommand("stopmotor", [motorId]);
  } catch (error) {
    appendSerialLog("rx", `Safe stopmotor failed: ${error.message}`);
  }
  try {
    await postSerialCommand("positioncontrol", [motorId, "0"]);
  } catch (error) {
    appendSerialLog("rx", `Disable PID failed: ${error.message}`);
  }
}

async function getFreshPosition(motorId, pollMs, timeoutMs) {
  const startedAt = Date.now();
  state.latestPosition = null;
  while (Date.now() - startedAt < timeoutMs) {
    if (state.pidTune.abort) throw new Error("aborted");
    await postSerialCommand("getposition", [motorId]);
    await sleep(Math.max(50, pollMs));
    if (Number.isFinite(state.latestPosition)) {
      return state.latestPosition;
    }
  }
  throw new Error("position read timeout");
}

async function waitForPositionNear(motorId, target, tolerance, timeoutMs, pollMs) {
  const startedAt = Date.now();
  while (Date.now() - startedAt < timeoutMs) {
    if (state.pidTune.abort) {
      return { ok: false, reason: "aborted", position: state.pidTune.lastPosition };
    }
    await postSerialCommand("getposition", [motorId]);
    await sleep(Math.max(50, pollMs));
    const position = state.latestPosition;
    if (Number.isFinite(position)) {
      state.pidTune.lastPosition = position;
      renderPidTuneStatus();
      if (Math.abs(position - target) <= tolerance) {
        return { ok: true, position };
      }
    }
  }
  return { ok: false, reason: "timeout", position: state.pidTune.lastPosition };
}

function readPidTuneGains() {
  const kp = parseBoundedInt(el.pidTuneKpMilli.value, ...CONFIG_LIMITS.pid_kp_milli, "KP milli");
  const ki = parseBoundedInt(el.pidTuneKiMilli.value, ...CONFIG_LIMITS.pid_ki_milli, "KI milli");
  const kd = parseBoundedInt(el.pidTuneKdMilli.value, ...CONFIG_LIMITS.pid_kd_milli, "KD milli");
  if (kp === null || ki === null || kd === null) return null;
  return { kp, ki, kd };
}

function updateSerialConfigInput() {
  const key = el.serialConfigKey.value;
  const [low, high] = CONFIG_LIMITS[key];
  el.serialConfigValue.min = low;
  el.serialConfigValue.max = high;
  el.serialConfigValue.placeholder = `${low}-${high}`;
  el.serialConfigHint.textContent = `${key}: integer from ${low} to ${high}.`;

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

function renderSerialSnapshot(snapshot) {
  state.serialConnected = Boolean(snapshot.connected);
  state.lastSerialConfig = snapshot.config || {};
  el.serialPort.value = snapshot.port || "/dev/ttyACM0";
  el.serialBaud.value = snapshot.baud || 115200;
  setPill(el.serialStatus, state.serialConnected ? "Serial connected" : "Serial disconnected", state.serialConnected ? "good" : "bad");

  const motor = snapshot.motor || {};
  const position = snapshot.position || {};
  const sensors = snapshot.sensors || {};
  const status = snapshot.status || {};
  state.statusSummary = { ...state.statusSummary, ...status };
  if (Array.isArray(snapshot.commands)) {
    state.statusCommands = snapshot.commands;
  }
  if (Array.isArray(snapshot.motor_ids)) {
    state.statusMotorIds = snapshot.motor_ids;
  }
  const motorId = snapshot.motor_id || "M0";
  el.serialMotorId.textContent = motorId;
  if (document.activeElement !== el.serialMotorInput) {
    el.serialMotorInput.value = motorId;
  }

  el.serialJobState.textContent = snapshot.job_state || "UNKNOWN";
  el.serialReady.textContent = snapshot.ready ? "ready" : "not ready";
  el.serialErrorValue.textContent = snapshot.last_error || "No serial errors";
  el.serialStateCard.classList.toggle("error", Boolean(snapshot.last_error));
  el.serialMotorPwm.textContent = valueText(motor.pwm);
  el.serialDirectionValue.textContent = valueText(motor.direction);
  el.serialPositionValue.textContent = valueText(position.pos ?? position.position);
  el.serialTrayValue.textContent = trayText(sensors.has_tray);
  el.serialUpstreamSensor.textContent = sensorText(sensors.upstream);
  el.serialDownstreamSensor.textContent = sensorText(sensors.downstream);
  renderStatusSummary(state.statusSummary, state.statusMotorIds, state.statusCommands);
  const pidState = position.position_control;
  el.serialPositionControl.textContent = boolText(pidState);
  setPill(el.serialPidPill, `PID ${boolText(pidState).toLowerCase()}`, pidState === true ? "good" : pidState === false ? "muted" : "bad");

  if (snapshot.config && snapshot.config[el.serialConfigKey.value] !== undefined) {
    el.serialConfigValue.value = snapshot.config[el.serialConfigKey.value];
  }
  syncPidTuneInputs(snapshot.config || {});
  const latestPosition = position.pos ?? position.position;
  if (Number.isFinite(latestPosition)) {
    state.latestPosition = latestPosition;
    state.pidTune.lastPosition = latestPosition;
    renderPidTuneStatus();
  }

  if (!state.serialConnected) {
    stopPositionPoll();
  }

  document.querySelectorAll(".serial-command, #serialSetMotorButton, #serialStopMotorButton, #serialPositionControlOnButton, #serialPositionControlOffButton, #serialSetPositionButton, #serialSetOffsetButton, #serialSetConfigButton, #serialGetConfigButton, #serialResetConfigButton, #serialRawButton").forEach((button) => {
    button.disabled = !state.serialConnected;
  });
  updatePidTuneButtons();
  renderPositionPollState();
}

function renderStatusSummary(status, motorIds, commands) {
  el.statusAppName.textContent = valueText(status.app_name);
  el.statusMachineId.textContent = valueText(status.machine_id);
  el.statusTopicName.textContent = valueText(status.topic_name);
  el.statusMqttUri.textContent = valueText(status.mqtt_uri);
  el.statusMotorList.innerHTML = motorIds.length ? motorIds.map((motorId) => `<span class="chip">${escapeHtml(motorId)}</span>`).join("") : "-";
  el.statusCommandCount.textContent = `${commands.length} command${commands.length === 1 ? "" : "s"}`;
  el.statusCommandList.textContent = commands.length ? commands.join("\n") : "Run status to load firmware commands.";
}

function renderPositionPollState() {
  const running = Boolean(state.positionPollTimer);
  setPill(el.positionPollStatus, running ? `Position poll ${el.positionPollInterval.value || 100} ms` : "Position poll off", running ? "good" : "muted");
  el.positionPollToggleButton.textContent = running ? "Stop Poll" : "Start Poll";
  el.positionPollToggleButton.disabled = false;
}

function syncPidTuneInputs(config) {
  const fields = [
    ["pid_kp_milli", el.pidTuneKpMilli],
    ["pid_ki_milli", el.pidTuneKiMilli],
    ["pid_kd_milli", el.pidTuneKdMilli],
    ["position_tolerance_counts", el.pidTuneTolerance],
  ];
  fields.forEach(([key, input]) => {
    if (config[key] !== undefined && document.activeElement !== input && !state.pidTune.running) {
      input.value = config[key];
    }
  });
}

function updatePidTuneButtons() {
  const disabled = !state.serialConnected;
  el.pidTuneApplyButton.disabled = disabled || state.pidTune.running;
  el.pidTuneRunButton.disabled = disabled || state.pidTune.running;
  el.pidTuneAbortButton.disabled = disabled || !state.pidTune.running;
}

function renderPidTuneStatus() {
  el.pidTuneStartValue.textContent = valueText(state.pidTune.startPosition);
  el.pidTuneTargetValue.textContent = valueText(state.pidTune.outboundTarget);
  el.pidTuneCurrentValue.textContent = valueText(state.pidTune.lastPosition);
}

function setPidTuneResult(value) {
  el.pidTuneResultValue.textContent = value;
}

function appendPidTuneHistory(entry) {
  const line = [
    `kp=${entry.gains?.kp ?? "-"}`,
    `ki=${entry.gains?.ki ?? "-"}`,
    `kd=${entry.gains?.kd ?? "-"}`,
    `start=${entry.start ?? "-"}`,
    `target=${entry.target ?? "-"}`,
    `final=${entry.final ?? "-"}`,
    `result=${entry.result}`,
    `ms=${entry.elapsedMs}`,
  ].join(" ");
  state.pidTune.history.push(line);
  if (state.pidTune.history.length > 50) {
    state.pidTune.history = state.pidTune.history.slice(-50);
  }
  el.pidTuneHistory.textContent = state.pidTune.history.join("\n");
  el.pidTuneHistory.scrollTop = el.pidTuneHistory.scrollHeight;
}

function currentMotorId() {
  return el.serialMotorInput.value.trim() || "M0";
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

function boolText(value) {
  if (value === null || value === undefined) return "Unknown";
  return value ? "On" : "Off";
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
  if (state.serialLogLines.length > 1000) {
    state.serialLogLines = state.serialLogLines.slice(-1000);
  }
  renderSerialLog();
}

function renderSerialLog() {
  el.serialLogOutput.textContent = state.serialLogLines.join("\n");
  el.serialLogOutput.scrollTop = el.serialLogOutput.scrollHeight;
}

start();
