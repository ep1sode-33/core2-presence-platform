"use strict";

const INTEGER_CONFIG_FIELDS = new Set([
  "minimum_on_ms",
  "pir_hold_ms",
  "sound_hold_ms",
  "max_sound_bridge_ms",
  "cooldown_ms",
  "telemetry_interval_ms",
  "upload_batch_size",
]);

const elements = {
  device: document.querySelector("#deviceSelect"),
  hours: document.querySelector("#hoursSelect"),
  refresh: document.querySelector("#refreshButton"),
  message: document.querySelector("#message"),
  connection: document.querySelector("#connectionState"),
  canvas: document.querySelector("#timelineCanvas"),
  caption: document.querySelector("#chartCaption"),
  form: document.querySelector("#configForm"),
  review: document.querySelector("#reviewButton"),
  dialog: document.querySelector("#confirmDialog"),
  changeList: document.querySelector("#changeList"),
  confirm: document.querySelector("#confirmCheckbox"),
  apply: document.querySelector("#applyButton"),
  releaseSelect: document.querySelector("#releaseSelect"),
  reviewRelease: document.querySelector("#reviewReleaseButton"),
  releaseBundle: document.querySelector("#releaseBundle"),
  importRelease: document.querySelector("#importReleaseButton"),
  releaseMessage: document.querySelector("#releaseMessage"),
  releaseDialog: document.querySelector("#releaseDialog"),
  releaseReviewText: document.querySelector("#releaseReviewText"),
  releaseConfirm: document.querySelector("#releaseConfirmCheckbox"),
  applyRelease: document.querySelector("#applyReleaseButton"),
  commandAction: document.querySelector("#commandAction"),
  issueCommand: document.querySelector("#issueCommandButton"),
};

const view = {
  presence: document.querySelector("#presenceValue"),
  latestDetail: document.querySelector("#latestDetail"),
  lastSeen: document.querySelector("#lastSeenValue"),
  lastSeenDetail: document.querySelector("#lastSeenDetail"),
  firmware: document.querySelector("#firmwareValue"),
  deviceId: document.querySelector("#deviceIdValue"),
  revision: document.querySelector("#revisionValue"),
  revisionDetail: document.querySelector("#revisionDetail"),
  statusCard: document.querySelector(".status-card"),
  sampleCount: document.querySelector("#sampleCount"),
  presentFraction: document.querySelector("#presentFraction"),
  pirFraction: document.querySelector("#pirFraction"),
  soundFraction: document.querySelector("#soundFraction"),
  micMean: document.querySelector("#micMean"),
  noiseMean: document.querySelector("#noiseMean"),
  thresholdMean: document.querySelector("#thresholdMean"),
  thresholdRatio: document.querySelector("#thresholdRatio"),
  mismatchCount: document.querySelector("#mismatchCount"),
  feedbackCount: document.querySelector("#feedbackCount"),
  feedbackBody: document.querySelector("#feedbackBody"),
  configRevision: document.querySelector("#configRevision"),
  healthLevel: document.querySelector("#healthLevel"),
  healthConnectivity: document.querySelector("#healthConnectivity"),
  healthWifi: document.querySelector("#healthWifi"),
  healthBuild: document.querySelector("#healthBuild"),
  healthTasks: document.querySelector("#healthTasks"),
  healthQueues: document.querySelector("#healthQueues"),
  healthStorage: document.querySelector("#healthStorage"),
  healthMemory: document.querySelector("#healthMemory"),
  healthSensors: document.querySelector("#healthSensors"),
  releasePhase: document.querySelector("#releasePhase"),
  releaseDesired: document.querySelector("#releaseDesired"),
  releaseRunning: document.querySelector("#releaseRunning"),
  releasePrevious: document.querySelector("#releasePrevious"),
  releaseGood: document.querySelector("#releaseGood"),
  releaseProgress: document.querySelector("#releaseProgress"),
  releaseError: document.querySelector("#releaseError"),
  transitionBody: document.querySelector("#transitionBody"),
  logCount: document.querySelector("#logCount"),
  logBody: document.querySelector("#logBody"),
  crashCount: document.querySelector("#crashCount"),
  crashBody: document.querySelector("#crashBody"),
  commandBody: document.querySelector("#commandBody"),
};

