"use strict";
window.addEventListener("error", event => {
  if (event.filename && /\/(app|ble)\.js/.test(event.filename)) {
    const progress = document.getElementById("ble-progress");
    if (progress) progress.textContent = "Could not load the page. Refresh to try again. " + (event.message || "");
  }
});
// The BLE transport shares the dashboard's configuration and history protocol.
class AirCueBluetooth {
  constructor(onDisconnect, onProgress = () => {}, onLive = () => {}) {
    this.onProgress = onProgress;
    this.onLive = onLive; this.hasLive = false; this.latestLive = null;
    this.onDisconnect = onDisconnect; this.chain = Promise.resolve(); this.pending = null;
    this.intentionalDisconnect = false;
    this.onValue = event => this.receive(event.target.value);
    this.onLiveValue = event => this.receiveLive(event.target.value);
    this.onLost = () => {
      if (this.pending) { clearTimeout(this.pending.timeout); this.pending.reject(new Error("Bluetooth disconnected. Move closer and try again.")); this.pending = null; }
      this.input = ""; this.hasLive = false; this.latestLive = null; this.onDisconnect?.(this.intentionalDisconnect);
    };
  }
  get connected() { return !!this.device?.gatt.connected; }
  async wait(promise, seconds, message) {
    let timer;
    try { return await Promise.race([promise, new Promise((resolve, reject) => { timer = setTimeout(() => reject(new Error(message)), seconds * 1000); })]); }
    finally { clearTimeout(timer); }
  }
  async connect(reuseDevice = false) {
    const nativeBluetooth = navigator.bluetooth;
    if (!nativeBluetooth || typeof nativeBluetooth.requestDevice !== "function") throw new Error("Use Chrome on Mac or Bluefy on iPhone to connect by Bluetooth.");
    this.intentionalDisconnect = false;
    if (!reuseDevice || !this.device) {
      this.onProgress("Select your AirCue device.");
      // Invoke the native chooser in the original click, before any asynchronous wait.
      // Match the advertised GATT service. A name-only alternative also matches
      // macOS's Classic Bluetooth cache entry, which cannot create a GATT link.
      this.device = await this.wait(nativeBluetooth.requestDevice({filters: [{services: [AirCueBluetooth.service]}], optionalServices: [AirCueBluetooth.service]}), 60, "No device selected. Close the chooser and try again.");
    }
    this.device.removeEventListener("gattserverdisconnected", this.onLost);
    this.device.addEventListener("gattserverdisconnected", this.onLost);
    this.decoder = new TextDecoder(); this.input = "";
    try {
      this.onProgress("Connecting to the device…");
      let server;
      for (let attempt = 0; attempt < 3; ++attempt) {
        try { server = await this.wait(this.device.gatt.connect(), 10, "Connection timed out. Move closer and try again."); break; }
        catch (error) {
          if (attempt === 2 || error.name === "NotAllowedError") throw error;
          this.onProgress("Reconnecting…");
          await new Promise(resolve => setTimeout(resolve, 1000));
        }
      }
      const service = await server.getPrimaryService(AirCueBluetooth.service);
      this.writer = await service.getCharacteristic(AirCueBluetooth.rx);
      this.reader = await service.getCharacteristic(AirCueBluetooth.tx);
      // This authenticated read triggers OS pairing. The PIN is shown only on the OLED.
      this.onProgress("Checking pairing…");
      await this.wait(this.reader.readValue(), 60, "Pairing timed out. Hold button 1 for two seconds and enter the code on the device.");
      this.reader.addEventListener("characteristicvaluechanged", this.onValue);
      await this.reader.startNotifications();
      this.onProgress("Loading readings…");
      const status = await this.request("status");
      // Older firmware keeps working through the regular status request path.
      try {
        this.liveReader = await service.getCharacteristic(AirCueBluetooth.live);
        this.liveReader.addEventListener("characteristicvaluechanged", this.onLiveValue);
        await this.liveReader.startNotifications(); this.hasLive = true;
        this.receiveLive(await this.liveReader.readValue());
      } catch { this.liveReader?.removeEventListener("characteristicvaluechanged", this.onLiveValue); this.liveReader = null; this.hasLive = false; this.latestLive = null; }
      return status;
    } catch (error) { this.disconnect(); throw error; }
  }
  disconnect(intentional = true) {
    this.intentionalDisconnect = intentional;
    this.reader?.removeEventListener("characteristicvaluechanged", this.onValue);
    this.liveReader?.removeEventListener("characteristicvaluechanged", this.onLiveValue);
    if (this.connected) this.device.gatt.disconnect();
    this.device?.removeEventListener("gattserverdisconnected", this.onLost);
    this.onLost();
  }
  receiveLive(value) {
    if (value.byteLength !== 20 || value.getUint8(0) !== 1 || value.getUint8(2) > 3 || value.getUint8(3) > 15) return;
    const mask = value.getUint8(1), levels = ["UNASSESSED", "CLEAR", "WATCH", "HIGH"], trends = ["UNKNOWN", "RISING", "STABLE", "FALLING"];
    const fields = ["co2_raw_ppm", "temperature_c", "humidity_rh", "pressure_hpa", "barometer_temperature_c"], scales = [1, 100, 100, 10, 100], climate = {};
    fields.forEach((field, i) => { climate[field] = mask & (1 << i) ? value.getInt16(4 + i * 2, true) / scales[i] : null; });
    this.latestLive = {climate, ppm: mask & 32 ? value.getUint16(14, true) : null,
      quality: ["COMMISSIONING", "WARMUP", "VALID", "FAULT"][value.getUint8(2)],
      level: levels[value.getUint8(3) & 3], trend: trends[value.getUint8(3) >> 2],
      range_warn: !!(mask & 64), recovering: !!(mask & 128), rawAge: value.getUint16(16, true),
      sequence: value.getUint16(18, true), receivedAt: performance.now()};
    this.onLive(this.latestLive);
  }
  receive(value) {
    this.input += this.decoder.decode(new Uint8Array(value.buffer, value.byteOffset, value.byteLength), {stream: true});
    if (this.input.length > 32768) { this.disconnect(); return; }
    let end;
    while ((end = this.input.indexOf("\n")) >= 0) {
      const line = this.input.slice(0, end); this.input = this.input.slice(end + 1);
      try {
        const reply = JSON.parse(line), pending = this.pending;
        if (!pending || reply.id !== pending.id) continue;
        if (reply.result?.http_status >= 400) {clearTimeout(pending.timeout); this.pending = null; pending.reject(new Error(reply.result.message || "The device rejected this request."));}
        else {
          if (pending.op === "history" && reply.result?.records) {
            const snapshots = new Map((reply.result.config_snapshots || []).map(item => [item.revision, item]));
            reply.result.records = reply.result.records.map(item => {
              if (!item.wire) return item;
              const row = AirCueBluetooth.decodeRecord(item.wire);
              return {...row, run_id: item.run_id, run_t0: item.run_t0, order: item.order, config_snapshot: snapshots.get(row.config_revision) || null};
            });
          }
          clearTimeout(pending.timeout); this.pending = null; pending.resolve(reply.result);
        }
      } catch { this.disconnect(); return; }
    }
  }
  request(op, body = {}) {
    if (op === "history") body = {...body, format: "wire"};
    const task = this.chain.then(() => this.exchange(op, body));
    this.chain = task.catch(() => {}); return task;
  }
  async exchange(op, body) {
    if (!this.connected) throw new Error("Connect to the device first.");
    const id = Array.from(crypto.getRandomValues(new Uint8Array(12)), byte => byte.toString(16).padStart(2, "0")).join("");
    const bytes = new TextEncoder().encode(JSON.stringify({id, op, body}) + "\n");
    if (bytes.length > 1536) throw new Error("Request too large. Save your changes separately.");
    let complete;
    const response = new Promise((resolve, reject) => {
      complete = {id, op, resolve, reject, timeout: setTimeout(() => { reject(new Error("Bluetooth timed out. Reconnecting…")); this.disconnect(false); }, 60000)};
      this.pending = complete;
    });
    // Install a rejection handler before asynchronous writes can fail.
    response.catch(() => {});
    try {
      for (let offset = 0; offset < bytes.length; offset += 20) {
        const chunk = bytes.slice(offset, offset + 20);
        if (this.writer.writeValueWithResponse) await this.writer.writeValueWithResponse(chunk);
        else await this.writer.writeValue(chunk);
      }
    } catch (error) {
      clearTimeout(complete.timeout); if (this.pending === complete) this.pending = null;
      complete.reject(error);
    }
    return response;
  }
  static decodeRecord(wire) {
    if (!/^(?:[0-9a-f]{192}|[0-9a-f]{224})$/.test(wire)) throw new Error("Invalid history record.");
    const raw = Uint8Array.from(wire.match(/../g), pair => parseInt(pair, 16)), view = new DataView(raw.buffer);
    if (!((raw[0] === 1 && raw.length === 96) || (raw[0] === 2 && raw.length === 112 && raw[1] === 1)) || ![1, 2].includes(raw[1])) throw new Error("Unsupported history format.");
    let crc = 0xffffffff;
    for (let i = 0; i < raw.length - 4; ++i) { crc ^= raw[i]; for (let bit = 0; bit < 8; ++bit) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1)); }
    if ((~crc >>> 0) !== view.getUint32(raw.length - 4, true)) throw new Error("Could not verify a history record. Sync history again.");
    const u16 = offset => view.getUint16(offset, true), u32 = offset => view.getUint32(offset, true), i32 = offset => view.getInt32(offset, true);
    const u64 = offset => u32(offset) + u32(offset + 4) * 4294967296;
    const flags = u16(2), sequence = u32(20), boot = wire.slice(8, 40), utc = u64(32);
    const levels = ["UNASSESSED", "CLEAR", "WATCH", "HIGH"], trends = ["UNKNOWN", "RISING", "STABLE", "FALLING"];
    const row = {record_id: `${boot}:${sequence}`, boot_id: boot, sequence, monotonic_ms: u64(24), measured_at: utc || null, received_at: null,
      time_quality: flags & 1 ? "UTC_UNKNOWN" : "SYNCED", demo: !!(flags & 2), partial: !!(flags & 4), interrupted: !!(flags & 8), range_warn: !!(flags & 16), record_type: raw[1] === 1 ? "MINUTE" : "EVENT"};
    if (raw[1] === 1) {
      const valid = u16(52);
      Object.assign(row, {mean: valid ? i32(40) : null, minimum: valid ? i32(44) : null, maximum: valid ? i32(48) : null,
        valid_count: valid, expected_count: u16(54), config_revision: u32(56), valid_ms: u32(60), observed_high_ms: u32(64), red_valid_ms: u32(68), window_ms: u32(72),
        level: levels[raw[76]], trend: trends[raw[77]], warmup_count: u16(78), error_count: u16(80), missed_count: u16(82), window_index: u32(84) === 0xffffffff ? null : u32(84)});
      if (raw[0] === 2) {
        const fields = ["co2_raw_ppm", "temperature_c", "humidity_rh", "pressure_hpa", "barometer_temperature_c"], scales = [1, 100, 100, 10, 100], mask = u16(98);
        fields.forEach((field, i) => { row[field] = mask & (1 << i) ? view.getInt16(88 + i * 2, true) / scales[i] : null; });
        row.co2_raw_count = u16(100); row.barometer_count = u16(102);
      }
    } else {
      const events = ["NONE", "BOOT", "EPISODE_START", "FALLING", "RECOVERED", "CONFIG_CHANGED", "SENSOR_FAULT", "SENSOR_RESTORED", "TIME_ANCHOR", "GAP", "MAINTENANCE", "CONFIG"];
      Object.assign(row, {event: events[raw[40]], level: levels[raw[41]], episode_id: u32(60) ? `${wire.slice(88, 120)}:${u32(60)}` : null, peak: i32(64), high_ms: u32(68), red_ms: u32(72), valid_ms: u32(76), config_revision: u32(80), extra: u64(84)});
      if (raw[40] === 2) row.episode_id = row.record_id;
    }
    return row;
  }
}
AirCueBluetooth.service = "6d7ac4e0-8eb2-4a80-9a9b-3a5c6f420001";
AirCueBluetooth.rx = "6d7ac4e0-8eb2-4a80-9a9b-3a5c6f420002";
AirCueBluetooth.tx = "6d7ac4e0-8eb2-4a80-9a9b-3a5c6f420003";
AirCueBluetooth.live = "6d7ac4e0-8eb2-4a80-9a9b-3a5c6f420004";
window.AirCueBluetooth = AirCueBluetooth;
