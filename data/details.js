const $ = (id) => document.getElementById(id);

// Everything here comes from three endpoints:
//   /status?full=1  — the ESP's own view (short keys, see makeStatusJson()),
//   /can            — decoded BMS frames and link counters,
//   /inv_config     — raw QPIRI etc.; polled slowly, it costs six serial round trips.
const STATUS_TIMEOUT_MS = 3000;
const CONFIG_INTERVAL_MS = 60000;
let statusInFlight = false;
let lastStatus = null;
let lastCan = null;
let lastCfg = null;
let resetReasonLogged = false;
// For the CAN rx rate: counter and time of the previous /can poll.
let prevCanRx = null;
let prevCanRxMs = 0;
let canRxRate = null;

function logln(s) {
  const el = $("log");
  el.textContent = (el.textContent === "—" ? "" : el.textContent + "\n") + s;
  el.scrollTop = el.scrollHeight;
}
function setConn(ok, msg) {
  const el = $("conn");
  el.textContent = ok ? "" : msg;
  el.hidden = ok;
}

const has = (v) => v !== undefined && v !== null;
const num = (v, d = 0, unit = "") => has(v) && !isNaN(Number(v)) ? `${Number(v).toFixed(d)}${unit}` : "—";
const onOff = (v) => has(v) ? (v ? "ON" : "OFF") : "—";
function formatPower(w, signed) {
  const n = Number(w);
  if (!has(w) || isNaN(n)) return "—";
  const sign = signed && n > 0 ? "+" : "";
  if (Math.abs(n) >= 1000) return `${sign}${(n / 1000).toFixed(2)} kW`;
  return `${sign}${Math.round(n)} W`;
}
function formatBytes(b) {
  if (!has(b) || isNaN(b)) return "—";
  const n = Number(b);
  const KB = 1024, MB = KB * 1024, GB = MB * 1024;
  if (n < KB) return `${n} B`;
  if (n < MB) return `${Math.round(n / KB)} KB`;
  if (n < GB) return `${Math.round(n / MB)} MB`;
  return `${(n / GB).toFixed(2)} GB`;
}
function formatDuration(ms) {
  const t = Math.floor(Number(ms || 0) / 1000);
  const d = Math.floor(t / 86400);
  const h = Math.floor((t % 86400) / 3600);
  const m = Math.floor((t % 3600) / 60);
  const s = t % 60;
  return (d ? `${d}d ` : "") + `${String(h).padStart(2, "0")}:${String(m).padStart(2, "0")}:${String(s).padStart(2, "0")}`;
}
function formatStaleSecs(s) {
  const total = Math.max(0, Math.round(Number(s) || 0));
  if (total < 60) return `${total}s`;
  if (total < 3600) return `${Math.floor(total / 60)}m${String(total % 60).padStart(2, "0")}s`;
  return `${Math.floor(total / 3600)}h${String(Math.floor((total % 3600) / 60)).padStart(2, "0")}m`;
}
function flags(list) {
  // list: [label, state] with state true/false/"bad"; returns HTML.
  return list.map(([label, st]) => {
    const cls = st === "bad" ? "bad" : (st ? "on" : "off");
    return `<span class="flag ${cls}">${label}</span>`;
  }).join("");
}

const QMOD_NAMES = { P: "Power on", S: "Standby", L: "Line", B: "Battery", F: "Fault", H: "Power saving" };
const BOILER_W = [0, 500, 1000, 2000];

// Pylontech 0x359: protection = bytes 0-1, alarm = bytes 2-3 (same bit layout).
const PROT_BITS = [
  [0x0002, "over V"], [0x0004, "under V"], [0x0008, "over T"], [0x0010, "under T"],
  [0x0080, "dchg over I"], [0x0100, "chg over I"], [0x0800, "system err"],
];
const ALARM_BITS = [
  [0x0002, "high V"], [0x0004, "low V"], [0x0008, "high T"], [0x0010, "low T"],
  [0x0080, "dchg high I"], [0x0100, "chg high I"], [0x0800, "comm fail"],
];
function bitFlags(value, table) {
  if (!has(value)) return "—";
  const v = Number(value);
  if (v === 0) return flags([["OK", true]]);
  const known = table.filter(([m]) => v & m).map(([, l]) => [l, "bad"]);
  const unknownMask = table.reduce((acc, [m]) => acc & ~m, v);
  if (unknownMask) known.push([`0x${unknownMask.toString(16)}`, "bad"]);
  return flags(known);
}

