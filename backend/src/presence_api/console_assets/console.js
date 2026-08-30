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
};

let snapshot = null;
let pendingConfig = null;
let refreshTimer = null;
let snapshotController = null;
let configDirty = false;
let configEditBase = null;

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
    setConnection(error.status === 403 ? "Outside LAN" : "Unavailable", "offline");
    setMessage(error.status === 403 ? "This console is available only on the home LAN." : error.message, "error");
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
    snapshot = await apiFetch(
      `/v1/console/devices/${deviceId}/snapshot?hours=${hours}&max_points=720`,
      { signal: controller.signal },
    );
    renderSnapshot(snapshot);
    setMessage(`Updated ${formatEastern(snapshot.server_utc_ms, true)} Eastern`, "success");
  } catch (error) {
    if (error.name === "AbortError") return;
    setConnection(error.status === 403 ? "Outside LAN" : "Unavailable", "offline");
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
