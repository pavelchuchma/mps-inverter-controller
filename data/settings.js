// Settings page: reads inverter configuration on demand (QPIRI + QFLAG) and
// compares it against the target setup from the manual
// (doc/.../nastaveni_MPS-5500H_PowerSafe_48V.md). Six fields are editable and can
// be written back to the inverter via POST /inv_set. All parsing/mapping lives
// here so it can be tweaked by re-uploading web files without reflashing firmware.
//
// QPIRI token indices follow doc/ps_rs232_protocol_FULL_ai_ready.txt section 4.6.
// The exact order/count is model-dependent — confirm against the raw QPIRI dump
// shown in the "Raw payloads" box below before trusting the comparison.

// QPIRI field order (token index after the leading '(' is stripped by firmware):
//  0 grid rating V        1 grid rating A         2 AC out rating V
//  3 AC out rating Hz     4 AC out rating A        5 AC out rating VA
//  6 AC out rating W      7 batt rating V          8 batt re-charge V
//  9 batt under V        10 batt bulk V           11 batt float V
// 12 batt type           13 max AC charge A       14 current max charge A
// 15 input volt range    16 output src priority   17 charger src priority
// 18 parallel max num    19 machine type          20 topology
// 21 output mode         22 batt re-discharge V   23 PV OK cond     24 PV power balance

const BATT_TYPE = { "0": "AGM", "1": "Flooded", "2": "User (USE)" };
const OUT_PRIORITY = { "0": "Utility first", "1": "Solar first", "2": "SBU" };
const CHG_PRIORITY = { "0": "Utility first", "1": "Solar first (CSO)", "2": "Solar+Utility", "3": "Solar only" };
const IN_RANGE = { "0": "Appliance (APL)", "1": "UPS" };
const PV_BALANCE = { "0": "dle proudu", "1": "dle zátěž+nabíjení" };

