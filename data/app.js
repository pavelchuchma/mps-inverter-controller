const $ = (id) => document.getElementById(id);
// The log pane lives on details.html; here the traffic goes to the console.
function logln(s) { console.log(s); }

// ---- formatting -----------------------------------------------------------

// Czech decimal comma.
const dec = (n, digits) => Number(n).toFixed(digits).replace('.', ',');

// Power in W, switching to kW with one decimal from 1 kW up.
// With `signed`, a positive value gets an explicit "+" (battery charging).
function formatPower(w, signed) {
  const n = Number(w);
  if (w == null || isNaN(n)) return "—";
  const sign = signed && n > 0 ? "+" : (n < 0 ? "−" : "");
  const a = Math.abs(n);
  return a >= 1000 ? `${sign}${dec(a / 1000, 1)} kW` : `${sign}${Math.round(a)} W`;
}

// Tank colour: a straight blend from blue at 0 °C to red at 60 °C.
function tempColor(t) {
  const k = Math.max(0, Math.min(1, Number(t) / 60));
  const a = [42, 110, 196], b = [212, 62, 44];
  return `rgb(${a.map((c, i) => Math.round(c + (b[i] - c) * k)).join(' ')})`;
}

// ---- temperature display with hysteresis ------------------------------------
// Integer display; a new value is committed only after it holds steady for
// TEMP_STABLE_COUNT consecutive readings (suppresses ±1 °C jitter).
const TEMP_STABLE_COUNT = 10;
const tempState = {}; // { elId: { displayed, candidate, count } }

function updateTemp(elId, raw) {
  if (raw === undefined || raw === null) {
    $(elId).textContent = "—";
    return;
  }
  const v = Math.round(Number(raw));
  let st = tempState[elId];
  if (!st) {
    tempState[elId] = { displayed: v, candidate: v, count: 0 };
    $(elId).textContent = `${v}°`;
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
      $(elId).textContent = `${v}°`;
    }
  } else {
    st.candidate = v;
    st.count = 1;
  }
}

// ---- commands ---------------------------------------------------------------

let boilerPower = 0;
let boilerFault = false;
let boilerInputOn = true; // start enabled; updated from j.bo on first /status
let boilerManual = false; // updated from j.bman on each /status
const boilerLabels = ["Vyp", "500 W", "1000 W", "2000 W"];
const boilerWatts = [0, 500, 1000, 2000];

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

// ---- header: data age + banner ----------------------------------------------

let lastGoodMs = 0;   // wall clock of the last successful /status
let fetchError = "";  // non-empty while the ESP itself is unreachable

// Shown only while the data is stale or the ESP is unreachable; fresh data needs no caption.
const AGE_STALE_S = 10;
function renderAge() {
  const el = $("age");
  const s = lastGoodMs ? Math.max(0, Math.round((Date.now() - lastGoodMs) / 1000)) : null;
  const problem = !!fetchError || s == null || s > AGE_STALE_S;
  el.hidden = !problem;
  if (!problem) return;
  el.textContent = s == null ? "zatím žádná data" : `poslední data před ${s} s`;
}

function setBanner(msg) {
  const el = $("banner");
  el.hidden = !msg;
  if (msg) el.textContent = msg;
}

// ---- render -----------------------------------------------------------------

const RING = 150.8; // circumference of the r=24 inverter ring
const FLOW_MIN_W = 30; // below this a wire is drawn idle