let snapshot = null;
let pendingConfig = null;
let refreshTimer = null;
let snapshotController = null;
let configDirty = false;
let configEditBase = null;
let operationsData = null;
let pendingRelease = null;

function setMessage(text, tone = "neutral") {
  elements.message.textContent = text;
  elements.message.dataset.tone = tone;
}

function setConnection(text, tone) {
  elements.connection.textContent = text;
  elements.connection.dataset.tone = tone;
}

function requestHeaders(includeJson = false) {
  const headers = new Headers();
  if (includeJson) headers.set("Content-Type", "application/json");
  return headers;
}

async function apiFetch(path, options = {}) {
  const response = await fetch(path, {
    ...options,
    headers: options.headers || requestHeaders(Boolean(options.body)),
    cache: "no-store",
  });
  if (!response.ok) {
    let detail = `${response.status} ${response.statusText}`;
    try {
      const body = await response.json();
      if (body.detail) detail = body.detail;
    } catch (_error) {
      // Preserve the HTTP status if the body is not JSON.
    }
    const error = new Error(detail);
    error.status = response.status;
    throw error;
  }
  return response.json();
}

function formatAge(milliseconds) {
  if (!Number.isFinite(milliseconds)) return "—";
  if (milliseconds < 1_000) return "now";
  const seconds = Math.floor(milliseconds / 1_000);
  if (seconds < 60) return `${seconds}s ago`;
  const minutes = Math.floor(seconds / 60);
  if (minutes < 60) return `${minutes}m ago`;
  const hours = Math.floor(minutes / 60);
  if (hours < 48) return `${hours}h ago`;
  return `${Math.floor(hours / 24)}d ago`;
}

function formatEastern(milliseconds, withDate = false) {
  const options = withDate
    ? { month: "short", day: "numeric", hour: "2-digit", minute: "2-digit", second: "2-digit" }
    : { hour: "2-digit", minute: "2-digit" };
  return new Intl.DateTimeFormat("en-US", {
    ...options,
    timeZone: "America/New_York",
    hour12: false,
  }).format(new Date(milliseconds));
}

function formatPercent(value) {
  return value == null ? "—" : `${(value * 100).toFixed(1)}%`;
}

function formatNumber(value, digits = 1) {
  return value == null ? "—" : Number(value).toFixed(digits);
}

function fillDeviceList(devices) {
  const previous = elements.device.value;
  elements.device.replaceChildren();
  if (!devices.length) {
    const option = new Option("No devices found", "");
    elements.device.add(option);
    elements.device.disabled = true;
    elements.refresh.disabled = true;
    return;
  }
  for (const device of devices) {
    const suffix = device.online ? "online" : formatAge(device.last_seen_age_ms);
    elements.device.add(new Option(`${device.device_id} · ${suffix}`, device.device_id));
  }
  if (devices.some((device) => device.device_id === previous)) {
    elements.device.value = previous;
  }
  elements.device.disabled = false;
  elements.refresh.disabled = false;
}

async function connect() {
  configDirty = false;
  configEditBase = null;
  setConnection("Connecting", "loading");
  setMessage("Loading devices…");
  try {
    const data = await apiFetch("/v1/console/devices");
    fillDeviceList(data.items);
    if (!data.items.length) {
      setConnection("Connected", "neutral");
      setMessage("Connected, but no device has uploaded telemetry yet.");
      return;
    }
    await loadSnapshot();
    startRefreshTimer();
  } catch (error) {
    stopRefreshTimer();
    setConnection(error.status === 403 ? "Access denied" : "Unavailable", "offline");
    setMessage(error.status === 403 ? "This console is available only from an approved LAN or Tailnet device." : error.message, "error");
  }
}

