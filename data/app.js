const $ = (id) => document.getElementById(id);
function logln(s) {
  const el = $("log");
  el.textContent = (el.textContent === "—" ? "" : el.textContent + "\n") + s;
  el.scrollTop = el.scrollHeight;
}
function setConn(ok, msg) {
  const el = $("conn");
  el.textContent = msg;
  el.className = "pill " + (ok ? "ok" : "err");
}

// Format byte counts (e.g. from /proc/net/dev) using base-1024 units.
function formatBytes(b) {
  if (b == null || isNaN(b)) return "—";
  const n = Number(b);
  const KB = 1024, MB = 1024 * 1024, GB = 1024 * 1024 * 1024;
  if (n < KB) return `${n} B`;
  if (n < MB) return `${Math.round(n / KB)} KB`;
  if (n < GB) return `${Math.round(n / MB)} MB`;
  return `${(n / GB).toFixed(2)} GB`;
}

// Format seconds into "Ns" / "NmNs" / "NhNmNs" for stale-suffix display.
function formatStaleSecs(s) {
  const total = Math.max(0, Math.round(Number(s) || 0));
  if (total < 60) return `${total}s`;
  if (total < 3600) {
    const m = Math.floor(total / 60);
    const ss = total % 60;
    return `${m}m${String(ss).padStart(2, '0')}s`;
  }
  const h = Math.floor(total / 3600);
  const m = Math.floor((total % 3600) / 60);
  return `${h}h${String(m).padStart(2, '0')}m`;
}

// Apply a value + optional "stale: Xs" suffix to one of the phone tiles.
// When stale, also flag the card so its value text fades to gray.
function applyPhoneTile(cardId, valueId, suffixId, text, staleSecs, threshold) {
  const card = $(cardId);
  $(valueId).textContent = text;
  if (text === "—") {
    $(suffixId).textContent = "";
    card.classList.add("stale");
    return;
  }
  const isStale = Number(staleSecs) > threshold;
  $(suffixId).textContent = isStale ? `stale: ${formatStaleSecs(staleSecs)}` : "";
  card.classList.toggle("stale", isStale);
}

// Format milliseconds (e.g. from millis()) to HH:MM:SS
function formatMsToHMS(ms) {
  const totalSec = Math.floor(Number(ms || 0) / 1000);
  const h = Math.floor(totalSec / 3600);
  const m = Math.floor((totalSec % 3600) / 60);
  const s = totalSec % 60;
  const hh = String(h).padStart(2, '0');
  const mm = String(m).padStart(2, '0');
  const ss = String(s).padStart(2, '0');
  return `${hh}:${mm}:${ss}`;
}

let resetReasonLogged = false;
let boilerPower = 0;
let boilerFault = false;
let boilerInputOn = true; // start enabled; updated from j.bo on first /status
const boilerLabels = ["OFF", "500W", "1000W", "2000W"];

// Temperature: integer display with hysteresis — a new value is committed only
// after it holds steady for TEMP_STABLE_COUNT consecutive readings (suppresses ±1 °C jitter).
const TEMP_STABLE_COUNT = 10;
const tempState = {}; // { elId: { displayed, candidate, count } }

function updateTemp(elId, raw) {
  if (raw === undefined || raw === null) return;
  const v = Math.round(Number(raw));
  let st = tempState[elId];
  if (!st) {
    tempState[elId] = { displayed: v, candidate: v, count: 0 };
    $(elId).textContent = String(v);
    return;
  }
  if (v === st.displayed) {
    st.candidate = v;
    st.count = 0;
    return;
  }
  if (v === st.candidate) {
    if (++st.count >= TEMP_STABLE_COUNT) {
      st.displayed = v;
      st.count = 0;
      $(elId).textContent = String(v);
    }
  } else {
    st.candidate = v;
    st.count = 1;
  }
}

async function clearLog() {
  if (!confirm("Clear /app.log on ESP?")) return;
  await send({ type: "cmd", name: "clear_log" });
}

async function restartDevice() {
  if (!confirm("Restart ESP?")) return;
  await send({ type: "cmd", name: "restart" });
}

async function setBoiler(level) {
  await send({ type: "cmd", name: "set_boiler", value: level });
  await fetchStatus();
}

async function send(obj) {
  const s = JSON.stringify(obj);
  logln("SEND: " + s);
  try {
    const resp = await fetch('/cmd', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: s
    });
    const txt = await resp.text();
    logln("RX: " + txt);
    if (resp.ok) {
      try { const j = JSON.parse(txt); if (j.msg) logln("ACK: " + j.msg); }
      catch (e) { }
    }
  } catch (e) {
    logln("ERR send: " + e);
  }
}