// Row definitions for the settings actually present in the manual (placeholder
// "(neuvedeno v manuálu)" rows are removed). Each row mirrors the manual columns:
//   id, name, expected (manual value), note (manual note).
// Read mapping:
//   qpiri: token index + (num | enum) comparison
//   flag:  QFLAG letter + expected enabled/disabled
// Editable rows additionally carry `edit`:
//   { prefix, kind:'volt'|'curr', min, max, step }  -> writes "<prefix><value>".
const ROWS = [
  { id: "00", name: "Návrat z režimu nastavení", expected: "ESC", note: "Jen odchod z menu." },
  { id: "01", name: "Priorita zdroje (pro pokrytí zátěže)", expected: "SBU",
    note: "Ostrovní režim: Solar → Battery (síť nemáš).",
    qpiri: { idx: 16, type: "enum", map: OUT_PRIORITY, code: "2" } },
  { id: "02", name: "Maximální nabíjecí proud (Solar + síť)", expected: "20 A",
    note: "Celkový nabíjecí proud. Editovatelné — hodnoty z QMCHGCR.",
    qpiri: { idx: 14, type: "num", value: 20, unit: "A" },
    edit: { prefix: "MCHGC", kind: "curr" } },
  { id: "03", name: "Rozsah AC napětí vstupu", expected: "Přístroje (APL)",
    note: "AC nemáš, ale bezpečné nastavení.",
    qpiri: { idx: 15, type: "enum", map: IN_RANGE, code: "0" } },
  { id: "04", name: "Režim úspory energie", expected: "Vypnuto (SdS)",
    note: "Ať se měnič nevypíná při malé zátěži.",
    flag: { letter: "J", enabled: false } },
  { id: "05", name: "Typ baterie", expected: "USE (uživatelský)",
    note: "Nutné pro vlastní Bulk/Float/LVD.",
    qpiri: { idx: 12, type: "enum", map: BATT_TYPE, code: "2" } },
  { id: "06", name: "Auto restart při přetížení", expected: "Zapnuto",
    note: "Praktické pro ostrov.",
    flag: { letter: "U", enabled: true } },
  { id: "07", name: "Auto restart při přehřátí", expected: "Zapnuto",
    note: "Po ochlazení znovu najede.",
    flag: { letter: "V", enabled: true } },
  { id: "08", name: "Výstupní napětí", expected: "230 V", note: "Standard CZ/EU.",
    qpiri: { idx: 2, type: "num", value: 230, unit: "V" } },
  { id: "09", name: "Výstupní frekvence", expected: "50 Hz", note: "Standard CZ/EU.",
    qpiri: { idx: 3, type: "num", value: 50, unit: "Hz" } },
  { id: "11", name: "Max nabíjecí proud ze sítě", expected: "20 A",
    note: "Nemáš AC → nastav minimum.",
    qpiri: { idx: 13, type: "num", value: 20, unit: "A" } },
  { id: "12", name: "Napětí pro návrat ke spotřebě ze sítě (SBU)", expected: "46 V",
    note: "Bez sítě se nepoužije, ale nastav konzervativně.",
    qpiri: { idx: 8, type: "num", value: 46, unit: "V" },
    edit: { prefix: "PBCV", kind: "volt", min: 44, max: 51, step: 0.1 } },
  { id: "13", name: "Napětí pro návrat ke spotřebě z baterie (SBU)", expected: "50 V",
    note: "Reconnect hranice, aby to necukalo.",
    qpiri: { idx: 22, type: "num", value: 50, unit: "V" },
    edit: { prefix: "PBDV", kind: "volt", min: 48, max: 58, step: 0.1 } },
  { id: "16", name: "Priorita zdroje nabíječe", expected: "Solar first (CSO)",
    note: "Nabíjení pouze z FV.",
    qpiri: { idx: 17, type: "enum", map: CHG_PRIORITY, code: "1" } },
  { id: "18", name: "Nastavení alarmu", expected: "Zapnutý",
    note: "Doporučeno pro diagnostiku. Pozn.: mapováno na QFLAG 'A' (buzzer) — semantiku ověřit dumpem.",
    flag: { letter: "A", enabled: true } },
  { id: "19", name: "Automatický návrat na výchozí stránku", expected: "Návrat na výchozí (ESP)",
    note: "Jen chování LCD.",
    flag: { letter: "K", enabled: true } },
  { id: "20", name: "Podsvícení", expected: "Zapnuto", note: "Jen LCD.",
    flag: { letter: "X", enabled: true } },
  { id: "22", name: "Pípnutí při výpadku primárního zdroje", expected: "Zapnuto",
    note: "V ostrovním režimu to pomůže hlídat stav.",
    flag: { letter: "Y", enabled: true } },
  { id: "23", name: "Bypass při přetížení", expected: "Bypass zakázán",
    note: "Bypass = síť, ale žádnou nemáš.",
    flag: { letter: "B", enabled: false } },
  { id: "25", name: "Log chyb", expected: "Povolen", note: "Pomůže při diagnostice.",
    flag: { letter: "Z", enabled: true } },
  { id: "26", name: "Nabíjecí napětí v „bulk“ fázi", expected: "55.2 V",
    note: "Doporučené Bulk/Absorb pro 48V VRLA (4×12V).",
    qpiri: { idx: 10, type: "num", value: 55.2, unit: "V" },
    edit: { prefix: "PCVV", kind: "volt", min: 48.0, max: 58.4, step: 0.1 } },
  { id: "27", name: "Udržovací (Float) napětí baterie", expected: "54.5 V",
    note: "Odpovídá štítku PowerSafe (cca 54.5–55.0V dle teploty).",
    qpiri: { idx: 11, type: "num", value: 54.5, unit: "V" },
    edit: { prefix: "PBFT", kind: "volt", min: 48.0, max: 58.4, step: 0.1 } },
  { id: "29", name: "Nízké stejnosměrné přerušení (LVD)", expected: "46.5 V",
    note: "Šetrné minimum pro životnost baterií. SoC guard (níže) tento parametr přepíná 46.0 / 48.0 V.",
    qpiri: { idx: 9, type: "num", value: 46.5, unit: "V" },
    edit: { prefix: "PSDV", kind: "volt", min: 40.0, max: 48.0, step: 0.1 } },
  { id: "31", name: "Rovnováha solárního výkonu", expected: "Povoleno",
    note: "Omezí výkon dle: zátěž + nabíjení.",
    qpiri: { idx: 24, type: "enum", map: PV_BALANCE, code: "1" } },
];