async function loadSnapshot({ quiet = false } = {}) {
  if (!elements.device.value) return;
  if (snapshotController) snapshotController.abort();
  const controller = new AbortController();
  snapshotController = controller;
  if (!quiet) setMessage("Loading telemetry…");
  try {
    const deviceId = encodeURIComponent(elements.device.value);
    const hours = encodeURIComponent(elements.hours.value);
    const paths = [
      `/v1/console/devices/${deviceId}/snapshot?hours=${hours}&max_points=720`,
      `/v1/console/devices/${deviceId}/health?limit=120`,
      `/v1/console/devices/${deviceId}/release-status`,
      `/v1/console/devices/${deviceId}/logs?limit=200`,
      `/v1/console/devices/${deviceId}/coredumps?limit=10`,
      "/v1/console/releases",
      `/v1/console/devices/${deviceId}/commands?limit=50`,
    ];
    const results = await Promise.all(
      paths.map((path) => apiFetch(path, { signal: controller.signal })),
    );
    [snapshot] = results;
    operationsData = {
      health: results[1],
      release: results[2],
      logs: results[3],
      crashes: results[4],
      releases: results[5],
      commands: results[6],
    };
    renderSnapshot(snapshot);
    renderOperations(operationsData);
    setMessage(`Updated ${formatEastern(snapshot.server_utc_ms, true)} Eastern`, "success");
  } catch (error) {
    if (error.name === "AbortError") return;
    setConnection(error.status === 403 ? "Access denied" : "Unavailable", "offline");
    setMessage(error.message, "error");
    if (error.status === 403) stopRefreshTimer();
  } finally {
    if (snapshotController === controller) snapshotController = null;
  }
}

function renderSnapshot(data) {
  const { device, latest, calibration, window: timeWindow } = data;
  setConnection(device.online ? "Device online" : "Device offline", device.online ? "online" : "offline");
  view.presence.textContent = latest ? latest.state.toUpperCase() : "NO DATA";
  view.statusCard.dataset.state = latest ? latest.state : "unknown";
  view.latestDetail.textContent = latest
    ? `PIR ${latest.pir ? "active" : "quiet"} · sound ${latest.sound_active ? "active" : "quiet"}`
    : "No sample";
  view.lastSeen.textContent = formatAge(device.last_seen_age_ms);
  view.lastSeenDetail.textContent = device.last_seen_at_ms == null
    ? "No telemetry received"
    : formatEastern(device.last_seen_at_ms, true) + " Eastern";
  view.firmware.textContent = device.firmware_version || "Unknown";
  view.deviceId.textContent = device.device_id;
  const reportedRevision = device.latest_reported_config_revision == null
    ? "?"
    : device.latest_reported_config_revision;
  view.revision.textContent = `${device.desired_config_revision} / ${reportedRevision}`;
  view.revisionDetail.textContent = `${device.config_sync.replace("_", " ")} · desired / latest reported · highest seen ${device.highest_applied_config_revision}`;
  view.sampleCount.textContent = timeWindow.sample_count.toLocaleString();
  view.presentFraction.textContent = formatPercent(calibration.present_fraction);
  view.pirFraction.textContent = formatPercent(calibration.pir_active_fraction);
  view.soundFraction.textContent = formatPercent(calibration.sound_active_fraction);
  view.micMean.textContent = formatNumber(calibration.mic_envelope_mean);
  view.noiseMean.textContent = formatNumber(calibration.noise_floor_mean);
  view.thresholdMean.textContent = formatNumber(calibration.sound_threshold_mean);
  view.thresholdRatio.textContent = formatNumber(calibration.threshold_to_noise_ratio, 3);
  view.mismatchCount.textContent = String(calibration.feedback.mismatch);
  view.feedbackCount.textContent = `${timeWindow.feedback_count} labels`;
  view.configRevision.textContent = configDirty && configEditBase
    ? `Revision ${data.config.revision} · editing from ${configEditBase.revision}`
    : `Revision ${data.config.revision}`;
  if (!configDirty) fillConfig(data.config, device.device_id);
  renderFeedback(data.feedback);
  renderTransitions(data.transitions);
  drawTimeline();
  const bucket = Math.round(timeWindow.bucket_ms / 1000);
  elements.caption.textContent = timeWindow.sample_count
    ? `${timeWindow.sample_count.toLocaleString()} samples · ${timeWindow.returned_points.toLocaleString()} chart buckets at about ${bucket}s each · observed time with receive-time fallback`
    : "No telemetry samples in this window.";
}

function fillConfig(configResponse, deviceId) {
  for (const [name, value] of Object.entries(configResponse.config)) {
    const input = elements.form.elements.namedItem(name);
    if (input) input.value = String(value);
  }
  configEditBase = {
    deviceId,
    revision: configResponse.revision,
    config: { ...configResponse.config },
  };
  configDirty = false;
  elements.review.disabled = false;
}

