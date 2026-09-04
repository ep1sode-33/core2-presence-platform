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

const UI_LABELS = Object.freeze({
  configField: Object.freeze({
    minimum_on_ms: "最短亮屏时间（毫秒）",
    pir_hold_ms: "PIR 保持时间（毫秒）",
    sound_hold_ms: "声音保持时间（毫秒）",
    max_sound_bridge_ms: "最大声音续接时间（毫秒）",
    cooldown_ms: "冷却时间（毫秒）",
    sound_factor: "声音阈值系数",
    telemetry_interval_ms: "遥测间隔（毫秒）",
    upload_batch_size: "上传批量大小",
  }),
  presenceState: Object.freeze({
    calibrating: "校准中",
    idle: "无人",
    present: "有人",
    cooldown: "冷却中",
  }),
  transitionReason: Object.freeze({
    boot: "启动",
    calibration_complete: "校准完成",
    pir_motion: "PIR 检测到动作",
    sound_bridge: "声音桥接延续",
    quiet_timeout: "安静超时",
    cooldown_timeout: "冷却超时",
    touch_wake: "触摸唤醒",
    bench_override: "台架测试覆盖",
    config_change: "配置变更",
    unknown: "未知",
  }),
  configSync: Object.freeze({
    unknown: "未知",
    in_sync: "已同步",
    pending: "待同步",
    regressed: "版本回退",
    divergent: "状态不一致",
  }),
  healthLevel: Object.freeze({
    healthy: "正常",
    degraded: "降级",
    action_required: "需要处理",
    unknown: "未知",
  }),
  sensorHealth: Object.freeze({
    healthy: "正常",
    degraded: "降级",
    fault: "故障",
    unknown: "未知",
  }),
  releasePhase: Object.freeze({
    idle: "空闲",
    downloading: "下载中",
    verifying: "验证中",
    reboot_pending: "等待重启",
    validating: "启动验证中",
    running: "运行中",
    failed: "失败",
    rejected: "已拒绝",
    rolled_back: "已回滚",
  }),
  rollbackOutcome: Object.freeze({
    none: "无",
    not_needed: "无需回滚",
    succeeded: "回滚成功",
    failed: "回滚失败",
    unknown: "未知",
  }),
  commandAction: Object.freeze({
    diagnostic_snapshot: "捕获诊断快照",
    set_log_level: "开启 10 分钟详细传感器日志",
    recalibrate_microphone: "重新校准麦克风",
    retry_upload: "立即重试上传",
    reboot: "重启应用",
    open_dev_ota: "请求需本机确认的开发 OTA 窗口",
  }),
  commandStatus: Object.freeze({
    queued: "排队中",
    leased: "已租约下发",
    accepted: "已接受",
    running: "执行中",
    succeeded: "成功",
    failed: "失败",
    expired: "已过期",
    rejected: "已拒绝",
  }),
  feedbackPresence: Object.freeze({
    present: "有人",
    absent: "无人",
  }),
  feedbackSource: Object.freeze({
    touch: "触摸屏",
    web: "网页",
    api: "API",
  }),
  logLevel: Object.freeze({
    debug: "调试",
    info: "信息",
    warning: "警告",
    error: "错误",
  }),
  symbolicationStatus: Object.freeze({
    matched_elf: "已匹配 ELF",
    missing_elf: "缺少 ELF",
    succeeded: "符号解析成功",
    failed: "符号解析失败",
  }),
});

const HTTP_ERROR_LABELS = Object.freeze({
  400: "请求无效",
  401: "身份验证失败",
  403: "无权访问",
  404: "未找到请求的资源",
  409: "请求冲突",
  413: "上传内容过大",
  422: "提交内容无效",
  429: "请求过于频繁",
  500: "服务器内部错误",
  502: "上游服务响应异常",
  503: "服务暂时不可用",
  504: "上游服务响应超时",
});

function labelFor(group, value) {
  if (value == null) return "未知";
  return UI_LABELS[group]?.[value] || String(value);
}

function httpErrorLabel(status) {
  return HTTP_ERROR_LABELS[status] || "请求失败";
}

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
  let response;
  try {
    response = await fetch(path, {
      ...options,
      headers: options.headers || requestHeaders(Boolean(options.body)),
      cache: "no-store",
    });
  } catch (error) {
    if (error.name === "AbortError") throw error;
    throw new Error("网络请求失败，请检查连接后重试。", { cause: error });
  }
  if (!response.ok) {
    let responseDetail = null;
    try {
      const body = await response.json();
      responseDetail = body.detail ?? null;
    } catch (_error) {
      // 响应正文不是 JSON 时，没有额外的诊断数据可保留。
    }
    const error = new Error(`${httpErrorLabel(response.status)}（HTTP ${response.status}）`);
    error.status = response.status;
    // Keep the backend's raw diagnostic payload available to developer tooling,
    // but never mix it into the localized message rendered by the page.
    error.detail = responseDetail;
    throw error;
  }
  return response.json();
}