// Parse a QFLAG payload like "EakxyzDbjuv" into { letter: bool } enabled map.
function parseFlags(qflag) {
  const map = {};
  let state = null;
  for (const ch of qflag) {
    if (ch === "E") state = true;
    else if (ch === "D") state = false;
    else if (state !== null && /[A-Za-z]/.test(ch)) map[ch.toUpperCase()] = state;
  }
  return map;
}

const FLOAT_EPS = 0.1;

// Evaluate one row against parsed QPIRI tokens and QFLAG map.
// Returns { read: <display string>, status: "ok"|"mismatch"|"na", raw: <token|null> }.
function evalRow(row, toks, flags) {
  if (row.qpiri) {
    const r = row.qpiri;
    if (!toks || toks.length <= r.idx || toks[r.idx] === undefined) {
      return { read: "—", status: "na", raw: null };
    }
    const raw = toks[r.idx];
    if (r.type === "num") {
      const v = parseFloat(raw);
      if (isNaN(v)) return { read: raw, status: "na", raw };
      const ok = Math.abs(v - r.value) <= FLOAT_EPS;
      return { read: `${raw}${r.unit ? " " + r.unit : ""}`, status: ok ? "ok" : "mismatch", raw };
    }
    if (r.type === "enum") {
      const label = r.map[raw] !== undefined ? r.map[raw] : "?";
      const ok = raw === r.code;
      return { read: `${label} (${raw})`, status: ok ? "ok" : "mismatch", raw };
    }
  }
  if (row.flag) {
    if (!flags || flags[row.flag.letter] === undefined) {
      return { read: "—", status: "na", raw: null };
    }
    const on = flags[row.flag.letter];
    const ok = on === row.flag.enabled;
    return { read: on ? "Zapnuto (E)" : "Vypnuto (D)", status: ok ? "ok" : "mismatch", raw: null };
  }
  return { read: "—", status: "na", raw: null };
}

function esc(s) {
  return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}

// Empty text hides the pill: a successful read has nothing to say, the table
// and the state section below speak for themselves.
function setStatus(text, cls) {
  const status = document.getElementById("status");
  status.textContent = text;
  status.className = "pill" + (cls ? " " + cls : "");
  status.style.display = text ? "" : "none";
}

// Selectable max-charging-current values from QMCHGCR (raw tokens, e.g. "010").
let MCHGCR = [];

// Build the editable input cell for a row, prefilled with the read raw value.
// Voltage fields use a plain text input (type=text, not number) so the value is
// never locale-formatted — we always keep/send a decimal dot.
function editCell(row, raw) {
  if (!row.edit) return "";
  const e = row.edit;
  if (raw === null || raw === undefined || raw === "") return "—";
  if (e.kind === "volt") {
    const v = parseFloat(raw);
    const val = isNaN(v) ? "" : v.toFixed(1);
    return `<input class="edit" type="text" inputmode="decimal" size="6"
      data-id="${esc(row.id)}" data-name="${esc(row.name)}" data-prefix="${e.prefix}"
      data-kind="volt" data-min="${e.min}" data-max="${e.max}"
      data-orig="${esc(val)}" value="${esc(val)}"> V`;
  }
  // current: select limited to the QMCHGCR-reported values (only valid choices).
  let toks = MCHGCR.slice();
  if (raw && !toks.includes(raw)) toks = [raw, ...toks];
  const opts = toks.map(tok => {
    const sel = tok === raw ? " selected" : "";
    return `<option value="${esc(tok)}"${sel}>${parseInt(tok, 10)} A</option>`;
  }).join("");
  return `<select class="edit"
    data-id="${esc(row.id)}" data-name="${esc(row.name)}" data-prefix="${e.prefix}"
    data-kind="curr" data-orig="${esc(raw)}">${opts}</select>`;
}