function readConfig() {
  const values = {};
  for (const [name] of Object.entries(configEditBase.config)) {
    const input = elements.form.elements.namedItem(name);
    values[name] = INTEGER_CONFIG_FIELDS.has(name)
      ? Number.parseInt(input.value, 10)
      : Number.parseFloat(input.value);
  }
  return values;
}

function reviewConfig(event) {
  event.preventDefault();
  if (!snapshot || !configEditBase || !elements.form.reportValidity()) return;
  if (
    snapshot.device.device_id !== configEditBase.deviceId
    || snapshot.config.revision !== configEditBase.revision
  ) {
    fillConfig(snapshot.config, snapshot.device.device_id);
    setMessage("Configuration changed while you were editing. Current values were reloaded; review your change again.", "error");
    return;
  }
  const reviewedConfig = readConfig();
  const changes = Object.entries(reviewedConfig).filter(
    ([name, value]) => value !== configEditBase.config[name],
  );
  elements.changeList.replaceChildren();
  if (!changes.length) {
    configDirty = false;
    setMessage("No configuration values changed.");
    return;
  }
  for (const [name, value] of changes) {
    const item = document.createElement("li");
    item.textContent = `${name}: ${configEditBase.config[name]} → ${value}`;
    elements.changeList.append(item);
  }
  pendingConfig = {
    deviceId: configEditBase.deviceId,
    baseRevision: configEditBase.revision,
    config: reviewedConfig,
  };
  stopRefreshTimer();
  if (snapshotController) snapshotController.abort();
  elements.confirm.checked = false;
  elements.apply.disabled = true;
  elements.dialog.showModal();
}

async function applyConfig() {
  if (!elements.confirm.checked || !pendingConfig || !snapshot) return;
  const reviewed = pendingConfig;
  elements.apply.disabled = true;
  try {
    const deviceId = encodeURIComponent(reviewed.deviceId);
    await apiFetch(`/v1/console/devices/${deviceId}/config`, {
      method: "PUT",
      headers: requestHeaders(true),
      body: JSON.stringify({
        base_revision: reviewed.baseRevision,
        created_by: "presence-console",
        config: reviewed.config,
      }),
    });
    elements.dialog.close();
    configDirty = false;
    configEditBase = null;
    await loadSnapshot({ quiet: true });
    setMessage("Configuration revision issued; waiting for device acknowledgement.", "success");
  } catch (error) {
    elements.dialog.close();
    if (error.status === 409) {
      configDirty = false;
      configEditBase = null;
      await loadSnapshot({ quiet: true });
      setMessage("Configuration changed elsewhere. Refreshed current values; review again.", "error");
    } else {
      setMessage(error.message, "error");
    }
  } finally {
    pendingConfig = null;
  }
}

function setReleaseMessage(text, tone = "neutral") {
  elements.releaseMessage.textContent = text;
  elements.releaseMessage.dataset.tone = tone;
}

async function fileToBase64(file) {
  const bytes = new Uint8Array(await file.arrayBuffer());
  const chunks = [];
  for (let offset = 0; offset < bytes.length; offset += 0x8000) {
    chunks.push(String.fromCharCode(...bytes.subarray(offset, offset + 0x8000)));
  }
  return window.btoa(chunks.join(""));
}

async function importReleaseBundle() {
  const [file] = elements.releaseBundle.files;
  if (!file) {
    setReleaseMessage("Choose the canonical .ota.zip bundle first.", "error");
    return;
  }
  elements.importRelease.disabled = true;
  setReleaseMessage(`Reading and verifying ${file.name}…`);
  try {
    const imported = await apiFetch("/v1/console/releases/import", {
      method: "POST",
      headers: requestHeaders(true),
      body: JSON.stringify({
        bundle_base64: await fileToBase64(file),
        imported_by: "presence-console",
      }),
    });
    elements.releaseBundle.value = "";
    await loadSnapshot({ quiet: true });
    elements.releaseSelect.value = imported.release_id;
    elements.reviewRelease.disabled = false;
    setReleaseMessage(`Verified ${imported.firmware_version} counter ${imported.release_counter}.`, "success");
  } catch (error) {
    setReleaseMessage(error.message, "error");
  } finally {
    elements.importRelease.disabled = false;
  }
}

