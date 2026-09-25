const $ = (id) => document.getElementById(id);
// The log pane lives on details.html; here the traffic goes to the console.
function logln(s) { console.log(s); }

// ---- formatting -----------------------------------------------------------

// Czech decimal comma.
const dec = (n, digits) => Number(n).toFixed(digits).replace('.', ',');

// Readings below this [W] are measurement noise (the panels report a few watts
// at night), shown as a plain zero.
const POWER_NOISE_W = 10;

// Power in W, switching to kW with one decimal from 1 kW up.
// With `signed`, a positive value gets an explicit "+" (battery charging).
function formatPower(w, signed) {
  let n = Number(w);
  if (w == null || isNaN(n)) return "—";
  if (Math.abs(n) < POWER_NOISE_W) n = 0;
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

const boilerWatts = [0, 500, 1000, 2000];
let boilerFault = false; // a fault is logged once, on its rising edge

// Target temperature buttons: each cell is tinted with the tank colour of its
// temperature (the CSS reads it from --c).
document.querySelectorAll(".tgt-btn").forEach((btn) => {
  btn.style.setProperty("--c", tempColor(btn.dataset.t));
});

async function setTarget(celsius) {
  await send({ type: "cmd", name: "set_boiler_target_temp", value: celsius });
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

  // Boiler state first: the house figure below needs to know whether it heats.
  // The boiler input is the phase behind the physical thermostat (on = asking
  // for heat); `tr` is the virtual thermostat saying the tank is at its target.
  const inputOn = !!j.bo;
  const reached = !!j.tr;
  const fault = !!j.bf;
  if (fault && !boilerFault) logln("BOILER FAULT: " + (j.bfr || "(unspecified)"));
  boilerFault = fault;
  const power = j.bp != null ? Number(j.bp) : 0;
  const heating = !fault && inputOn && !reached && power > 0;
  const boilW = heating ? boilerWatts[power] : 0;

  // Flow diagram. The inverter's AC output includes the boiler, so the house
  // gets the rest; without the subtraction 2 kW of sun read as 2 kW house and
  // 2 kW boiler at once.
  const pv = valid && j.pcp != null ? Number(j.pcp) : null;
  const home = valid && j.aw != null ? Math.max(0, Number(j.aw) - boilW) : null;
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

  // Boiler on the diagram: the effective heat request.
  $("c_boil").classList.toggle("on", valid && heating);
  $("tank_edge").style.stroke = heating ? "var(--heat)" : "var(--idle)";
  $("v_boil").textContent = fault ? "porucha" : formatPower(boilW, false);
  $("v_boil").classList.toggle("heat", heating);

  // Target temperature section. "reached" wins over the physical input: once both
  // sensors are above the target it does not matter that the thermostat opened too.
  const target = j.tt != null ? Number(j.tt) : null;
  const low = j.th != null && j.tl != null ? Math.round(Math.min(Number(j.th), Number(j.tl))) : null;
  $("tgt_big").textContent = target == null ? "—" : `${target}°`;
  const st = $("tgt_state"), note = $("tgt_note");
  if (target == null) {
    st.textContent = "—";
    note.textContent = "";
  } else if (reached) {
    st.textContent = "dosaženo";
    note.innerHTML = `teplota nad <b class="num">${target}°</b>`;
  } else if (!inputOn) {
    st.textContent = "vypnul fyzický termostat";
    note.innerHTML = low == null ? "" : `spodní teplota <b class="num">${low}°</b>, fyzický termostat je nastavený níž`;
  } else {
    // Heat is requested; whether it flows depends on the regulation (surplus) or the held power.
    // Without both sensors the target is out of action and the tank heats up to the physical thermostat.
    st.textContent = heating ? "ohřívá" : "žádá teplo";
    note.innerHTML = low == null ? "teplota není k dispozici, hřeje po fyzický termostat"
      : `spodní teplota <b class="num">${low}°</b>, chybí <b class="num">${Math.max(1, target - low)}°</b>`;
  }
  st.classList.toggle("on", target != null && !reached && inputOn);
  // Without both temperatures the target has no effect, so the buttons are locked
  // (the stored target stays highlighted).
  document.querySelectorAll(".tgt-btn").forEach((btn) => {
    btn.setAttribute("aria-pressed", String(Number(btn.dataset.t) === target));
    btn.disabled = target == null || low == null;
  });

  $("manualwarn").hidden = !j.bman;

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
