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
let chargerOn = false;
let boilerPower = 0;
const boilerLabels = ["OFF", "500W", "1000W", "2000W"];

async function toggleCharger() {
  const newState = !chargerOn;
  await send({ type: "cmd", name: "set_charger", value: newState });
  await fetchStatus();
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

    if (j.type === "status") {
      const valid = !!j.valid;

      $("tempH").textContent = (j.temp_h !== undefined && j.temp_h !== null) ? Number(j.temp_h).toFixed(1) : "—";
      $("tempL").textContent = (j.temp_l !== undefined && j.temp_l !== null) ? Number(j.temp_l).toFixed(1) : "—";
      $("ac_out_voltage").textContent = valid && j.ac_out_voltage !== undefined && j.ac_out_voltage !== null ? Number(j.ac_out_voltage).toFixed(1) : "—";
      $("ac_out_frequency").textContent = valid && j.ac_out_frequency !== undefined && j.ac_out_frequency !== null ? Number(j.ac_out_frequency).toFixed(2) : "—";
      $("ac_apparent_va").textContent = valid && j.ac_apparent_va !== undefined && j.ac_apparent_va !== null ? String(Math.round(j.ac_apparent_va)) : "—";
      $("ac_active_w").textContent = valid && j.ac_active_w !== undefined && j.ac_active_w !== null ? String(Math.round(j.ac_active_w)) : "—";
      $("load_percent").textContent = valid && j.load_percent !== undefined && j.load_percent !== null ? String(Math.round(j.load_percent)) : "—";
      $("batt_voltage").textContent = valid && j.batt_voltage !== undefined && j.batt_voltage !== null ? Number(j.batt_voltage).toFixed(2) : "—";
      $("batt_charge_current").textContent = valid && j.batt_charge_current !== undefined && j.batt_charge_current !== null ? Number(j.batt_charge_current).toFixed(2) : "—";
      $("batt_soc").textContent = valid && j.batt_soc !== undefined && j.batt_soc !== null ? Number(j.batt_soc).toFixed(1) : "—";
      $("heatsink_temp").textContent = valid && j.heatsink_temp !== undefined && j.heatsink_temp !== null ? Number(j.heatsink_temp).toFixed(1) : "—";
      $("pv_input_current").textContent = valid && j.pv_input_current !== undefined && j.pv_input_current !== null ? Number(j.pv_input_current).toFixed(2) : "—";
      $("pv_input_voltage").textContent = valid && j.pv_input_voltage !== undefined && j.pv_input_voltage !== null ? Number(j.pv_input_voltage).toFixed(2) : "—";
      $("batt_voltage_from_scc").textContent = valid && j.batt_voltage_from_scc !== undefined && j.batt_voltage_from_scc !== null ? Number(j.batt_voltage_from_scc).toFixed(2) : "—";
      $("batt_discharge_current").textContent = valid && j.batt_discharge_current !== undefined && j.batt_discharge_current !== null ? Number(j.batt_discharge_current).toFixed(2) : "—";
      $("pv_charging_power").textContent = valid && j.pv_charging_power !== undefined && j.pv_charging_power !== null ? String(Math.round(j.pv_charging_power)) : "—";
      $("g_inverter_mode_code").textContent = j.g_inverter_mode_code !== undefined && j.g_inverter_mode_code !== null ? j.g_inverter_mode_code : "—";
      $("g_inverter_mode_name").textContent = j.g_inverter_mode_name !== undefined && j.g_inverter_mode_name !== null ? j.g_inverter_mode_name : "—";

      if (j.charger_on !== undefined) {
        chargerOn = !!j.charger_on;
        $("charger_status").textContent = chargerOn ? "ON" : "OFF";
        $("charger_status").style.color = chargerOn ? "#22c55e" : "#ef4444";
        $("charger_btn").textContent = chargerOn ? "Turn OFF" : "Turn ON";
      }

      if (j.boiler_power !== undefined) {
        boilerPower = j.boiler_power;
        $("boiler_status").textContent = boilerLabels[boilerPower] || "—";
        $("boiler_status").style.color = boilerPower > 0 ? "#ef4444" : "";
        document.querySelectorAll(".boiler-btn").forEach((btn, i) => {
          btn.style.background = (i === boilerPower) ? "#dbeafe" : "";
          btn.style.fontWeight = (i === boilerPower) ? "700" : "";
        });
      }

      if (!resetReasonLogged && (j.reset_reason !== undefined || j.reset_reason_str !== undefined)) {
        const rr = (j.reset_reason_str || "").toString();
        const rrn = (j.reset_reason !== undefined) ? String(j.reset_reason) : "";
        const msg = rr || rrn ? `ESP reset reason: ${rr}${rr && rrn ? ` (${rrn})` : rrn ? rrn : ""}` : "ESP reset reason: (unknown)";
        logln(msg);
        resetReasonLogged = true;
      }

      return;
    }
    logln("MSG: " + JSON.stringify(j));
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