function reviewRelease() {
  if (!operationsData || !elements.releaseSelect.value) return;
  const release = operationsData.releases.find(
    (item) => item.release_id === elements.releaseSelect.value,
  );
  if (!release) return;
  pendingRelease = release;
  elements.releaseReviewText.textContent = `${snapshot.device.device_id} will request ${release.firmware_version}, release counter ${release.release_counter}, build ${release.build_id}, for ${release.hardware_model}. SHA-256 ${release.image_sha256}.`;
  elements.releaseConfirm.checked = false;
  elements.applyRelease.disabled = true;
  stopRefreshTimer();
  elements.releaseDialog.showModal();
}

async function applyRelease() {
  if (!pendingRelease || !elements.releaseConfirm.checked || !snapshot) return;
  elements.applyRelease.disabled = true;
  try {
    const deviceId = encodeURIComponent(snapshot.device.device_id);
    await apiFetch(`/v1/console/devices/${deviceId}/release`, {
      method: "PUT",
      headers: requestHeaders(true),
      body: JSON.stringify({
        release_id: pendingRelease.release_id,
        selected_by: "presence-console",
      }),
    });
    elements.releaseDialog.close();
    await loadSnapshot({ quiet: true });
    setReleaseMessage("Desired release selected; the device will fetch it on its next authenticated poll.", "success");
  } catch (error) {
    elements.releaseDialog.close();
    setReleaseMessage(error.message, "error");
  }
}

async function issueCommand() {
  if (!snapshot) return;
  const action = elements.commandAction.value;
  if (!window.confirm(`Issue one-shot command “${action}” to ${snapshot.device.device_id}?`)) return;
  const command = { action };
  if (action === "set_log_level") {
    command.level = "debug_sensor";
    command.duration_seconds = 600;
  } else if (action === "open_dev_ota") {
    command.requires_local_confirmation = true;
  }
  elements.issueCommand.disabled = true;
  try {
    const deviceId = encodeURIComponent(snapshot.device.device_id);
    await apiFetch(`/v1/console/devices/${deviceId}/commands`, {
      method: "POST",
      headers: requestHeaders(true),
      body: JSON.stringify({
        command,
        expires_in_seconds: 600,
        created_by: "presence-console",
      }),
    });
    await loadSnapshot({ quiet: true });
    setMessage(`Command ${action} queued.`, "success");
  } catch (error) {
    setMessage(error.message, "error");
  } finally {
    elements.issueCommand.disabled = false;
  }
}

function renderFeedback(feedback) {
  view.feedbackBody.replaceChildren();
  if (!feedback.length) {
    const row = document.createElement("tr");
    const cell = document.createElement("td");
    cell.colSpan = 5;
    cell.className = "empty-cell";
    cell.textContent = "No feedback in window";
    row.append(cell);
    view.feedbackBody.append(row);
    return;
  }
  for (const item of [...feedback].reverse().slice(0, 100)) {
    const row = document.createElement("tr");
    const mismatch = item.observed_state != null
      && (item.actual_presence === "present") !== (item.observed_state === "present");
    row.dataset.mismatch = String(mismatch);
    const values = [
      formatEastern(item.marker_ms, true),
      item.actual_presence,
      item.observed_state || "—",
      item.source,
      item.note || "—",
    ];
    for (const value of values) {
      const cell = document.createElement("td");
      cell.textContent = value;
      row.append(cell);
    }
    view.feedbackBody.append(row);
  }
}

function emptyTable(body, columns, message) {
  body.replaceChildren();
  const row = document.createElement("tr");
  const cell = document.createElement("td");
  cell.colSpan = columns;
  cell.className = "empty-cell";
  cell.textContent = message;
  row.append(cell);
  body.append(row);
}

function appendCells(body, values, attributes = {}) {
  const row = document.createElement("tr");
  for (const [name, value] of Object.entries(attributes)) row.dataset[name] = value;
  for (const value of values) {
    const cell = document.createElement("td");
    cell.textContent = value;
    row.append(cell);
  }
  body.append(row);
}