function formatAge(milliseconds) {
  if (!Number.isFinite(milliseconds)) return "—";
  if (milliseconds < 1_000) return "刚刚";
  const seconds = Math.floor(milliseconds / 1_000);
  if (seconds < 60) return `${seconds} 秒前`;
  const minutes = Math.floor(seconds / 60);
  if (minutes < 60) return `${minutes} 分钟前`;
  const hours = Math.floor(minutes / 60);
  if (hours < 48) return `${hours} 小时前`;
  return `${Math.floor(hours / 24)} 天前`;
}

function formatEastern(milliseconds, withDate = false) {
  const options = withDate
    ? { month: "short", day: "numeric", hour: "2-digit", minute: "2-digit", second: "2-digit" }
    : { hour: "2-digit", minute: "2-digit" };
  return new Intl.DateTimeFormat("zh-CN", {
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
    const option = new Option("未发现设备", "");
    elements.device.add(option);
    elements.device.disabled = true;
    elements.refresh.disabled = true;
    return;
  }
  for (const device of devices) {
    const suffix = device.online ? "在线" : formatAge(device.last_seen_age_ms);
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
  setConnection("正在连接", "loading");
  setMessage("正在加载设备…");
  try {
    const data = await apiFetch("/v1/console/devices");
    fillDeviceList(data.items);
    if (!data.items.length) {
      setConnection("已连接", "neutral");
      setMessage("已连接，但还没有设备上传遥测数据。");
      return;
    }
    await loadSnapshot();
    startRefreshTimer();
  } catch (error) {
    stopRefreshTimer();
    setConnection(error.status === 403 ? "拒绝访问" : "服务不可用", "offline");
    setMessage(error.status === 403 ? "此控制台仅允许从已批准的局域网或 Tailnet 设备访问。" : error.message, "error");
  }
}

async function loadSnapshot({ quiet = false } = {}) {
  if (!elements.device.value) return;
  if (snapshotController) snapshotController.abort();
  const controller = new AbortController();
  snapshotController = controller;
  if (!quiet) setMessage("正在加载遥测数据…");
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
    setMessage(`更新于 ${formatEastern(snapshot.server_utc_ms, true)}（美东时间）`, "success");
  } catch (error) {
    if (error.name === "AbortError") return;
    setConnection(error.status === 403 ? "拒绝访问" : "服务不可用", "offline");
    setMessage(error.message, "error");
    if (error.status === 403) stopRefreshTimer();
  } finally {
    if (snapshotController === controller) snapshotController = null;
  }
}

function renderSnapshot(data) {
  const { device, latest, calibration, window: timeWindow } = data;
  setConnection(device.online ? "设备在线" : "设备离线", device.online ? "online" : "offline");
  view.presence.textContent = latest ? labelFor("presenceState", latest.state) : "暂无数据";
  view.statusCard.dataset.state = latest ? latest.state : "unknown";
  view.latestDetail.textContent = latest
    ? `PIR ${latest.pir ? "有动作" : "静止"} · 声音${latest.sound_active ? "活跃" : "安静"}`
    : "暂无采样";
  view.lastSeen.textContent = formatAge(device.last_seen_age_ms);
  view.lastSeenDetail.textContent = device.last_seen_at_ms == null
    ? "尚未收到遥测数据"
    : formatEastern(device.last_seen_at_ms, true) + "（美东时间）";
  view.firmware.textContent = device.firmware_version || "未知";
  view.deviceId.textContent = device.device_id;
  const reportedRevision = device.latest_reported_config_revision == null
    ? "?"
    : device.latest_reported_config_revision;
  view.revision.textContent = `${device.desired_config_revision} / ${reportedRevision}`;
  view.revisionDetail.textContent = `${labelFor("configSync", device.config_sync)} · 期望 / 最新上报 · 已应用最高版本 ${device.highest_applied_config_revision}`;
  view.sampleCount.textContent = timeWindow.sample_count.toLocaleString("zh-CN");
  view.presentFraction.textContent = formatPercent(calibration.present_fraction);
  view.pirFraction.textContent = formatPercent(calibration.pir_active_fraction);
  view.soundFraction.textContent = formatPercent(calibration.sound_active_fraction);
  view.micMean.textContent = formatNumber(calibration.mic_envelope_mean);
  view.noiseMean.textContent = formatNumber(calibration.noise_floor_mean);
  view.thresholdMean.textContent = formatNumber(calibration.sound_threshold_mean);
  view.thresholdRatio.textContent = formatNumber(calibration.threshold_to_noise_ratio, 3);
  view.mismatchCount.textContent = String(calibration.feedback.mismatch);
  view.feedbackCount.textContent = `${timeWindow.feedback_count} 条标注`;
  view.configRevision.textContent = configDirty && configEditBase
    ? `版本 ${data.config.revision} · 基于版本 ${configEditBase.revision} 编辑`
    : `版本 ${data.config.revision}`;
  if (!configDirty) fillConfig(data.config, device.device_id);
  renderFeedback(data.feedback);
  renderTransitions(data.transitions);
  drawTimeline();
  const bucket = Math.round(timeWindow.bucket_ms / 1000);
  elements.caption.textContent = timeWindow.sample_count
    ? `${timeWindow.sample_count.toLocaleString("zh-CN")} 个样本 · ${timeWindow.returned_points.toLocaleString("zh-CN")} 个图表桶，每桶约 ${bucket} 秒 · 优先采用观测时间，缺失时使用接收时间`
    : "此时间范围内无遥测样本。";
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
    setMessage("编辑期间配置已发生变化。已重新加载当前值，请再次检查修改。", "error");
    return;
  }
  const reviewedConfig = readConfig();
  const changes = Object.entries(reviewedConfig).filter(
    ([name, value]) => value !== configEditBase.config[name],
  );
  elements.changeList.replaceChildren();
  if (!changes.length) {
    configDirty = false;
    setMessage("配置值没有变化。");
    return;
  }
  for (const [name, value] of changes) {
    const item = document.createElement("li");
    item.textContent = `${labelFor("configField", name)}：${configEditBase.config[name]} → ${value}`;
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
    setMessage("配置版本已发布，正在等待设备确认。", "success");
  } catch (error) {
    elements.dialog.close();
    if (error.status === 409) {
      configDirty = false;
      configEditBase = null;
      await loadSnapshot({ quiet: true });
      setMessage("配置已在其他位置发生变化。已刷新当前值，请重新检查。", "error");
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
    setReleaseMessage("请先选择规范的 .ota.zip 固件包。", "error");
    return;
  }
  elements.importRelease.disabled = true;
  setReleaseMessage(`正在读取并验证 ${file.name}…`);
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
    setReleaseMessage(`已验证 ${imported.firmware_version}，发布计数器 ${imported.release_counter}。`, "success");
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
  elements.releaseReviewText.textContent = `${snapshot.device.device_id} 将请求适用于 ${release.hardware_model} 的 ${release.firmware_version}，发布计数器 ${release.release_counter}，构建 ${release.build_id}。SHA-256 ${release.image_sha256}。`;
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
    setReleaseMessage("已选择目标版本；设备将在下次通过身份验证的轮询中获取它。", "success");
  } catch (error) {
    elements.releaseDialog.close();
    setReleaseMessage(error.message, "error");
  }
}

