"use strict";
const $ = id => document.getElementById(id);
let mode = "cloud", device = "", csrf = "", state = null, timer = null, records = [], db = null, busy = false, rulesRevision = null;
let wifiListSignature = "", connectingBluetooth = false, cancelHistory = false, metricChosen = false;
let startupSuperseded = false;
const metrics = {co2_raw_ppm: ["Raw CO₂", "ppm", 0], temperature_c: ["Temperature", "°C", 2], humidity_rh: ["Relative humidity", "%", 2], pressure_hpa: ["Pressure", "hPa", 1], barometer_temperature_c: ["Pressure sensor temperature", "°C", 2], mean: ["Validated CO₂", "ppm", 0]};
let lastResponseAt = 0, lastCommandsAt = 0;
const message = text => { $("message").textContent = text; $("feedback").hidden = !text; };
function inlineStatus(id, text) {
  const element = $(id);
  element.textContent = text;
  element.hidden = !text;
}
const viewTitles = {overview: "Overview", history: "History", settings: "Settings"};
const labels = {
  VALID: "Reading valid", COMMISSIONING: "Sensor setup", WARMUP: "Warming up", FAULT: "Sensor error",
  UNASSESSED: "Pending", CLEAR: "Normal", WATCH: "Elevated", HIGH: "High",
  UNKNOWN: "Trend pending", RISING: "Rising", STABLE: "Steady", FALLING: "Falling",
  MANUAL_OFFLINE: "Wi-Fi off", NO_SAVED_NETWORK: "No saved networks", WIFI_CONNECTED: "Connected", CONNECTING: "Connecting", RETRY_BACKOFF: "Retrying",
  OFFLINE: "Offline", SYNCED: "Synced", CATCHING_UP: "Syncing", WAITING_FOR_WIFI: "Waiting for Wi-Fi", WAITING_FOR_TIME: "Waiting for clock sync",
  AUTH_FAILED: "Sign-in failed", CONTENT_CONFLICT: "Data conflict", TLS_OR_TRANSPORT_FAILED: "Connection failed", SERVER_UNAVAILABLE: "Server unavailable",
  BROKER_ACKNOWLEDGED: "Synced", CONNECTED: "Connected", RECONNECTING: "Reconnecting", AUTH_TLS_OR_NETWORK_FAILED: "Connection failed", NOT_CONFIGURED: "Not configured"
};
const label = value => labels[value] || value || "Unknown";
function showView(view, remember = true) {
  if (!Object.prototype.hasOwnProperty.call(viewTitles, view)) view = "overview";
  document.querySelectorAll("[data-panel]").forEach(panel => { panel.hidden = panel.dataset.panel !== view; });
  document.querySelectorAll('[role="tab"]').forEach(tab => {
    const selected = tab.dataset.view === view;
    tab.setAttribute("aria-selected", String(selected)); tab.tabIndex = selected ? 0 : -1;
  });
  $("view-title").textContent = viewTitles[view];
  if (remember && location.hash !== `#${view}`) history.pushState(null, "", `#${view}`);
  window.scrollTo(0, 0);
  if (view === "history") requestAnimationFrame(renderHistory);
}
const date = value => value ? new Date(value).toLocaleString("en-US") : "Time unavailable";
const duration = value => value == null ? "—" : `${Math.floor(value / 3600000)}h ${Math.floor(value / 60000) % 60}m ${Math.floor(value / 1000) % 60}s`;
const recordTime = row => row.measured_at ? date(row.measured_at) : row.estimated_measured_at ? `Approx. ${date(row.estimated_measured_at)}` : `Uptime +${duration(row.monotonic_ms)}`;
function compareRecords(a, b) {
  const first = a.order || a.received_order, second = b.order || b.received_order;
  if (first != null && second != null) return BigInt(first) < BigInt(second) ? -1 : BigInt(first) > BigInt(second) ? 1 : 0;
  if (a.boot_id === b.boot_id) return a.monotonic_ms - b.monotonic_ms;
  return (a.measured_at || a.estimated_measured_at || a.received_at || 0) - (b.measured_at || b.estimated_measured_at || b.received_at || 0) || a.boot_id.localeCompare(b.boot_id);
}
const base = () => mode !== "cloud" ? "/api/v1" : `/v1/devices/${encodeURIComponent(device)}`;
let reconnectTimer = null;
function scheduleBluetoothRecovery(attempt = 0) {
  clearTimeout(reconnectTimer);
  if (attempt >= 3 || mode !== "ble") return;
  reconnectTimer = setTimeout(async () => {
    if (mode !== "ble" || connectingBluetooth || aircueTransport.connected) return;
    if (busy) {cancelHistory = true; scheduleBluetoothRecovery(attempt); return;}
    try { await connectBluetooth(); }
    catch { scheduleBluetoothRecovery(attempt + 1); }
  }, [1000, 3000, 8000][attempt]);
}
const aircueTransport = new AirCueBluetooth(intentional => {
  if (mode === "ble") {
    clearTimeout(timer);
    $("freshness").textContent = "Bluetooth disconnected";
    $("switch-transport").textContent = "Reconnect Bluetooth";
    inlineStatus("notice", intentional ? "" : "Reconnecting to the device…");
    if (!intentional) scheduleBluetoothRecovery();
  }
}, text => {
  if ($("login-panel").hidden) message(text);
  else inlineStatus("ble-progress", text);
}, () => {
  if (mode === "ble" && state?.device_id === device) { applyLive(); paintMeasurements(); }
});
async function request(path, method = "GET", body = null, retry = false) {
  const transport = mode;
  if (mode === "ble") {
    const url = new URL(path, location.origin), op = url.pathname.split("/").pop();
    return aircueTransport.request(op, method === "GET" ? Object.fromEntries(url.searchParams) : body);
  }
  const options = {method, credentials: "same-origin", headers: {"Content-Type": "application/json", "X-CSRF-Token": csrf}, signal: AbortSignal.timeout(15000)};
  if (body) options.body = JSON.stringify(body);
  try {
    const response = await fetch(path, options);
    const result = await response.json();
    if (!response.ok) {
      if (response.status === 401 && transport === mode && mode !== "ble" && !path.endsWith("/session")) showLogin();
      throw new Error(typeof result.detail === "string" ? result.detail : result.message || `Request failed (${response.status})`);
    }
    return result;
  } catch (error) {
    // Reuse the exact request body and ID if an HTTP response was lost.
    if (retry && transport === mode && (error.name === "TypeError" || error.name === "TimeoutError")) return request(path, method, body, false);
    throw error;
  }
}
async function openCache() {
  return new Promise((resolve, reject) => {
    const opening = indexedDB.open("aircue-history-v1", 1);
    opening.onupgradeneeded = () => { const store = opening.result.createObjectStore("records", {keyPath: "key"}); store.createIndex("device", "device"); opening.result.createObjectStore("cursors"); };
    opening.onsuccess = () => resolve(opening.result); opening.onerror = () => reject(opening.error);
  });
}
// Cloud row cursors and device journal cursors belong to different order domains.
const cacheKey = () => `${location.origin}|${device}|${mode}`;
const cursorKey = () => cacheKey();
async function cachedHistory() {
  if (!db) return [];
  return new Promise((resolve, reject) => { const query = db.transaction("records").objectStore("records").index("device").getAll(cacheKey()); query.onsuccess = () => resolve(query.result.map(row => row.value)); query.onerror = () => reject(query.error); });
}
async function cursor() {
  if (!db) return "";
    return new Promise(resolve => { const query = db.transaction("cursors").objectStore("cursors").get(cursorKey()); query.onsuccess = () => resolve(query.result || ""); query.onerror = () => resolve(""); });
}
async function savePage(page, advanceCursor = true) {
  if (!db) return;
  return new Promise((resolve, reject) => {
    const tx = db.transaction(["records", "cursors"], "readwrite");
    for (const row of page.records) tx.objectStore("records").put({key: `${cacheKey()}|${row.record_id}`, device: cacheKey(), value: row});
    if (advanceCursor) tx.objectStore("cursors").put(page.next_cursor, cursorKey());
    tx.oncomplete = resolve; tx.onerror = () => reject(tx.error);
  });
}
function showLogin() {
  clearTimeout(timer);
  $("dashboard").hidden = true;
  $("login-panel").hidden = false;
  $("logout").hidden = true;
  $("source").textContent = "";
  document.body.classList.remove("signed-in");
}
async function enter(initialStatus = null) {
  if (mode === "cloud") {
    const data = await request("/v1/devices");
    if (!data.devices.length) throw new Error("No device is linked to this account.");
    $("devices").replaceChildren(...data.devices.map(id => { const option = document.createElement("option"); option.value = option.textContent = id; return option; }));
    device = data.devices[0]; $("device-picker").hidden = false;
  }
  $("device-picker").hidden = mode !== "cloud";
  $("login-panel").hidden = true; $("dashboard").hidden = false; $("logout").hidden = false;
  document.body.classList.add("signed-in");
  $("source").textContent = mode === "ble" ? "Bluetooth" : mode === "local" ? "Local device" : "Online";
  $("switch-transport").textContent = mode === "ble" ? "Switch to online" : "Use Bluetooth";
  $("logout").textContent = mode === "ble" ? "Disconnect" : "Sign out";
  showView(location.hash.slice(1), false);
  records = await cachedHistory(); renderHistory();
  if (initialStatus) { state = initialStatus; lastResponseAt = performance.now(); applyLive(); paintStatus(); }
  else await refresh();
  await syncHistory(false);
}
function applyLive() {
  const frame = aircueTransport.latestLive;
  if (mode !== "ble" || !state || !frame || performance.now() - frame.receivedAt > 3000) return;
  for (const key of ["ppm", "quality", "level", "trend", "range_warn", "recovering"]) state[key] = frame[key];
  state.climate = {...state.climate, ...frame.climate};
  state.sample_age_ms = frame.ppm == null ? null : frame.rawAge;
  state.stale = frame.ppm == null;
  state.diagnostics = {...state.diagnostics, raw_sample_ok: frame.climate.co2_raw_ppm != null, raw_ppm: frame.climate.co2_raw_ppm, raw_sample_age_ms: frame.rawAge};
  lastResponseAt = frame.receivedAt;
}
function paintLiveAge() {
  if (!state || $("dashboard").hidden) return;
  const live = mode === "ble" && aircueTransport.hasLive, stamp = live ? aircueTransport.latestLive?.receivedAt : lastResponseAt;
  const age = stamp ? Math.max(0, (performance.now() - stamp + (mode === "cloud" ? state.heartbeat_age_ms || 0 : 0)) / 1000) : null;
  $("data-age").textContent = mode === "ble" && !aircueTransport.connected ? "Disconnected" : age == null ? "Waiting for data" : `${live ? "Live" : "Updated"} ${age < 1 ? "just now" : `${Math.floor(age)}s ago`}`;
  $("data-age").classList.toggle("stale", live && age > 3);
  if (live && age > 3 && aircueTransport.connected) {
    $("freshness").textContent = "Waiting for data"; $("freshness").className = "badge warn";
  }
}
function paintMeasurements() {
  const s = state, cfg = s.config || {}, diagnostics = s.diagnostics || {};
  const ambient = s.climate || {temperature_c: s.diagnostics?.raw_sample_ok ? s.diagnostics.temperature_c : null, humidity_rh: s.diagnostics?.raw_sample_ok ? s.diagnostics.humidity_rh : null, pressure_hpa: s.diagnostics?.hardware?.pressure_hpa};
  for (const [id, field, digits] of [["climate-temperature", "temperature_c", 1], ["climate-humidity", "humidity_rh", 1], ["climate-pressure", "pressure_hpa", 1]]) {
    $(id).textContent = ambient[field] == null ? "—" : `${Number(ambient[field]).toFixed(digits)} ${metrics[field][1]}`;
  }
  const stale = s.stale || s.sample_age_ms == null || s.sample_age_ms > (mode !== "cloud" ? 5000 : 90000);
  const climateFresh = ["co2_raw_ppm", "temperature_c", "humidity_rh", "pressure_hpa"].some(key => ambient[key] != null) && (mode === "cloud" ? s.heartbeat_age_ms != null && s.heartbeat_age_ms <= 90000 : mode === "ble" ? aircueTransport.connected : !stale);
  const rawPpm = s.climate ? s.climate.co2_raw_ppm : diagnostics.raw_sample_ok && (diagnostics.raw_sample_age_ms == null || diagnostics.raw_sample_age_ms <= 5000) ? diagnostics.raw_ppm : null;
  const showRaw = Number.isFinite(rawPpm) && (s.quality !== "VALID" || s.ppm == null);
  const displayedPpm = showRaw ? rawPpm : s.ppm;
  $("ppm").textContent = Number.isFinite(displayedPpm) ? Math.round(displayedPpm) : "—";
  $("co2-title").textContent = showRaw ? "CO₂ (raw)" : Number.isFinite(s.ppm) && stale ? "CO₂ (last valid)" : "CO₂";
  const statusFlagged = showRaw && s.quality === "FAULT";
  $("quality").textContent = `${statusFlagged ? "Check sensor status" : label(s.quality || "Waiting for device")}${cfg.demo ? " · Demo" : ""}${s.recovering ? " · Checking readings" : ""}`;
  const advice = $("ventilation-advice");
  if (advice) advice.textContent = ventilationAdvice(s, stale);
  $("level").textContent = label(s.level || "UNASSESSED"); $("trend").textContent = label(s.trend || "UNKNOWN");
  $("quality").title = s.quality || ""; $("level").title = s.level || ""; $("trend").title = s.trend || "";
  $("level").className = `badge ${s.level === "HIGH" ? "bad" : s.level === "WATCH" ? "warn" : ""}`;
  $("range-warning").hidden = !s.range_warn;
  $("freshness").textContent = stale && !climateFresh ? "Stale data" : mode === "cloud" ? "Synced" : "Live";
  $("freshness").className = `badge ${stale && !climateFresh ? "warn" : ""}`;
  inlineStatus("notice", stale && !climateFresh ? s.offline_reason ? "Device offline. Showing last received readings." : "Showing the last reading." : "");
  $("measured").textContent = date(s.measured_at); $("received").textContent = date(s.received_at);
  $("age").textContent = duration(s.sample_age_ms); $("uptime").textContent = duration(s.uptime_ms);
  inlineStatus("raw-preview", showRaw ? "Raw readings are excluded from alerts and validated statistics." : "");
  paintLiveAge();
}
function ventilationAdvice(s, stale) {
  // Trend supports the concentration decision; falling alone never means recovery.
  if (stale || mode === "ble" && !aircueTransport.connected) return "Waiting for a current reading.";
  if (s.quality === "FAULT") return "Check the sensor in Settings.";
  if (s.quality !== "VALID") return "Guidance will be available when the sensor is ready.";
  if (s.recovering || s.level === "UNASSESSED") return "Checking readings.";
  if (s.level === "HIGH") return s.trend === "FALLING" ? "CO₂ is falling and still high. Keep ventilating." : "CO₂ is high. Increase ventilation.";
  if (s.level === "WATCH") return s.trend === "FALLING" ? "CO₂ is falling. Keep ventilating." : "CO₂ is elevated. Increase ventilation.";
  if (s.level === "CLEAR") return s.trend === "RISING" ? "CO₂ is in the normal range and rising. Check again soon." : "CO₂ is in the normal range. Maintain ventilation.";
  return "Waiting for a valid reading.";
}
function paintStatus() {
  const s = state, cfg = s.config || {}, diagnostics = s.diagnostics || {};
  paintMeasurements();
  $("connection").textContent = `${label(s.network)} / ${label(s.cloud)}`;
  $("wifi-name").textContent = s.connected_ssid || s.wifi_target_ssid || cfg.ssid || "Not configured";
  const wifi = s.sta_connected ? s.connected_ssid || "Wi-Fi connected" : label(s.network);
  $("connection-summary").textContent = `${wifi} · Mac ${label(s.cloud)}${s.remote_mqtt ? ` · Lab 5 ${label(s.remote_mqtt)}` : ""}`;
  const count = cfg.wifi_networks?.length || 0;
  $("network-summary").textContent = `${cfg.uplink_enabled ? "On" : "Off"} · ${count} saved ${count === 1 ? "network" : "networks"}`;
  renderWifi(cfg.wifi_networks || [], s.connected_ssid);
  $("sound-toggle").textContent = cfg.buzzer_enabled ? "Turn off alert sound" : "Turn on alert sound";
  if (!$("wifi").contains(document.activeElement)) $("wifi").elements.ssid.value = cfg.ssid || "";
  $("backlog").textContent = s.storage?.backlog_upper_bound ?? "—";
  $("revision").textContent = `v${cfg.revision || "?"}`;
  $("rules-summary").textContent = `Watch ${cfg.watch_on ?? "—"} / high ${cfg.high_on ?? "—"} ppm`;
  $("uplink").textContent = cfg.uplink_enabled ? "Turn off Wi-Fi" : "Turn on Wi-Fi";
  const ep = s.episode || {};
  $("episode").textContent = ep.active ? `Active alert · Peak ${ep.peak} ppm${ep.interrupted ? " · Sampling interrupted" : ""}` : "";
  $("episode-details").hidden = !ep.active;
  $("episode-meta").textContent = ep.active ? `Above threshold ${duration(ep.high_ms)} · In alert ${duration(ep.red_ms)}` : "";
  $("mute").hidden = !ep.active;
  $("mute").disabled = !ep.active; $("mute").textContent = s.mute_remaining_s ? `Muted · ${s.mute_remaining_s}s left` : "Mute for 5 min";
  if (rulesRevision !== cfg.revision && !$("rules").contains(document.activeElement)) {
    for (const input of $("rules").querySelectorAll("input")) input.value = cfg[input.name] ?? "";
    rulesRevision = cfg.revision;
  }
  const needsPreparation = !cfg.commissioned || s.quality === "COMMISSIONING";
  $("commission-progress").textContent = needsPreparation ? `${duration(diagnostics.commission_elapsed_ms)} of 12 hours of continuous sampling.` : "Sensor setup complete.";
  $("run-info").textContent = cfg.run_id ? `Run ${cfg.run_id} · Started ${date(cfg.run_t0)}` : "No active recording.";
  $("run-summary").textContent = cfg.run_id ? "Recording" : needsPreparation ? "Sensor setup in progress" : "Ready to record";
  $("preparation-progress").value = Math.min(43200000, diagnostics.commission_elapsed_ms || 0);
  $("preparation-progress").hidden = !needsPreparation;
  $("firmware-summary").textContent = `Firmware ${s.firmware || "unknown"} · ${s.storage?.healthy ? "Storage healthy" : "Check storage"}`;
  $("demo").textContent = cfg.demo ? "Disable demo mode" : "Enable demo mode";
  $("commission").disabled = !!cfg.run_id || !needsPreparation || !(diagnostics.commission_elapsed_ms >= 43200000 && diagnostics.self_test === 0 && $("fresh-air").checked);
  $("run-start").disabled = !!cfg.run_id || !cfg.commissioned || cfg.demo || s.quality !== "VALID" || s.time_quality !== "SYNCED";
  $("run-stop").disabled = !cfg.run_id;
  $("demo").disabled = !!cfg.run_id;
  $("ota").disabled = !!cfg.run_id || !cfg.uplink_enabled || !s.sta_connected || s.time_quality !== "SYNCED";
  for (const input of $("rules").elements) input.disabled = !!cfg.run_id;
  $("diagnostics").textContent = JSON.stringify({firmware: s.firmware, time_quality: s.time_quality, storage: s.storage, bluetooth: s.bluetooth, remote_mqtt: s.remote_mqtt, remote_last_ack_utc: s.remote_last_ack_utc, diagnostics, ota: s.ota}, null, 2);
  updateExportLink();
}
function updateExportLink() {
  const form = $("export-filter"), values = new FormData(form), query = new URLSearchParams();
  const from = values.get("from") ? new Date(values.get("from")).getTime() : null;
  const to = values.get("to") ? new Date(values.get("to")).getTime() : null;
  const invalid = !form.checkValidity() || [from, to].some(value => value != null && (!Number.isSafeInteger(value) || value < 0)) || from != null && to != null && from >= to;
  $("export").setAttribute("aria-disabled", String(invalid));
  $("export-range-note").textContent = invalid ? "Enter valid dates and a run ID. The end must come after the start." : "Leave both dates blank to include records without timestamps.";
  if (from != null) query.set("from_ms", from);
  if (to != null) query.set("to_ms", to);
  if (values.get("run_id")) query.set("run_id", values.get("run_id"));
  $("export").href = invalid || mode === "ble" ? "#" : `${base()}/export${query.size ? "?" + query : ""}`;
  return !invalid;
}
async function refresh() {
  clearTimeout(timer);
  const transport = mode, selected = device;
  if (busy || transport === "ble" && !aircueTransport.connected) return;
  try {
    const value = await request(`${base()}/status`);
    if (mode !== transport || device !== selected) return;
    state = value; lastResponseAt = performance.now(); applyLive(); paintStatus();
    if (mode === "cloud" && Date.now() - lastCommandsAt > 15000) { lastCommandsAt = Date.now(); await showCommands(); }
  }
  catch (error) {
    inlineStatus("notice", "Connection lost. Retrying…");
    $("freshness").textContent = "Disconnected";
  }
  if (!$("dashboard").hidden && mode === transport && (mode !== "ble" || aircueTransport.connected)) timer = setTimeout(refresh, mode === "ble" && aircueTransport.hasLive ? 15000 : mode !== "cloud" ? 2000 : 5000);
}
async function syncHistory(all = true) {
  if (busy) return;
  const transport = mode, selected = device;
  busy = true; cancelHistory = false; clearTimeout(timer);
  $("sync-history").disabled = true; $("sync-history").classList.add("is-pending"); $("sync-history").textContent = "Syncing…";
  $("cancel-history").hidden = !all;
  if (all) message("");
  try {
    let next = await cursor(), pages = 0;
    // A first look at recent online data must not skip records in a later full sync.
    const recent = transport === "cloud" && !all && !next;
    do {
      const query = recent ? "limit=128&recent=true" : `limit=128&cursor=${encodeURIComponent(next)}`;
      const page = await request(`${base()}/history?${query}`);
      if (mode !== transport || device !== selected) throw new Error("Connection changed. Sync history again.");
      await savePage(page, !recent); next = page.next_cursor;
      const index = new Map(records.map(row => [row.record_id, row]));
      for (const row of page.records) index.set(row.record_id, row);
      records = [...index.values()]; renderHistory(); ++pages;
      if (!cancelHistory) $("sync-history").textContent = `Syncing (${records.length})`;
      if (cancelHistory || !all || !page.has_more) break;
    } while (pages < 2048);
    $("gaps").textContent = JSON.stringify(await request(`${base()}/gaps`), null, 2);
    if (mode === "cloud") await showCommands();
    if (all) message(cancelHistory ? "Sync canceled. Downloaded records saved." : `History synced. ${records.length} records available.`);
  } catch (error) { message(error.message); }
  finally { busy = false; $("sync-history").disabled = false; $("sync-history").classList.remove("is-pending"); $("sync-history").textContent = "Sync history"; $("cancel-history").hidden = true; timer = setTimeout(refresh, 1000); }
}
function renderHistory() {
  records.sort(compareRecords);
  const minutes = records.filter(row => row.record_type === "MINUTE");
  const events = records.filter(row => row.record_type === "EVENT");
  $("history-count").textContent = minutes.length ? `Showing the last ${Math.min(60, minutes.length)} of ${minutes.length}` : "No minute records";
  $("event-count").textContent = events.length ? `Showing the last ${Math.min(20, events.length)} of ${events.length}` : "No events";
  $("events-empty").hidden = events.length > 0;
  if (!metricChosen && !minutes.some(row => row.co2_raw_ppm != null) && minutes.some(row => row.mean != null)) $("chart-metric").value = "mean";
  const field = $("chart-metric").value, recent = minutes.slice(-240);
  const first = recent.findIndex(row => row[field] != null), plotted = first < 0 ? [] : recent.slice(first);
  const points = plotted.filter(row => row[field] != null).length, available = points > 0;
  $("history-info").textContent = `${records.length} records · ${points} points on chart`;
  $("chart-empty").hidden = available;
  $("chart-empty").textContent = !minutes.length ? "No minute records yet." : field === "mean" ? "No validated CO₂ readings yet. Select CO₂ (raw) to see readings during setup." : !minutes.some(row => Object.prototype.hasOwnProperty.call(row, field)) ? "Older records do not contain this measurement." : "No valid readings for this metric.";
  $("chart").hidden = $("chart-legend").hidden = !available;
  $("metric-note").textContent = field === "co2_raw_ppm" ? "Raw readings include sensor setup." : "";
  $("metric-note").hidden = field !== "co2_raw_ppm";
  $("chart-legend").textContent = field === "mean" ? "● Average   — Min / max   · Gaps show missing readings" : "One-minute averages · Gaps show missing readings";
  const columns = field === "mean" ? ["Time", "Average (ppm)", "Range", "Valid / expected", "Status", "Flags"] : ["Time", `${metrics[field][0]} (${metrics[field][1]})`, "Samples", "Flags"];
  $("history-columns").replaceChildren(...columns.map(label => { const th = document.createElement("th"); th.scope = "col"; th.textContent = label; return th; }));
  $("history-rows").replaceChildren(...minutes.slice(-60).reverse().map(row => {
    const tr = document.createElement("tr");
    const flags = [row.demo && "DEMO", row.partial && "PARTIAL", row.time_quality, row.missed_count > 0 && "GAP"].filter(Boolean).join(" / ");
    const count = row[["pressure_hpa", "barometer_temperature_c"].includes(field) ? "barometer_count" : "co2_raw_count"];
    const values = field === "mean" ? [recordTime(row), row.mean ?? "—", row.mean == null ? "—" : `${row.minimum}–${row.maximum}`, `${row.valid_count}/${row.expected_count}`, `${row.level} · ${row.trend}`, flags] : [recordTime(row), row[field] == null ? "—" : Number(row[field]).toFixed(metrics[field][2]), count ?? "—", flags];
    for (const value of values) { const td = document.createElement("td"); td.textContent = value; tr.append(td); } return tr;
  }));
  $("events").replaceChildren(...events.slice(-20).reverse().map(row => {
    const li = document.createElement("li");
    const detail = ["EPISODE_START", "FALLING", "RECOVERED", "CONFIG_CHANGED"].includes(row.event) ? ` · Peak ${row.peak} ppm · Above threshold ${duration(row.high_ms)} · In alert ${duration(row.red_ms)}` : "";
    li.textContent = `${recordTime(row)} · ${row.event}${detail}${row.demo ? " · DEMO" : ""}${row.interrupted ? " · INTERRUPTED" : ""}`; return li;
  }));
  drawChart(plotted);
}
function drawChart(rows) {
  const canvas = $("chart"), scale = window.devicePixelRatio || 1, width = canvas.clientWidth, height = 230;
  // Hidden tabs have no layout width; redraw after the history panel opens.
  if (!width) return;
  canvas.width = width * scale; canvas.height = height * scale;
  const ctx = canvas.getContext("2d"); ctx.scale(scale, scale); ctx.clearRect(0, 0, width, height);
  const field = $("chart-metric").value, definition = metrics[field], formal = field === "mean";
  const values = rows.flatMap(row => formal ? [row.minimum, row.maximum] : [row[field]]).filter(value => value != null && Number.isFinite(value));
  if (!values.length) return;
  const range = Math.max(Math.max(...values) - Math.min(...values), definition[1] === "ppm" ? 100 : definition[1] === "%" ? 5 : 1);
  const min = formal ? 0 : Math.min(...values) - range * .15, max = formal ? Math.max(2000, ...values) : Math.max(...values) + range * .15;
  const left = field === "pressure_hpa" ? 60 : 49, right = width - 8, top = 15, bottom = 198;
  const y = value => bottom - (value - min) / (max - min) * (bottom - top), x = i => left + i / Math.max(1, rows.length - 1) * (right - left);
  const tickDecimals = Math.min(2, Math.max(0, Math.ceil(-Math.log10((max - min) / 4))));
  ctx.font = "11px system-ui"; ctx.textAlign = "right";
  for (let i = 0; i <= 4; i++) { const value = min + (max - min) * i / 4; ctx.fillStyle = "#63756c"; ctx.fillText(value.toFixed(tickDecimals), left - 8, y(value) + 4); ctx.strokeStyle = "#e4e9df"; ctx.beginPath(); ctx.moveTo(left, y(value)); ctx.lineTo(right, y(value)); ctx.stroke(); }
  for (const [key, color, lineWidth] of formal ? [["minimum", "#b4c7a8", 1], ["maximum", "#b4c7a8", 1], ["mean", "#22634e", 2]] : [[field, "#22634e", 2]]) {
    ctx.beginPath(); ctx.strokeStyle = color; ctx.lineWidth = lineWidth; let previous = null;
    rows.forEach((row, i) => {
      const wall = row.measured_at || row.estimated_measured_at;
      const t = wall || row.monotonic_ms, domain = wall ? "utc" : `boot:${row.boot_id}`;
      if (row[key] == null || t == null) { previous = null; return; }
      if (!previous || previous.domain !== domain || t - previous.time > 65000 || t <= previous.time) ctx.moveTo(x(i), y(row[key])); else ctx.lineTo(x(i), y(row[key]));
      previous = {time: t, domain};
    }); ctx.stroke();
    if (key === field) { ctx.fillStyle = color; rows.forEach((row, i) => { if (row[key] != null) { ctx.beginPath(); ctx.arc(x(i), y(row[key]), 2.5, 0, Math.PI * 2); ctx.fill(); } }); }
  }
  ctx.textAlign = "left"; ctx.fillStyle = "#63756c";
  ctx.fillText(`Older to newer · 1 min per point · ${definition[1]}`, left, 222);
}
function newId() {
  const bytes = crypto.getRandomValues(new Uint8Array(16));
  return Array.from(bytes, x => x.toString(16).padStart(2, "0")).join("");
}
async function mutate(operation, payload) {
  if (!state?.config?.revision) throw new Error("Wait for the device settings to load.");
  let body = {request_id: newId(), base_revision: state.config.revision, ...payload};
  let path = `${base()}/${operation}`, method = operation === "config" ? "PUT" : "POST";
  if (mode === "cloud") { body = {...body, operation, expires_at: Date.now() + 300000}; path = `${base()}/commands`; method = "POST"; }
  message("Saving…");
  const result = await request(path, method, body, true);
  const text = {applied: "Saved", accepted: "Request accepted", pending: "Waiting for device confirmation", conflict: "Settings changed. Try again.", rejected: "The device rejected this setting."}[result.status] || result.status;
  $("command-status").textContent = text; message(text); await refresh(); if (mode === "cloud") await showCommands(); return result;
}
async function showCommands() {
  const result = await request(`${base()}/commands`);
  $("commands").replaceChildren(...result.commands.slice(0, 8).map(command => { const li = document.createElement("li"); li.textContent = `${command.request_id.slice(0, 8)} · ${command.status}`; return li; }));
}
const act = fn => async event => {
  if (event) event.preventDefault();
  const target = event?.submitter || (event?.currentTarget?.tagName === "BUTTON" ? event.currentTarget : null);
  if (target?.classList.contains("is-pending")) return;
  if (target) { target.classList.add("is-pending"); target.setAttribute("aria-busy", "true"); }
  try { await fn(event); } catch (error) { message(error.message); }
  finally { if (target) { target.classList.remove("is-pending"); target.removeAttribute("aria-busy"); } }
};
$("login").addEventListener("submit", act(async event => {
  startupSuperseded = true;
  const values = Object.fromEntries(new FormData(event.target));
  const result = await request("/api/v1/session", "POST", values);
  csrf = result.csrf_token;
  event.target.password.value = "";
  inlineStatus("login-hint", "");
  message("");
  await enter();
}));
$("logout").onclick = act(async () => { if (busy) throw new Error("Wait for history to finish syncing."); if (mode === "ble") {aircueTransport.disconnect(); mode = "cloud";} else {await request("/api/v1/session", "DELETE"); csrf = "";} message(""); showLogin(); });
$("devices").onchange = act(async event => { device = event.target.value; rulesRevision = null; records = await cachedHistory(); renderHistory(); await refresh(); await syncHistory(false); });
$("sync-history").onclick = () => syncHistory(true);
$("cancel-history").onclick = () => { cancelHistory = true; $("sync-history").textContent = "Canceling…"; };
$("chart-metric").onchange = () => { metricChosen = true; renderHistory(); };
document.querySelectorAll("[data-metric]").forEach(button => button.onclick = () => { metricChosen = true; $("chart-metric").value = button.dataset.metric; showView("history"); renderHistory(); });
$("export-filter").oninput = updateExportLink;
$("export-filter").onsubmit = event => { event.preventDefault(); if (updateExportLink()) $("export").click(); };
$("export").onclick = act(async event => {
  if (!updateExportLink()) return;
  if (mode !== "ble") { location.href = $("export").href; return; }
  await exportBluetooth();
});
$("rules").onsubmit = act(event => mutate("config", {patch: Object.fromEntries([...new FormData(event.target)].map(([k, v]) => [k, Number(v)]))}));
$("wifi").onsubmit = act(async event => {
  const patch = Object.fromEntries(new FormData(event.target));
  // Preserve a saved network's password when the user only changes its priority.
  if (!patch.wifi_password && state?.config?.wifi_networks?.includes(patch.ssid)) delete patch.wifi_password;
  await mutate("wifi", {action: "upsert", ...patch}); event.target.wifi_password.value = "";
});
$("sound-toggle").onclick = act(() => mutate("config", {patch: {buzzer_enabled: !state.config.buzzer_enabled}}));
$("uplink").onclick = act(() => mutate("uplink", {enabled: !state.config.uplink_enabled}));
$("mute").onclick = act(() => mutate("mute", {episode_id: state.episode.episode_id, duration_s: 300}));
$("commission").onclick = act(() => mutate("commission", {fresh_air_confirmed: $("fresh-air").checked}));
$("fresh-air").onchange = () => { if (state) paintStatus(); };
$("run-start").onclick = act(() => mutate("run_start", {}));
$("run-stop").onclick = act(() => mutate("run_stop", {}));
$("demo").onclick = act(() => mutate("config", {patch: {demo: !state.config.demo}}));
$("ota").onclick = act(() => mutate("ota", {}));
$("dismiss-message").onclick = () => message("");
document.querySelectorAll("[data-view]").forEach(control => control.addEventListener("click", () => {
  showView(control.dataset.view);
  if (control.dataset.expand) $(control.dataset.expand).open = true;
  if (control.getAttribute("role") !== "tab") $(`panel-${control.dataset.view}`).focus({preventScroll: true});
}));
$("page-tabs").addEventListener("keydown", event => {
  const tabs = [...$("page-tabs").querySelectorAll('[role="tab"]')], index = tabs.indexOf(event.target);
  if (index < 0 || !["ArrowLeft", "ArrowRight", "Home", "End"].includes(event.key)) return;
  event.preventDefault();
  const next = event.key === "Home" ? 0 : event.key === "End" ? tabs.length - 1 : (index + (event.key === "ArrowRight" ? 1 : -1) + tabs.length) % tabs.length;
  tabs[next].focus(); showView(tabs[next].dataset.view);
});
document.querySelectorAll("[data-settings]").forEach(detail => detail.addEventListener("toggle", () => {
  if (detail.open) document.querySelectorAll("[data-settings]").forEach(other => { if (other !== detail) other.open = false; });
}));
// Invalid advanced fields must be visible before native form validation focuses them.
$("rules").addEventListener("invalid", event => { const detail = event.target.closest("details.inline-disclosure"); if (detail) detail.open = true; }, true);
window.addEventListener("popstate", () => { if (!$("dashboard").hidden) showView(location.hash.slice(1), false); });
window.addEventListener("resize", () => drawChart(records.filter(row => row.record_type === "MINUTE").slice(-240)));
setInterval(() => {
  if (state?.sample_age_ms != null && lastResponseAt) $("age").textContent = duration(state.sample_age_ms + performance.now() - lastResponseAt);
  paintLiveAge();
}, 1000);
(async () => {
  try { db = await openCache(); } catch { /* History remains available from the device or server. */ }
  try {
    const info = await request("/api/v1/info");
    if (startupSuperseded || mode === "ble" || connectingBluetooth) return;
    mode = info.mode; device = info.device_id || "";
    try {
      const session = await request("/api/v1/session");
      // A slow startup request must not replace a newer explicit connection.
      if (startupSuperseded || mode === "ble" || connectingBluetooth) return;
      csrf = session.csrf_token; await enter();
    } catch { if (!startupSuperseded && mode !== "ble" && !connectingBluetooth) showLogin(); }
  } catch {
    if (!startupSuperseded && mode !== "ble" && !connectingBluetooth) {
      inlineStatus("login-hint", "AirCue server unavailable.");
      showLogin();
    }
  }
})();