function renderTransitions(transitions) {
  if (!transitions.length) {
    emptyTable(view.transitionBody, 8, "No transitions in window");
    return;
  }
  view.transitionBody.replaceChildren();
  for (const item of [...transitions].reverse().slice(0, 200)) {
    const pirAge = item.pir_age_ms == null ? "?" : `${item.pir_age_ms}ms ago`;
    const soundAge = item.sound_age_ms == null ? "?" : `${item.sound_age_ms}ms ago`;
    const mic = item.mic_envelope == null
      ? "legacy record"
      : `${formatNumber(item.mic_envelope)} / ${formatNumber(item.sound_threshold)} (noise ${formatNumber(item.noise_floor)})`;
    appendCells(view.transitionBody, [
      formatEastern(item.marker_ms, true),
      `${item.from_state || "boot"} → ${item.to_state}`,
      item.reason,
      item.pir == null ? "legacy record" : `${item.pir ? "active" : "quiet"} · ${pirAge}`,
      item.sound_active == null ? "legacy record" : `${item.sound_active ? "active" : "quiet"} · ${soundAge}`,
      mic,
      item.brightness_before == null ? "legacy record" : `${item.brightness_before} → ${item.brightness_after}`,
      `${item.build_id || "unknown"} · config ${item.applied_config_revision ?? "?"}`,
    ]);
  }
}

function releaseLabel(releaseId, releases) {
  if (!releaseId) return "—";
  const release = releases.find((item) => item.release_id === releaseId);
  return release ? `${release.firmware_version} · #${release.release_counter}` : releaseId;
}

function renderOperations(data) {
  const health = data.health;
  const latest = health.latest;
  view.healthConnectivity.textContent = health.server_online
    ? `online · activity ${formatAge(snapshot.device.last_seen_age_ms)}`
    : "offline by server observation";
  view.healthLevel.textContent = latest ? latest.level.toUpperCase() : "UNKNOWN";
  view.healthLevel.dataset.level = latest ? latest.level : "unknown";
  if (latest) {
    view.healthWifi.textContent = latest.wifi.connected
      ? `${latest.wifi.ip || "no IP"} · ${latest.wifi.rssi_dbm ?? "?"} dBm · ${latest.wifi.reconnect_count} reconnects`
      : "disconnected";
    view.healthBuild.textContent = `${latest.firmware_version} · ${latest.build_id} · ${latest.reset_reason}`;
    view.healthTasks.textContent = `main ${latest.tasks.main_heartbeat_ms}ms · uploader ${latest.tasks.uploader_heartbeat_ms}ms`;
    view.healthQueues.textContent = `telemetry ${latest.queues.telemetry_depth}/${latest.queues.telemetry_capacity} · feedback ${latest.queues.feedback_depth}/${latest.queues.feedback_capacity}`;
    view.healthStorage.textContent = `${latest.storage.spool_files} spool · ${latest.storage.dead_files} dead · ${latest.storage.littlefs_free_bytes.toLocaleString()} B free`;
    view.healthMemory.textContent = `${latest.memory.free_heap_bytes.toLocaleString()} B free · minimum ${latest.memory.min_free_heap_bytes.toLocaleString()} B`;
    view.healthSensors.textContent = `PIR ${latest.sensors.pir_status} · mic ${latest.sensors.mic_status}${latest.sensors.pir_only_mode ? " · PIR-only" : ""}`;
  } else {
    for (const node of [view.healthWifi, view.healthBuild, view.healthTasks, view.healthQueues, view.healthStorage, view.healthMemory, view.healthSensors]) node.textContent = "No health report";
  }

  const releases = data.releases;
  const previousSelection = elements.releaseSelect.value;
  elements.releaseSelect.replaceChildren();
  if (!releases.length) {
    elements.releaseSelect.add(new Option("No releases imported", ""));
    elements.reviewRelease.disabled = true;
  } else {
    elements.releaseSelect.add(new Option("Choose verified release…", ""));
    for (const release of releases) {
      elements.releaseSelect.add(new Option(
        `${release.firmware_version} · counter ${release.release_counter} · ${release.build_id}`,
        release.release_id,
      ));
    }
    if (releases.some((release) => release.release_id === previousSelection)) {
      elements.releaseSelect.value = previousSelection;
    }
    elements.reviewRelease.disabled = !elements.releaseSelect.value;
  }
  const target = data.release.target;
  const status = data.release.latest_status;
  view.releaseDesired.textContent = target ? releaseLabel(target.release.release_id, releases) : "—";
  view.releaseRunning.textContent = releaseLabel(status?.running_release_id, releases);
  view.releasePrevious.textContent = releaseLabel(status?.previous_release_id, releases);
  view.releaseGood.textContent = releaseLabel(status?.last_known_good_release_id, releases);
  view.releasePhase.textContent = status ? status.phase.toUpperCase() : "IDLE";
  view.releaseProgress.textContent = status
    ? `${status.progress_percent ?? "—"}% · rollback ${status.rollback_outcome}`
    : "—";
  view.releaseError.textContent = status?.last_error || "—";

  view.logCount.textContent = `${data.logs.retained_records} retained`;
  if (!data.logs.items.length) emptyTable(view.logBody, 4, "No operational events");
  else {
    view.logBody.replaceChildren();
    for (const item of data.logs.items) appendCells(view.logBody, [
      formatEastern(item.received_at_ms, true),
      item.level,
      item.event_type,
      JSON.stringify(item.fields),
    ], { level: item.level });
  }

  view.crashCount.textContent = `${data.crashes.retained_reports} retained`;
  if (!data.crashes.items.length) emptyTable(view.crashBody, 4, "No crash reports");
  else {
    view.crashBody.replaceChildren();
    for (const item of data.crashes.items) appendCells(view.crashBody, [
      formatEastern(item.received_at_ms, true),
      item.reset_reason,
      item.build_id,
      `${item.symbolication_status}: ${item.summary.join(" · ")}`,
    ]);
  }

  if (!data.commands.length) emptyTable(view.commandBody, 5, "No commands");
  else {
    view.commandBody.replaceChildren();
    for (const item of data.commands) appendCells(view.commandBody, [
      formatEastern(item.created_at_ms, true),
      item.command.action,
      item.status,
      String(item.delivery_attempts),
      item.latest_result ? JSON.stringify(item.latest_result) : "—",
    ], { status: item.status });
  }
}