async function issueCommand() {
  if (!snapshot) return;
  const action = elements.commandAction.value;
  if (!window.confirm(`要向 ${snapshot.device.device_id} 下发一次性命令“${labelFor("commandAction", action)}”吗？`)) return;
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
    setMessage(`命令“${labelFor("commandAction", action)}”已加入队列。`, "success");
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
    cell.textContent = "此时间范围内无反馈";
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
      labelFor("feedbackPresence", item.actual_presence),
      item.observed_state ? labelFor("presenceState", item.observed_state) : "—",
      labelFor("feedbackSource", item.source),
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
    emptyTable(view.transitionBody, 8, "此时间范围内无状态变化");
    return;
  }
  view.transitionBody.replaceChildren();
  for (const item of [...transitions].reverse().slice(0, 200)) {
    const pirAge = item.pir_age_ms == null ? "未知" : `${item.pir_age_ms} 毫秒前`;
    const soundAge = item.sound_age_ms == null ? "未知" : `${item.sound_age_ms} 毫秒前`;
    const mic = item.mic_envelope == null
      ? "旧版记录"
      : `${formatNumber(item.mic_envelope)} / ${formatNumber(item.sound_threshold)}（噪声 ${formatNumber(item.noise_floor)}）`;
    appendCells(view.transitionBody, [
      formatEastern(item.marker_ms, true),
      `${item.from_state ? labelFor("presenceState", item.from_state) : "启动"} → ${labelFor("presenceState", item.to_state)}`,
      labelFor("transitionReason", item.reason),
      item.pir == null ? "旧版记录" : `${item.pir ? "有动作" : "静止"} · ${pirAge}`,
      item.sound_active == null ? "旧版记录" : `${item.sound_active ? "活跃" : "安静"} · ${soundAge}`,
      mic,
      item.brightness_before == null ? "旧版记录" : `${item.brightness_before} → ${item.brightness_after}`,
      `${item.build_id || "未知"} · 配置 ${item.applied_config_revision ?? "?"}`,
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
    ? `在线 · 最近活动：${formatAge(snapshot.device.last_seen_age_ms)}`
    : "服务器判定为离线";
  view.healthLevel.textContent = latest ? labelFor("healthLevel", latest.level) : "未知";
  view.healthLevel.dataset.level = latest ? latest.level : "unknown";
  if (latest) {
    view.healthWifi.textContent = latest.wifi.connected
      ? `${latest.wifi.ip || "无 IP"} · ${latest.wifi.rssi_dbm ?? "?"} dBm · 已重连 ${latest.wifi.reconnect_count} 次`
      : "未连接";
    view.healthBuild.textContent = `${latest.firmware_version} · ${latest.build_id} · ${latest.reset_reason}`;
    view.healthTasks.textContent = `主任务 ${latest.tasks.main_heartbeat_ms} 毫秒 · 上传任务 ${latest.tasks.uploader_heartbeat_ms} 毫秒`;
    view.healthQueues.textContent = `遥测 ${latest.queues.telemetry_depth}/${latest.queues.telemetry_capacity} · 反馈 ${latest.queues.feedback_depth}/${latest.queues.feedback_capacity}`;
    view.healthStorage.textContent = `${latest.storage.spool_files} 个待传文件 · ${latest.storage.dead_files} 个失效文件 · 可用 ${latest.storage.littlefs_free_bytes.toLocaleString("zh-CN")} B`;
    view.healthMemory.textContent = `可用 ${latest.memory.free_heap_bytes.toLocaleString("zh-CN")} B · 历史最低 ${latest.memory.min_free_heap_bytes.toLocaleString("zh-CN")} B`;
    view.healthSensors.textContent = `PIR ${labelFor("sensorHealth", latest.sensors.pir_status)} · 麦克风 ${labelFor("sensorHealth", latest.sensors.mic_status)}${latest.sensors.pir_only_mode ? " · 仅 PIR 模式" : ""}`;
  } else {
    for (const node of [view.healthWifi, view.healthBuild, view.healthTasks, view.healthQueues, view.healthStorage, view.healthMemory, view.healthSensors]) node.textContent = "暂无健康报告";
  }

  const releases = data.releases;
  const previousSelection = elements.releaseSelect.value;
  elements.releaseSelect.replaceChildren();
  if (!releases.length) {
    elements.releaseSelect.add(new Option("尚未导入固件版本", ""));
    elements.reviewRelease.disabled = true;
  } else {
    elements.releaseSelect.add(new Option("选择已验证的固件版本…", ""));
    for (const release of releases) {
      elements.releaseSelect.add(new Option(
        `${release.firmware_version} · 计数器 ${release.release_counter} · ${release.build_id}`,
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
  view.releasePhase.textContent = status ? labelFor("releasePhase", status.phase) : "空闲";
  view.releaseProgress.textContent = status
    ? `${status.progress_percent ?? "—"}% · 回滚：${labelFor("rollbackOutcome", status.rollback_outcome)}`
    : "—";
  view.releaseError.textContent = status?.last_error || "—";

  view.logCount.textContent = `保留 ${data.logs.retained_records} 条`;
  if (!data.logs.items.length) emptyTable(view.logBody, 4, "暂无运行事件");
  else {
    view.logBody.replaceChildren();
    for (const item of data.logs.items) appendCells(view.logBody, [
      formatEastern(item.received_at_ms, true),
      labelFor("logLevel", item.level),
      item.event_type,
      JSON.stringify(item.fields),
    ], { level: item.level });
  }

  view.crashCount.textContent = `保留 ${data.crashes.retained_reports} 条`;
  if (!data.crashes.items.length) emptyTable(view.crashBody, 4, "暂无崩溃报告");
  else {
    view.crashBody.replaceChildren();
    for (const item of data.crashes.items) appendCells(view.crashBody, [
      formatEastern(item.received_at_ms, true),
      item.reset_reason,
      item.build_id,
      `${labelFor("symbolicationStatus", item.symbolication_status)}：${item.summary.join(" · ")}`,
    ]);
  }

  if (!data.commands.length) emptyTable(view.commandBody, 5, "暂无命令");
  else {
    view.commandBody.replaceChildren();
    for (const item of data.commands) appendCells(view.commandBody, [
      formatEastern(item.created_at_ms, true),
      labelFor("commandAction", item.command.action),
      labelFor("commandStatus", item.status),
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
    context.fillText("当前时间窗暂无样本", width / 2, height / 2);
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