function render(j) {
  // /status uses short keys to minimize GSM payload; see makeStatusJson() in esp_webserver.cpp.
  const valid = !!j.iv;
  const battValid = !!j.bav;
  $("app").classList.toggle("stale", !valid);

  // Flow diagram
  const pv = valid && j.pcp != null ? Number(j.pcp) : null;
  const home = valid && j.aw != null ? Number(j.aw) : null;
  const battW = battValid && j.bv != null && j.bc != null ? Number(j.bv) * Number(j.bc) : null;
  $("v_pv").textContent = formatPower(pv, false);
  $("v_home").textContent = formatPower(home, false);
  $("v_bat").textContent = formatPower(battW, true);
  $("c_pv").classList.toggle("on", pv != null && pv > FLOW_MIN_W);
  $("c_home").classList.toggle("on", home != null && home > FLOW_MIN_W);
  const cb = $("c_bat");
  cb.classList.toggle("on", battW != null && Math.abs(battW) > FLOW_MIN_W);
  cb.classList.toggle("rev", battW != null && battW < 0); // discharging: current runs from the battery to the hub

  // Inverter load ring
  const lp = valid && j.lp != null ? Math.round(Number(j.lp)) : null;
  $("v_lp").textContent = lp == null ? "—" : `${lp} %`;
  $("inv_arc").setAttribute("stroke-dasharray", `${(RING * Math.min(100, lp || 0) / 100).toFixed(1)} ${RING}`);
  $("inv_arc").setAttribute("stroke", lp != null && lp >= 80 ? "var(--err)" : "var(--ink)");

  // Battery bar + row
  const soc = battValid && j.bs != null ? Math.round(Number(j.bs)) : null;
  const guardPct = j.sgarm != null ? Number(j.sgarm) : 15;
  const crit = soc != null && soc <= guardPct, lo = soc != null && soc <= 30;
  $("b_fill").style.width = `${soc == null ? 0 : soc}%`;
  $("b_fill").className = "fill" + (crit ? " crit" : lo ? " lo" : "");
  $("i_batfill").setAttribute("width", soc == null ? 0 : (17 * soc / 100).toFixed(1));
  $("i_batfill").style.fill = crit ? "var(--bat-crit)" : lo ? "var(--bat-lo)" : "var(--bat)";
  $("b_soc").textContent = soc == null ? "—" : `${soc} %`;
  $("b_mode").textContent = battW == null ? "stav neznámý" : battW > FLOW_MIN_W ? "nabíjí se" : battW < -FLOW_MIN_W ? "vybíjí se" : "v klidu";
  $("b_volt").textContent = battValid && j.bv != null && j.bc != null
    ? `${dec(j.bv, 1)} V · ${Number(j.bc) > 0 ? "+" : Number(j.bc) < 0 ? "−" : ""}${dec(Math.abs(Number(j.bc)), 1)} A`
    : "";
  const guard = $("b_guard");
  guard.hidden = !j.sg;
  if (j.sg) {
    guard.style.left = `${guardPct}%`;
    $("b_guard_lbl").textContent = j.sga ? `ochrana ${guardPct} % · aktivní` : `ochrana ${guardPct} %`;
  }

  // Boiler temperatures: hysteresis for the digits, raw value for the colour
  updateTemp("t_h", j.th);
  updateTemp("t_l", j.tl);
  $("g_top").setAttribute("stop-color", j.th == null ? "#9aa39d" : tempColor(j.th));
  $("g_bot").setAttribute("stop-color", j.tl == null ? "#9aa39d" : tempColor(j.tl));

  // Boiler state
  if (j.bo !== undefined) {
    // The boiler input is the phase behind the thermostat, so "on" means the boiler is asking for heat.
    boilerInputOn = !!j.bo;
  }
  $("thermo").textContent = boilerInputOn ? "termostat žádá teplo" : "termostat spokojen";
  $("thermo").classList.toggle("on", boilerInputOn);

  if (j.bman !== undefined) boilerManual = !!j.bman;
  $("manualwarn").hidden = !boilerManual;

  const newFault = !!j.bf;
  if (newFault && !boilerFault) logln("BOILER FAULT: " + (j.bfr || "(unspecified)"));
  boilerFault = newFault;

  if (j.bp !== undefined) boilerPower = Number(j.bp);
  const heating = !boilerFault && boilerInputOn && boilerPower > 0;
  const label = boilerLabels[boilerPower] || "—";
  const big = $("bo_big"), note = $("bo_note");
  big.className = "big num";
  if (boilerFault) {
    big.textContent = "PORUCHA";
    big.classList.add("fault");
    note.textContent = j.bfr || "";
  } else if (!boilerInputOn) {
    // With the thermostat open nothing heats whatever the commanded power. In Manual
    // the held power still matters (it resumes once the thermostat closes), so keep it visible.
    big.textContent = "Vyp";
    note.textContent = boilerManual && boilerPower > 0 ? `ručně ${label}, čeká na termostat` : "voda je teplá";
  } else {
    big.textContent = label;
    if (boilerPower > 0) big.classList.add("heat");
    note.textContent = boilerPower > 0 ? (boilerManual ? "drženo ručně" : "z přebytku panelů") : (boilerManual ? "ručně vypnuto" : "přebytek zatím nestačí");
  }
  $("c_boil").classList.toggle("on", valid && heating);
  $("tank_edge").style.stroke = heating ? "var(--heat)" : "var(--idle)";
  $("v_boil").textContent = boilerFault ? "porucha" : formatPower(heating ? boilerWatts[boilerPower] : 0, false);
  $("v_boil").classList.toggle("heat", heating);

  const btnsDisabled = boilerFault || !boilerInputOn;
  document.querySelectorAll(".boiler-btn").forEach((btn) => {
    btn.disabled = btnsDisabled;
    btn.setAttribute("aria-pressed", String(Number(btn.dataset.p) === boilerPower));
  });

  setBanner(!valid ? "Měnič neodpovídá (RS232)" : "");
}

// ---- polling ----------------------------------------------------------------

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
      fetchError = `HTTP ${resp.status}`;
      setBanner(`ESP odpovědělo chybou ${resp.status}`);
      logln(`HTTP status ${resp.status}`);
      return;
    }
    const j = await resp.json();
    fetchError = "";
    lastGoodMs = Date.now();
    render(j);
  } catch (e) {
    if (e && (e.name === 'AbortError' || e.code === 20)) {
      fetchError = "timeout";
      setBanner(`ESP neodpovídá (timeout ${STATUS_TIMEOUT_MS / 1000} s)`);
      logln(`Fetch timeout (${STATUS_TIMEOUT_MS / 1000}s)`);
    } else {
      fetchError = "fetch";
      setBanner("Spojení s ESP selhalo");
      logln("Fetch error: " + e);
    }
  } finally {
    clearTimeout(to);
    statusInFlight = false;
    renderAge();
  }
}

// Initial fetch and schedule polling
fetchStatus();
setInterval(fetchStatus, 1250);
setInterval(renderAge, 1000);