function drawTimeline() {
  const canvas = elements.canvas;
  const bounds = canvas.getBoundingClientRect();
  const ratio = Math.min(window.devicePixelRatio || 1, 2);
  canvas.width = Math.max(1, Math.round(bounds.width * ratio));
  canvas.height = Math.max(1, Math.round(bounds.height * ratio));
  const context = canvas.getContext("2d");
  context.scale(ratio, ratio);
  const width = bounds.width;
  const height = bounds.height;
  const padding = { left: 48, right: 18, top: 18, bottom: 30 };
  const plotWidth = Math.max(1, width - padding.left - padding.right);
  const plotHeight = Math.max(1, height - padding.top - padding.bottom);
  context.clearRect(0, 0, width, height);
  context.fillStyle = "#0c151e";
  context.fillRect(padding.left, padding.top, plotWidth, plotHeight);
  if (!snapshot || !snapshot.series.length) {
    context.fillStyle = "#91a2b2";
    context.font = "13px system-ui";
    context.textAlign = "center";
    context.fillText("No samples in this window", width / 2, height / 2);
    return;
  }

  const start = snapshot.window.start_ms;
  const end = snapshot.window.end_ms;
  const x = (value) => padding.left + ((value - start) / (end - start)) * plotWidth;
  const maxSignal = Math.max(
    1,
    ...snapshot.series.flatMap((point) => [point.mic_envelope_max, point.sound_threshold_mean]),
  ) * 1.08;
  const y = (value) => padding.top + plotHeight - (value / maxSignal) * plotHeight;

  context.strokeStyle = "#263747";
  context.lineWidth = 1;
  context.fillStyle = "#91a2b2";
  context.font = "11px system-ui";
  for (let index = 0; index <= 4; index += 1) {
    const lineY = padding.top + (plotHeight * index) / 4;
    context.beginPath();
    context.moveTo(padding.left, lineY);
    context.lineTo(width - padding.right, lineY);
    context.stroke();
    const value = maxSignal * (1 - index / 4);
    context.textAlign = "right";
    context.fillText(value.toFixed(0), padding.left - 7, lineY + 4);
  }

  for (const point of snapshot.series) {
    const left = x(point.start_ms);
    const right = Math.max(left + 1, x(point.end_ms + snapshot.window.bucket_ms));
    if (point.present_fraction > 0) {
      context.fillStyle = `rgba(86, 210, 140, ${0.08 + point.present_fraction * 0.18})`;
      context.fillRect(left, padding.top, right - left, plotHeight);
    }
    if (point.pir_active_fraction > 0) {
      context.fillStyle = "#ff6b6b";
      context.fillRect(left, padding.top, Math.max(2, right - left), 3);
    }
  }

  function line(key, color, dashed = false, widthValue = 1.5) {
    context.beginPath();
    context.strokeStyle = color;
    context.lineWidth = widthValue;
    context.setLineDash(dashed ? [6, 5] : []);
    snapshot.series.forEach((point, index) => {
      const pointX = x((point.start_ms + point.end_ms) / 2);
      const pointY = y(point[key]);
      if (index === 0) context.moveTo(pointX, pointY);
      else context.lineTo(pointX, pointY);
    });
    context.stroke();
    context.setLineDash([]);
  }
  line("noise_floor_mean", "#778898", false, 1);
  line("sound_threshold_mean", "#5fd3e8", true, 1.4);
  line("mic_envelope_mean", "#ffb44a", false, 2);

  for (const marker of snapshot.feedback) {
    const markerX = x(marker.marker_ms);
    context.strokeStyle = marker.actual_presence === "present" ? "#56d28c" : "#ff6b6b";
    context.lineWidth = 2;
    context.beginPath();
    context.moveTo(markerX, padding.top);
    context.lineTo(markerX, padding.top + plotHeight);
    context.stroke();
  }

  context.fillStyle = "#91a2b2";
  context.font = "11px system-ui";
  for (let index = 0; index <= 4; index += 1) {
    const timestamp = start + ((end - start) * index) / 4;
    context.textAlign = index === 0 ? "left" : index === 4 ? "right" : "center";
    context.fillText(formatEastern(timestamp), x(timestamp), height - 8);
  }
}