async function fetchStatus() {
  // Add a 1s timeout to the status fetch
  const ctrl = (typeof AbortController !== 'undefined') ? new AbortController() : null;
  const to = setTimeout(() => {
    try { ctrl && ctrl.abort(); } catch (_) {/* noop */ }
  }, 1000);
  try {
    const resp = await fetch('/status', { cache: 'no-store', signal: ctrl ? ctrl.signal : undefined });
    if (!resp.ok) {
      setConn(false, `HTTP ${resp.status}`);
      logln(`HTTP status ${resp.status}`);
      return;
    }
    const j = await resp.json();
    setConn(true, "HTTP OK");

    // /status response uses short keys to minimize GSM payload; see makeStatusJson() in esp_webserver.cpp for the mapping.
    const valid = !!j.iv;
    const battValid = !!j.bav;

    updateTemp("tempH", j.th);
    updateTemp("tempL", j.tl);
    $("ac_out_voltage").textContent = valid && j.av !== undefined && j.av !== null ? Number(j.av).toFixed(1) : "—";
    $("ac_active_w").textContent = valid && j.aw !== undefined && j.aw !== null ? String(Math.round(j.aw)) : "—";
    $("load_percent").textContent = valid && j.lp !== undefined && j.lp !== null ? String(Math.round(j.lp)) : "—";
    $("batt_voltage").textContent = battValid && j.bv !== undefined && j.bv !== null ? Number(j.bv).toFixed(2) : "—";
    $("batt_current").textContent = battValid && j.bc !== undefined && j.bc !== null ? String(Math.round(j.bc)) : "—";
    $("batt_soc").textContent = battValid && j.bs !== undefined && j.bs !== null ? String(Math.round(j.bs)) : "—";
    $("heatsink_temp").textContent = valid && j.ht !== undefined && j.ht !== null ? String(Math.round(j.ht)) : "—";
    $("pv_input_current_batt").textContent = valid && j.pi !== undefined && j.pi !== null ? String(Math.round(j.pi)) : "—";
    $("pv_input_voltage").textContent = valid && j.piv !== undefined && j.piv !== null ? Number(j.piv).toFixed(1) : "—";
    $("pv_charging_power").textContent = valid && j.pcp !== undefined && j.pcp !== null ? String(Math.round(j.pcp)) : "—";
    $("batt_mode").textContent = battValid && j.bm !== undefined && j.bm !== null && j.bm !== "" ? j.bm : "—";

    // Phone tiles: battery, mobile-data traffic.
    const phoneValid = !!j.phv;
    if (!phoneValid) {
      applyPhoneTile("phone_battery_card", "phone_battery_v", "phone_battery_stale", "—", 0, 0);
      applyPhoneTile("phone_rmnet_card",   "phone_rmnet_v",   "phone_rmnet_stale",   "—", 0, 0);
    } else {
      const pct = j.phbp;
      const statusRaw = (j.phbs || "").toString();
      let battText = `${pct}%`;
      if (j.phbc !== undefined && j.phbc !== null) {
        const ma = Number(j.phbc);
        const sign = ma > 0 ? "+" : "";
        battText += `\n${sign}${ma.toFixed(0)} mA`;
      }
      applyPhoneTile("phone_battery_card", "phone_battery_v", "phone_battery_stale",
                     battText, j.phbss, 120);

      const rx = Number(j.phrx) || 0;
      const tx = Number(j.phtx) || 0;
      const totalMb = Math.round((rx + tx) / (1024 * 1024));
      const rmnetText = `${totalMb.toString().replace(/\B(?=(\d{3})+(?!\d))/g, ' ')} MB`;
      applyPhoneTile("phone_rmnet_card", "phone_rmnet_v", "phone_rmnet_stale",
                     rmnetText, j.phns, 120);
    }

    if (j.co !== undefined) {
      const on = !!j.co;
      $("charger_status").textContent = on ? "ON" : "OFF";
      $("charger_status").style.color = on ? "#22c55e" : "#ef4444";
    }

    if (j.bo !== undefined) {
      boilerInputOn = !!j.bo;
      $("boiler_on").textContent = boilerInputOn ? "ON" : "OFF";
      $("boiler_on").style.color = boilerInputOn ? "#22c55e" : "#ef4444";
    }

    const newFault = !!j.bf;
    if (newFault && !boilerFault) {
      const reason = j.bfr || "(unspecified)";
      logln("BOILER FAULT: " + reason);
    }
    boilerFault = newFault;

    if (j.bp !== undefined) {
      boilerPower = j.bp;
      const statusEl = $("boiler_status");
      if (boilerFault) {
        statusEl.textContent = "ERROR";
        statusEl.style.color = "#ffffff";
        statusEl.style.background = "#ef4444";
        statusEl.style.fontWeight = "700";
        statusEl.style.padding = "0 6px";
      } else {
        statusEl.textContent = boilerLabels[boilerPower] || "—";
        statusEl.style.color = boilerPower > 0 ? "#ef4444" : "";
        statusEl.style.background = "";
        statusEl.style.fontWeight = "";
        statusEl.style.padding = "";
      }
      const btnsDisabled = boilerFault || !boilerInputOn;
      document.querySelectorAll(".boiler-btn").forEach((btn, i) => {
        btn.disabled = btnsDisabled;
        btn.style.opacity = btnsDisabled ? "0.4" : "";
        btn.style.cursor = btnsDisabled ? "not-allowed" : "";
        btn.style.background = (!btnsDisabled && i === boilerPower) ? "#dbeafe" : "";
        btn.style.fontWeight = (!btnsDisabled && i === boilerPower) ? "700" : "";
      });
    }

    if (!resetReasonLogged && (j.rr !== undefined || j.rrs !== undefined)) {
      const rr = (j.rrs || "").toString();
      const rrn = (j.rr !== undefined) ? String(j.rr) : "";
      const msg = rr || rrn ? `ESP reset reason: ${rr}${rr && rrn ? ` (${rrn})` : rrn ? rrn : ""}` : "ESP reset reason: (unknown)";
      logln(msg);
      resetReasonLogged = true;
    }
  } catch (e) {
    if (e && (e.name === 'AbortError' || e.code === 20)) {
      setConn(false, "Timeout 1s");
      logln("Fetch timeout (1s)");
    } else {
      setConn(false, "Fetch error");
      logln("Fetch error: " + e);
    }
  } finally {
    clearTimeout(to);
  }
}

// Initial fetch and schedule polling
fetchStatus();
setInterval(fetchStatus, 1250);