// QPIWS bit map (doc/ps_rs232_protocol_FULL_ai_ready.txt, 4.11). kind: "fault",
// "warn", or "a1" = fault when a1 (Inverter fault) is set, otherwise a warning.
const QPIWS_BITS = {
  1:  ["Inverter fault", "fault"],
  2:  ["Bus over", "fault"],
  3:  ["Bus under", "fault"],
  4:  ["Bus soft fail", "fault"],
  5:  ["LINE_FAIL (chybí síť)", "warn"],
  6:  ["OPV short (zkrat na výstupu)", "warn"],
  7:  ["Inverter voltage too low", "fault"],
  8:  ["Inverter voltage too high", "fault"],
  9:  ["Over temperature", "a1"],
  10: ["Fan locked", "a1"],
  11: ["Battery voltage high", "a1"],
  12: ["Battery low alarm", "warn"],
  14: ["Battery under shutdown", "warn"],
  16: ["Overload", "a1"],
  17: ["EEPROM fault", "warn"],
  18: ["Inverter over current", "fault"],
  19: ["Inverter soft fail", "fault"],
  20: ["Self test fail", "fault"],
  21: ["OP DC voltage over", "fault"],
  22: ["Battery open", "fault"],
  23: ["Current sensor fail", "fault"],
  24: ["Battery short", "fault"],
  25: ["Power limit", "warn"],
  26: ["PV voltage high", "warn"],
  27: ["MPPT overload fault", "warn"],
  28: ["MPPT overload", "warn"],
};

// QMOD work-mode letters.
const QMOD_NAMES = { P: "Power on", S: "Standby", L: "Line", B: "Battery", F: "Fault", H: "Power saving" };

function setPill(id, text, cls) {
  const el = document.getElementById(id);
  el.textContent = text;
  el.className = "pill" + (cls ? " " + cls : "");
}

// Render the inverter's own view of its state: work mode, whether it has the AC
// output switched on (QPIGS b4 and QPGS0 b1), the QPGS0 fault code and the set
// QPIWS bits. The ESP only relays the raw payloads, the decoding lives here.
function renderInverterState(data) {
  const mode = (data.qmod || "").trim();
  setPill("inv-mode", "Mód: " + (mode ? `${mode} (${QMOD_NAMES[mode] || "?"})` : "?"),
          mode === "F" ? "err" : (mode ? "ok" : ""));

  const qpigs = (data.qpigs || "").trim().split(/\s+/);
  const qpgs = (data.qpgs0 || "").trim().split(/\s+/);
  // QPIGS token 16 = device status bits b7..b0 as an 8-char string; b4 = load on.
  const dsb = qpigs.length > 16 ? qpigs[16] : "";
  const loadQpigs = dsb.length === 8 ? dsb[3] === "1" : null;
  // QPGS0 token 19 = inverter status bits b7..b0; b1 = load on. Token 3 = fault code.
  const isb = qpgs.length > 19 ? qpgs[19] : "";
  const loadQpgs = isb.length === 8 ? isb[6] === "1" : null;
  const fault = qpgs.length > 3 ? qpgs[3] : "";

  if (loadQpigs === null && loadQpgs === null) {
    setPill("inv-load", "Výstup: ?", "");
  } else {
    const on = loadQpigs !== false && loadQpgs !== false;
    const agree = loadQpigs === null || loadQpgs === null || loadQpigs === loadQpgs;
    setPill("inv-load", `Výstup: ${on ? "ZAPNUT" : "VYPNUT"}` + (agree ? "" : " (QPIGS/QPGS0 nesouhlasí)"),
            on && agree ? "ok" : "err");
  }
  setPill("inv-fault", "Kód poruchy: " + (fault || "?"),
          fault === "" ? "" : (fault === "00" ? "ok" : "err"));

  const bits = (data.qpiws || "").trim();
  const a1 = bits.length > 1 && bits[1] === "1";
  const rows = [];
  for (let i = 0; i < bits.length; i++) {
    if (bits[i] !== "1") continue;
    const def = QPIWS_BITS[i];
    const name = def ? def[0] : "(nedokumentovaný bit)";
    let kind = def ? def[1] : "warn";
    if (kind === "a1") kind = a1 ? "fault" : "warn";
    const normal = i === 5;  // off-grid: no grid is the expected state
    const cls = normal ? "match" : (kind === "fault" ? "mismatch" : "");
    const label = normal ? "očekávané (off-grid)" : (kind === "fault" ? "PORUCHA" : "varování");
    rows.push(`<tr class="${cls}"><td class="id">a${i}</td><td>${esc(name)}</td><td>${label}</td></tr>`);
  }
  if (!bits) rows.push(`<tr><td class="id">—</td><td>QPIWS neodpovědělo</td><td></td></tr>`);
  else if (!rows.length) rows.push(`<tr class="match"><td class="id">—</td><td>žádné varování</td><td>✓</td></tr>`);
  document.querySelector("#warn-tbl tbody").innerHTML = rows.join("");
}