function startRefreshTimer() {
  stopRefreshTimer();
  refreshTimer = window.setInterval(() => loadSnapshot({ quiet: true }), 10_000);
}

function stopRefreshTimer() {
  if (refreshTimer != null) window.clearInterval(refreshTimer);
  refreshTimer = null;
}

elements.refresh.addEventListener("click", () => loadSnapshot());
elements.device.addEventListener("change", () => {
  configDirty = false;
  configEditBase = null;
  loadSnapshot();
});
elements.hours.addEventListener("change", () => loadSnapshot());
elements.form.addEventListener("input", () => {
  if (configEditBase) configDirty = true;
});
elements.form.addEventListener("submit", reviewConfig);
elements.confirm.addEventListener("change", () => {
  elements.apply.disabled = !elements.confirm.checked;
});
elements.apply.addEventListener("click", applyConfig);
elements.dialog.addEventListener("close", () => {
  pendingConfig = null;
  if (elements.device.value) startRefreshTimer();
});
elements.releaseSelect.addEventListener("change", () => {
  elements.reviewRelease.disabled = !elements.releaseSelect.value;
});
elements.reviewRelease.addEventListener("click", reviewRelease);
elements.importRelease.addEventListener("click", importReleaseBundle);
elements.releaseConfirm.addEventListener("change", () => {
  elements.applyRelease.disabled = !elements.releaseConfirm.checked;
});
elements.applyRelease.addEventListener("click", applyRelease);
elements.releaseDialog.addEventListener("close", () => {
  pendingRelease = null;
  if (elements.device.value) startRefreshTimer();
});
elements.issueCommand.addEventListener("click", issueCommand);
window.addEventListener("resize", () => drawTimeline());
document.addEventListener("visibilitychange", () => {
  if (document.hidden) {
    stopRefreshTimer();
    if (snapshotController) snapshotController.abort();
  }
  else if (elements.device.value) {
    loadSnapshot({ quiet: true });
    startRefreshTimer();
  }
});

connect();
