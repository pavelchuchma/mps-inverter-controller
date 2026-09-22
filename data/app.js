const $ = (id) => document.getElementById(id);
// The log pane lives on details.html; here the traffic goes to the console.
function logln(s) { console.log(s); }
// Connection pill: shown only while something is wrong, hidden on a good poll.
function setConn(ok, msg) {
  const el = $("conn");
  el.textContent = ok ? "" : msg;
  el.hidden = ok;
}

// Format a power in W, switching to kW with one decimal from 1 kW up.
// With `signed`, a positive value gets an explicit "+" (battery charging).
function formatPower(w, signed) {
  const n = Number(w);
  if (w == null || isNaN(n)) return "—";
  const sign = signed && n > 0 ? "+" : "";
  if (Math.abs(n) >= 1000) return `${sign}${(n / 1000).toFixed(1)}kW`;
  return `${sign}${Math.round(n)}W`;
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

let boilerPower = 0;
let boilerFault = false;
let boilerInputOn = true; // start enabled; updated from j.bo on first /status
let boilerManual = false; // updated from j.bman on each /status
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

async function setBoiler(level) {
  await send({ type: "cmd", name: "set_boiler", value: level });
  await fetchStatus();
}

// Toggle Manual/Auto. Not gated on the boiler input like the power buttons:
// Manual + OFF must be selectable while the thermostat is open.
async function toggleBoilerManual() {
  const on = !boilerManual;
  if (on && !confirm("Switch boiler to Manual? The selected power is held until you change it — no automatic regulation. It ends only via the thermostat, an empty battery, or switching back to Auto.")) return;
  await send({ type: "cmd", name: "set_boiler_manual", value: on ? 1 : 0 });
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

const STATUS_TIMEOUT_MS = 3000;
let statusInFlight = false;

async function fetchStatus() {
  // The timeout is longer than the poll interval, so skip a tick while the
  // previous request is still pending rather than piling requests on the ESP.
  if (statusInFlight) return;
  statusInFlight = true;
  const ctrl = (typeof AbortController !== 'undefined') ? new AbortController() : null;
  const to = setTimeout(() => {
    try { ctrl && ctrl.abort(); } catch (_) {/* noop */ }
  }, STATUS_TIMEOUT_MS);
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
    $("load_percent").textContent = valid && j.lp !== undefined && j.lp !== null ? String(Math.round(j.lp)) : "—";

    // Summary tiles: "78% +334W" (SoC + signed battery power from the Pylontech
    // console, V*A) and the PV power from the roof as the inverter reports it.
    const socOk = battValid && j.bs !== undefined && j.bs !== null;
    const battPowerOk = battValid && j.bv !== undefined && j.bv !== null && j.bc !== undefined && j.bc !== null;
    $("batt_soc_v").textContent = socOk ? `${Math.round(j.bs)}%` : "—";
    $("batt_power_v").textContent = battPowerOk ? formatPower(Number(j.bv) * Number(j.bc), true) : "";
    $("pv_power").textContent = valid && j.pcp !== undefined && j.pcp !== null ? formatPower(j.pcp, false) : "—";

    if (j.bo !== undefined) {
      // Thermostat indicator: the boiler input is the phase behind the
      // thermostat, so "on" means the boiler is asking for heat.
      boilerInputOn = !!j.bo;
      $("boiler_thermo").classList.toggle("on", boilerInputOn);
    }

    if (j.bman !== undefined) {
      boilerManual = !!j.bman;
      const modeBtn = $("boiler_mode_btn");
      modeBtn.textContent = boilerManual ? "Manual" : "Auto";
      modeBtn.classList.toggle("manual", boilerManual);
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
        // Effective state: with the thermostat open nothing heats whatever the
        // commanded power, so say so. In Manual the held power still matters
        // (it resumes once the thermostat closes), so keep it visible.
        const label = boilerLabels[boilerPower] || "—";
        if (!boilerInputOn) {
          statusEl.textContent = boilerManual && boilerPower > 0 ? `OFF · ${label}` : "OFF";
          statusEl.style.color = "";
        } else {
          statusEl.textContent = label;
          statusEl.style.color = boilerPower > 0 ? "#ef4444" : "";
        }
        statusEl.style.background = "";
        statusEl.style.fontWeight = "";
        statusEl.style.padding = "";
      }
      $("boiler_card").classList.toggle("stale", !boilerFault && !boilerInputOn);
      const btnsDisabled = boilerFault || !boilerInputOn;
      document.querySelectorAll(".boiler-btn").forEach((btn, i) => {
        btn.disabled = btnsDisabled;
        btn.style.opacity = btnsDisabled ? "0.4" : "";
        btn.style.cursor = btnsDisabled ? "not-allowed" : "";
        btn.style.background = (!btnsDisabled && i === boilerPower) ? "#dbeafe" : "";
        btn.style.fontWeight = (!btnsDisabled && i === boilerPower) ? "700" : "";
      });
    }

  } catch (e) {
    if (e && (e.name === 'AbortError' || e.code === 20)) {
      setConn(false, `Timeout ${STATUS_TIMEOUT_MS / 1000}s`);
      logln(`Fetch timeout (${STATUS_TIMEOUT_MS / 1000}s)`);
    } else {
      setConn(false, "Fetch error");
      logln("Fetch error: " + e);
    }
  } finally {
    clearTimeout(to);
    statusInFlight = false;
  }
}

// Initial fetch and schedule polling
fetchStatus();
setInterval(fetchStatus, 1250);