function renderWifi(networks, connected) {
  const signature = JSON.stringify([networks, connected]);
  if (signature === wifiListSignature) return;
  wifiListSignature = signature;
  $("wifi-list").replaceChildren(...networks.map((ssid, index) => {
    const li = document.createElement("li"), title = document.createElement("strong"), actions = document.createElement("div");
    title.textContent = `${ssid}${ssid === connected ? " · Connected" : ""}`;
    const button = (text, fn) => { const node = document.createElement("button"); node.type = "button"; node.className = "secondary compact"; node.textContent = text; node.onclick = act(fn); actions.append(node); return node; };
    button("Edit", () => { $("wifi-editor").open = true; $("wifi").elements.ssid.value = ssid; $("wifi").elements.wifi_password.focus(); });
    button(index === 0 ? "Preferred" : "Prefer", () => mutate("wifi", {action: "prefer", ssid})).disabled = index === 0;
    button("Remove", async () => {
      const reconnect = ssid === connected ? " The device will try another network." : "";
      if (confirm(`Remove “${ssid}”?${reconnect}`)) await mutate("wifi", {action: "delete", ssid});
    });
    li.append(title, actions); return li;
  }));
}
async function connectBluetooth() {
  startupSuperseded = true;
  if (busy) throw new Error("Wait for history to finish syncing.");
  $("connect-ble").disabled = $("switch-transport").disabled = true;
  connectingBluetooth = true;
  clearTimeout(reconnectTimer);
  clearTimeout(timer);
  $("connect-ble").textContent = "Connecting…";
  try {
    if (aircueTransport.connected) aircueTransport.disconnect();
    const status = await aircueTransport.connect(!!aircueTransport.device); mode = "ble"; device = status.device_id; rulesRevision = null;
    inlineStatus("ble-progress", "");
    message("");
    await enter(status);
  } finally {
    connectingBluetooth = false;
    $("connect-ble").disabled = $("switch-transport").disabled = false;
    $("connect-ble").textContent = "Connect via Bluetooth";
  }
}
$("connect-ble").onclick = act(async () => {
  try { await connectBluetooth(); }
  catch (error) { inlineStatus("ble-progress", error.message || "Could not connect."); }
});
$("switch-transport").onclick = act(async () => {
  if (mode !== "ble" || !aircueTransport.connected) return connectBluetooth();
  if (busy) throw new Error("Wait for history to finish syncing.");
  aircueTransport.disconnect(); mode = "cloud"; rulesRevision = null;
  try { const session = await request("/api/v1/session"); csrf = session.csrf_token; await enter(); }
  catch { showLogin(); }
});
async function exportBluetooth() {
  if (busy) throw new Error("Wait for history to finish syncing.");
  busy = true; clearTimeout(timer); $("sync-history").disabled = true;
  try {
    // Reuse the atomic history/cursor cache and fetch only the missing suffix.
    // Local exports include already synchronized records, even after device retention expires.
    let next = await cursor() || "0", page;
    const saved = await cachedHistory(), merged = new Map(saved.map(row => [row.record_id, row]));
    do {
      page = await aircueTransport.request("history", {cursor: next});
      for (const row of page.records) merged.set(row.record_id, row);
      await savePage(page); next = page.next_cursor;
      message(`Preparing CSV (${merged.size} records)…`);
    } while (page.has_more);
    records = [...merged.values()].sort(compareRecords); renderHistory();
    let rows = records;
    const values = new FormData($("export-filter")), from = values.get("from") ? new Date(values.get("from")).getTime() : null, to = values.get("to") ? new Date(values.get("to")).getTime() : null;
    rows = rows.filter(row => (!values.get("run_id") || row.run_id === values.get("run_id")) && ((from == null && to == null) || row.measured_at != null && (from == null || row.measured_at >= from) && (to == null || row.measured_at < to)));
    const columns = ["record_id", "record_type", "run_id", "run_t0", "window_index", "measured_at", "estimated_measured_at", "received_at", "time_quality", "monotonic_ms", "mean", "minimum", "maximum", "valid_count", "expected_count", "valid_ms", "observed_high_ms", "red_valid_ms", "warmup_count", "error_count", "missed_count", "window_ms", "level", "trend", "config_revision", "demo", "partial", "interrupted", "event", "episode_id", "peak", "high_ms", "red_ms", "co2_raw_ppm", "temperature_c", "humidity_rh", "pressure_hpa", "barometer_temperature_c", "co2_raw_count", "barometer_count"];
    const cell = value => `"${String(value ?? "").replaceAll('"', '""')}"`;
    const csv = [columns.join(","), ...rows.map(row => columns.map(key => cell(row[key])).join(","))].join("\r\n") + "\r\n";
    const url = URL.createObjectURL(new Blob([csv], {type: "text/csv;charset=utf-8"})), link = document.createElement("a");
    link.href = url; link.download = `${device}.csv`; document.body.append(link); link.click(); link.remove(); setTimeout(() => URL.revokeObjectURL(url), 60000);
    message(`CSV exported. ${rows.length} records.`);
  } finally { busy = false; $("sync-history").disabled = false; await refresh(); }
}
if ("serviceWorker" in navigator && isSecureContext) {
  navigator.serviceWorker.register("/sw.js").then(() => navigator.serviceWorker.ready).then(() => {
    inlineStatus("offline-ready", "Ready for offline use.");
  }).catch(() => { inlineStatus("offline-ready", "Keep this page open for offline use."); });
}