function render(data) {
  renderInverterState(data);
  const toks = (data.qpiri || "").trim().split(/\s+/).filter(s => s.length);
  const flags = parseFlags(data.qflag || "");
  MCHGCR = (data.qmchgcr || "").trim().split(/\s+/).filter(s => s.length);

  if (!data.ok) {
    setStatus("Čtení selhalo nebo neúplné — měnič neodpověděl na QPIRI/QFLAG. Zkus Refresh.", "err");
  } else {
    setStatus("", "");
  }

  const rows = ROWS.map(row => {
    const r = evalRow(row, toks, flags);
    const cls = r.status === "mismatch" ? "mismatch" : (r.status === "ok" ? "match" : "");
    const badge = r.status === "mismatch" ? " ⚠" : (r.status === "ok" ? " ✓" : "");
    return `<tr class="${cls}">
      <td class="id">${esc(row.id)}</td>
      <td>${esc(row.name)}</td>
      <td>${esc(row.expected)}</td>
      <td>${esc(r.read)}${badge}</td>
      <td>${editCell(row, r.raw)}</td>
      <td class="note">${esc(row.note)}</td>
    </tr>`;
  }).join("");

  document.querySelector("#tbl tbody").innerHTML = rows;

  document.getElementById("raw").textContent =
    `QPIRI:   ${data.qpiri || "(prázdné)"}\n` +
    `QFLAG:   ${data.qflag || "(prázdné)"}\n` +
    `QMOD:    ${data.qmod || "(prázdné)"}\n` +
    `QMCHGCR: ${data.qmchgcr || "(prázdné)"}\n` +
    `QPIGS:   ${data.qpigs || "(prázdné)"}\n` +
    `QPIWS:   ${data.qpiws || "(prázdné)"}\n` +
    `QPGS0:   ${data.qpgs0 || "(prázdné)"}`;
}

// Collect changed editable fields and build write commands. Returns
// { changes:[{id,name,orig,now,cmd}], errors:[string] }.
function collectChanges() {
  const changes = [];
  const errors = [];
  document.querySelectorAll(".edit").forEach(inp => {
    const orig = (inp.dataset.orig || "").trim();
    const id = inp.dataset.id, name = inp.dataset.name, prefix = inp.dataset.prefix;
    if (inp.dataset.kind === "volt") {
      // Normalize a comma to a dot — we always send decimals with a dot.
      const raw = (inp.value || "").trim().replace(",", ".");
      if (raw === "" || raw === orig) return;
      const min = parseFloat(inp.dataset.min), max = parseFloat(inp.dataset.max);
      if (!/^\d+(\.\d+)?$/.test(raw)) { errors.push(`${id} ${name}: '${inp.value}' není platné číslo (použij tečku)`); return; }
      const v = parseFloat(raw);
      if (v < min || v > max) { errors.push(`${id} ${name}: ${v} mimo rozsah ${min}–${max} V`); return; }
      const now = v.toFixed(1);
      if (now === orig) return;
      changes.push({ id, name, orig: orig + " V", now: now + " V", cmd: prefix + now });
    } else { // curr (select)
      const tok = (inp.value || "").trim();
      if (tok === "" || tok === orig) return;
      changes.push({ id, name, orig: parseInt(orig, 10) + " A", now: parseInt(tok, 10) + " A", cmd: prefix + tok });
    }
  });
  return { changes, errors };
}