// One row: label, value (text or {html}), source tag, optional stale flag.
function row(label, value, src, stale) {
  const v = value && typeof value === "object" && "html" in value ? value.html : escapeHtml(value);
  return `<tr class="${stale ? "stale" : ""}"><td class="n">${label}</td><td class="val">${v}</td><td class="src">${src || ""}</td></tr>`;
}
function escapeHtml(s) {
  return String(s).replace(/[&<>]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;" }[c]));
}
function fill(id, rows) { $(id).innerHTML = rows.join(""); }

// QPIRI tokens the limits section needs (indices per settings.js header comment).
function parseCfg(cfg) {
  if (!cfg || !cfg.qpiri) return null;
  const t = cfg.qpiri.trim().split(/\s+/);
  const f = (i) => t.length > i ? Number(t[i]) : NaN;
  return { lvd_v: f(9), bulk_v: f(10), float_v: f(11), max_charge_a: f(14), redischarge_v: f(22) };
}

function render() {
  const j = lastStatus;
  if (!j) return;
  const c = lastCan || {};
  const cd = c.decoded || {};
  const cl = c.link || {};
  const inv = !!j.iv, bat = !!j.bav, can = !!j.cav;
  const cfg = parseCfg(lastCfg);
  // Source tags: where the value physically comes from, not which endpoint carried it.
  const INV = "inv", CFG = "inv cfg", CAN = "bat can", CON = "bat console", ESP = "esp", PH = "phone";

  fill("power", [
    row("PV výkon", formatPower(j.pcp), INV, !inv),
    row("AC činný výkon", formatPower(j.aw), INV, !inv),
    row("AC zdánlivý výkon", num(j.ava, 0, " VA"), INV, !inv),
    row("Výkon baterie (konzole, V·A)", bat && has(j.bv) && has(j.bc) ? formatPower(j.bv * j.bc, true) : "—", CON, !bat),
    row("Výkon baterie (CAN, V·A)", has(cd.voltage_v) && has(cd.current_a) ? formatPower(cd.voltage_v * cd.current_a, true) : "—", CAN, !can),
    row("Zátěž", num(j.lp, 0, " %"), INV, !inv),
  ]);

  fill("current", [
    row("Proud baterie (konzole)", num(j.bc, 1, " A"), CON, !bat),
    row("Proud baterie (CAN)", num(j.cbc, 1, " A"), CAN, !can),
    row("Nabíjecí proud dle měniče", num(j.bca, 0, " A"), INV, !inv),
    row("Vybíjecí proud dle měniče", num(j.bda, 0, " A"), INV, !inv),
    row("PV proud do baterie", num(j.pi, 0, " A"), INV, !inv),
  ]);

  fill("voltage", [
    row("Napětí baterie (konzole)", num(j.bv, 2, " V"), CON, !bat),
    row("Napětí baterie (CAN)", num(j.cbv, 2, " V"), CAN, !can),
    row("Napětí baterie dle měniče", num(j.ibv, 2, " V"), INV, !inv),
    row("Napětí baterie dle SCC", num(j.bvs, 2, " V"), INV, !inv),
    row("Požadované nabíjecí napětí (BMS)", num(j.chv, 1, " V"), CAN, !can),
    row("PV vstupní napětí", num(j.piv, 1, " V"), INV, !inv),
    row("AC výstupní napětí", num(j.av, 1, " V"), INV, !inv),
  ]);

  const mode = j.im ? `${j.im} (${j.imn || QMOD_NAMES[j.im] || "?"})` : "—";
  fill("soc", [
    row("SoC (konzole)", num(j.bs, 0, " %"), CON, !bat),
    row("SoC (CAN)", num(cd.soc, 0, " %"), CAN, !can),
    row("SoC dle měniče", num(j.ibs, 0, " %"), INV, !inv),
    row("SoH (BMS)", num(j.soh, 0, " %"), CAN, !can),
    row("Režim baterie", bat && j.bm ? j.bm : "—", CON, !bat),
    row("Režim měniče (QMOD)", mode, INV, !inv),
    row("Výstup měniče (QPIGS b4)", inv && has(j.lo) ? (j.lo ? "ZAPNUT" : "VYPNUT") : "—", INV, !inv),
    row("Stavové bity (QPIGS)", inv && has(j.dsb) ? `${Number(j.dsb).toString(2).padStart(8, "0")} / ${Number(j.asb).toString(2).padStart(3, "0")}` : "—", INV, !inv),
  ]);

  fill("temp", [
    row("Chladič měniče", num(j.ht, 0, " °C"), INV, !inv),
    row("Baterie (konzole)", num(j.bt, 1, " °C"), CON, !bat),
    row("Baterie (CAN)", num(cd.temp_c, 1, " °C"), CAN, !can),
    row("Boiler H / L", `${num(j.th, 1)} / ${num(j.tl, 1)} °C`, ESP),
  ]);

  const guard = j.sg ? (j.sga ? "zapnut, ARMED" : "zapnut, klid") : "vypnut";
  fill("boiler", [
    row("Výkon boileru", has(j.bp) ? `${BOILER_W[j.bp] ?? "?"} W` : "—", ESP),
    row("Režim", has(j.bman) ? (j.bman ? "Manual" : "Auto") : "—", ESP),
    row("Vstup (termostat)", onOff(j.bo), ESP),
    row("Porucha", j.bf ? `ANO: ${j.bfr || "(bez důvodu)"}` : "ne", ESP),
    row("SoC guard", guard, ESP),
    row("SoC guard: cílové LVD", num(j.sgl, 1, " V") + (has(j.sgok) ? (j.sgok ? " (zapsáno)" : " (zápis selhal)") : ""), ESP),
    row("SoC guard: prahy arm / disarm", has(j.sgarm) ? `${j.sgarm} % / ${j.sgdis} %` : "—", ESP),
  ]);

  fill("limits", [
    row("CCL / DCL (BMS)", can ? `${num(j.ccl, 0)} / ${num(j.dcl, 0)} A` : "—", CAN, !can),
    row("Max. nabíjecí proud měniče", cfg ? num(cfg.max_charge_a, 0, " A") : "—", CFG),
    row("Bulk / Float (měnič)", cfg ? `${num(cfg.bulk_v, 1)} / ${num(cfg.float_v, 1)} V` : "—", CFG),
    row("LVD / Redischarge (měnič)", cfg ? `${num(cfg.lvd_v, 1)} / ${num(cfg.redischarge_v, 1)} V` : "—", CFG),
    row("Nabíjecí napětí (BMS)", num(j.chv, 1, " V"), CAN, !can),
  ]);

  const rq = cd.request_flags;
  fill("can", [
    row("Link", has(c.valid) ? (c.valid ? "OK" : "STALE") : "—", CAN, !can),
    row("Stav linky / poslední rámec", has(cl.state) ? `${cl.state} / ${num(cl.last_rx_age_ms, 0, " ms")}` : "—", CAN, !can),
    row("rx rámců / rate", has(cl.rx) ? `${cl.rx}${canRxRate !== null ? ` / ${canRxRate.toFixed(1)} Hz` : ""}` : "—", CAN),
    row("bus_err / missed / rec / tec", has(cl.bus_err) ? `${cl.bus_err} / ${cl.missed} / ${cl.rec} / ${cl.tec}` : "—", CAN),
    row("recoveries / tx_failed", has(cl.recoveries) ? `${cl.recoveries} / ${cl.tx_failed}` : "—", CAN),
    row("Moduly", num(cd.modules, 0), CAN, !can),
    row("Request flagy", has(rq) ? { html: flags([
      ["chg_en", !!(rq & 0x80)], ["dchg_en", !!(rq & 0x40)],
      ["force_chg_1", !!(rq & 0x20)], ["force_chg_2", !!(rq & 0x10)], ["full_chg_req", !!(rq & 0x08)],
    ]) } : "—", CAN, !can),
    row("Protection", { html: bitFlags(cd.protection, PROT_BITS) }, CAN, !can),
    row("Alarm", { html: bitFlags(cd.alarm, ALARM_BITS) }, CAN, !can),
    row("System alarm (konzole)", has(j.bal) ? `0x${Number(j.bal).toString(16)}` : "—", CON, !bat),
    row("Konzole pozastavena (telnet)", onOff(j.slp), CON),
  ]);

  const ph = !!j.phv;
  fill("phone", [
    row("Baterie", ph ? `${j.phbp} % · ${j.phbs || ""}` : "—", PH, !ph),
    row("Proud baterie", ph ? `${Number(j.phbc) > 0 ? "+" : ""}${num(j.phbc, 0, " mA")}` : "—", PH, !ph),
    row("Stáří údaje o baterii", ph ? formatStaleSecs(j.phbss) : "—", PH, !ph || Number(j.phbss) > 120),
    row("Mobilní data rx / tx", ph ? `${formatBytes(j.phrx)} / ${formatBytes(j.phtx)}` : "—", PH, !ph),
    row("Stáří údaje o síti", ph ? formatStaleSecs(j.phns) : "—", PH, !ph || Number(j.phns) > 120),
    row("Nabíječka", onOff(j.co), ESP),
  ]);

  fill("system", [
    row("Uptime ESP", has(j.up) ? formatDuration(j.up) : "—", ESP),
    row("Reset reason", j.rrs ? `${j.rrs} (${j.rr})` : (has(j.rr) ? String(j.rr) : "—"), ESP),
    row("Stáří posledního QPIGS", has(j.ts) && has(j.up) ? formatStaleSecs((Number(j.up) - Number(j.ts)) / 1000) : "—", INV, !inv),
    row("Měnič / konzole / CAN platné", `${inv ? "✓" : "✗"} / ${bat ? "✓" : "✗"} / ${can ? "✓" : "✗"}`, ESP),
  ]);

  if (!resetReasonLogged && (has(j.rr) || has(j.rrs))) {
    logln(`ESP reset reason: ${j.rrs || "?"}${has(j.rr) ? ` (${j.rr})` : ""}`);
    resetReasonLogged = true;
  }
}

async function fetchJson(url, timeoutMs) {
  const ctrl = new AbortController();
  const to = setTimeout(() => ctrl.abort(), timeoutMs);
  try {
    const resp = await fetch(url, { cache: "no-store", signal: ctrl.signal });
    if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
    return await resp.json();
  } finally {
    clearTimeout(to);
  }
}

async function poll() {
  if (statusInFlight) return;
  statusInFlight = true;
  try {
    const [st, can] = await Promise.all([
      fetchJson("/status?full=1", STATUS_TIMEOUT_MS),
      fetchJson("/can", STATUS_TIMEOUT_MS).catch((e) => { logln("CAN fetch: " + e); return lastCan; }),
    ]);
    lastStatus = st;
    if (can) {
      const now = Date.now();
      if (can.link && prevCanRx !== null && now > prevCanRxMs) {
        canRxRate = (can.link.rx - prevCanRx) * 1000 / (now - prevCanRxMs);
      }
      if (can.link) { prevCanRx = can.link.rx; prevCanRxMs = now; }
      lastCan = can;
    }
    setConn(true, "");
    render();
  } catch (e) {
    const msg = e && e.name === "AbortError" ? `Timeout ${STATUS_TIMEOUT_MS / 1000}s` : String(e.message || e);
    setConn(false, msg);
    logln("Fetch error: " + msg);
  } finally {
    statusInFlight = false;
  }
}

async function pollConfig() {
  try {
    lastCfg = await fetchJson("/inv_config", 15000);
    render();
  } catch (e) {
    logln("inv_config fetch: " + e);
  }
}

async function send(obj) {
  const s = JSON.stringify(obj);
  logln("SEND: " + s);
  try {
    const resp = await fetch("/cmd", { method: "POST", headers: { "Content-Type": "application/json" }, body: s });
    const txt = await resp.text();
    logln("RX: " + txt);
  } catch (e) {
    logln("ERR send: " + e);
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

poll();
setInterval(poll, 2000);
pollConfig();
setInterval(pollConfig, CONFIG_INTERVAL_MS);