async function doUpdate() {
  const { changes, errors } = collectChanges();
  if (errors.length) {
    alert("Neplatné hodnoty — nic se neodeslalo:\n\n" + errors.join("\n"));
    return;
  }
  if (!changes.length) {
    setStatus("Žádné změny k uložení.", "");
    return;
  }
  const summary = changes.map(c => `${c.id} ${c.name}:  ${c.orig} → ${c.now}   [${c.cmd}]`).join("\n");
  if (!confirm("Opravdu zapsat do měniče následující změny?\n\n" + summary +
               "\n\nPozor: mění se nabíjecí parametry baterie.")) {
    return;
  }
  setStatus("Zapisuji do měniče…", "");
  try {
    const resp = await fetch("/inv_set", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ cmds: changes.map(c => c.cmd) }),
    });
    if (!resp.ok) { setStatus("Zápis: HTTP chyba " + resp.status, "err"); return; }
    const j = await resp.json();
    const lines = (j.results || []).map(r => `${r.cmd}: ${r.ok ? "ACK ✓" : (r.resp + " ✗")}`);
    const allOk = (j.results || []).every(r => r.ok);
    alert("Výsledek zápisu:\n\n" + lines.join("\n"));
    setStatus(allOk ? "Zápis OK, načítám znovu…" : "Část zápisů selhala — viz dialog.", allOk ? "ok" : "err");
  } catch (e) {
    setStatus("Chyba zápisu: " + e, "err");
    return;
  }
  await load(); // read-back confirms the new values
}

async function load() {
  setStatus("Načítám z měniče… (může trvat několik sekund)", "");
  try {
    const resp = await fetch("/inv_config", { cache: "no-store" });
    if (!resp.ok) { setStatus("HTTP chyba: " + resp.status, "err"); return; }
    render(await resp.json());
  } catch (e) {
    setStatus("Chyba načítání: " + e, "err");
  }
}

// ---- SoC guard (doc/todo/007-soc-guard-cutoff.md) ----
// State comes from /status: sg (enabled), sga (armed), sgl (intended LVD V),
// sgok (last PSDV write ACKed; absent = never written), sgarm / sgdis
// (thresholds, so the text matches the firmware constants).
let sgEnabled = false;

function renderGuard(j) {
  sgEnabled = !!j.sg;
  const status = document.getElementById("sg-status");
  const btn = document.getElementById("sg-toggle");
  document.getElementById("sg-arm").textContent = j.sgarm !== undefined ? j.sgarm : "?";
  document.getElementById("sg-dis").textContent = j.sgdis !== undefined ? j.sgdis : "?";
  let text, cls;
  if (!sgEnabled) {
    text = "Vypnuto"; cls = "";
  } else if (j.sga) {
    text = `Zapnuto — AKTIVNÍ, LVD držen na ${Number(j.sgl).toFixed(1)} V`; cls = "err";
  } else {
    text = `Zapnuto — klid, LVD ${Number(j.sgl).toFixed(1)} V`; cls = "ok";
  }
  if (j.sgok === false) text += " (poslední zápis PSDV selhal, opakuje se po dalším čtení QPIRI)";
  status.textContent = text;
  status.className = "pill" + (cls ? " " + cls : "");
  btn.textContent = sgEnabled ? "Vypnout guard" : "Zapnout guard";
  btn.disabled = false;
}

async function loadGuard() {
  try {
    const resp = await fetch("/status", { cache: "no-store" });
    if (!resp.ok) return;
    renderGuard(await resp.json());
  } catch (e) {
    document.getElementById("sg-status").textContent = "Chyba čtení: " + e;
  }
}

async function toggleGuard() {
  const on = !sgEnabled;
  const msg = on
    ? "Zapnout SoC guard? Při nízkém SoC z BMS ESP zvedne LVD měniče na 48.0 V a výstup se vypne."
    : "Vypnout SoC guard? Pokud je právě aktivní, LVD zůstane na 48.0 V — vrať ho ručně řádkem 29.";
  if (!confirm(msg)) return;
  const btn = document.getElementById("sg-toggle");
  btn.disabled = true;
  try {
    const resp = await fetch("/cmd", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ type: "cmd", name: "set_soc_guard", value: on ? 1 : 0 }),
    });
    if (!resp.ok) alert("HTTP chyba " + resp.status);
  } catch (e) {
    alert("Chyba: " + e);
  }
  await loadGuard();
}

document.getElementById("refresh").addEventListener("click", () => { load(); loadGuard(); });
document.getElementById("update").addEventListener("click", doUpdate);
document.getElementById("sg-toggle").addEventListener("click", toggleGuard);
window.addEventListener("DOMContentLoaded", () => { load(); loadGuard(); });
