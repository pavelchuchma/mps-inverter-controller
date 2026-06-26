// Settings page: reads inverter configuration on demand (QPIRI + QFLAG) and
// compares it against the target setup from the manual
// (doc/.../nastaveni_MPS-5500H_PowerSafe_48V.md). All parsing/mapping lives here
// so it can be tweaked by re-uploading web files without reflashing firmware.
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

// Row definitions for menu items 00..31 (items 32+ = equalization are omitted on
// purpose — equalization is disabled). Each row mirrors the manual's columns:
//   id, name, expected (manual value), note (manual note).
// Optional readers:
//   qpiri: token index + (num | enum) comparison
//   flag:  QFLAG letter + expected enabled/disabled
// Rows without a reader (ESC, items not specified in the manual) just show "—".
const ROWS = [
  { id: "00", name: "Návrat z režimu nastavení", expected: "ESC", note: "Jen odchod z menu." },
  { id: "01", name: "Priorita zdroje (pro pokrytí zátěže)", expected: "SBU",
    note: "Ostrovní režim: Solar → Battery (síť nemáš).",
    qpiri: { idx: 16, type: "enum", map: OUT_PRIORITY, code: "2" } },
  { id: "02", name: "Maximální nabíjecí proud (Solar + síť)", expected: "20 A",
    note: "Celkový nabíjecí proud.",
    qpiri: { idx: 14, type: "num", value: 20, unit: "A" } },
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
  { id: "10", name: "(neuvedeno v manuálu)", expected: "—", note: "Položka nebyla v tabulce jasně popsaná." },
  { id: "11", name: "Max nabíjecí proud ze sítě", expected: "20 A",
    note: "Nemáš AC → nastav minimum.",
    qpiri: { idx: 13, type: "num", value: 20, unit: "A" } },
  { id: "12", name: "Napětí pro návrat ke spotřebě ze sítě (SBU)", expected: "46 V",
    note: "Bez sítě se nepoužije, ale nastav konzervativně.",
    qpiri: { idx: 8, type: "num", value: 46, unit: "V" } },
  { id: "13", name: "Napětí pro návrat ke spotřebě z baterie (SBU)", expected: "50 V",
    note: "Reconnect hranice, aby to necukalo.",
    qpiri: { idx: 22, type: "num", value: 50, unit: "V" } },
  { id: "14", name: "(neuvedeno v manuálu)", expected: "—", note: "Položka nebyla v tabulce jasně popsaná." },
  { id: "15", name: "(neuvedeno v manuálu)", expected: "—", note: "Položka nebyla v tabulce jasně popsaná." },
  { id: "16", name: "Priorita zdroje nabíječe", expected: "Solar first (CSO)",
    note: "Nabíjení pouze z FV.",
    qpiri: { idx: 17, type: "enum", map: CHG_PRIORITY, code: "1" } },
  { id: "17", name: "(neuvedeno v manuálu)", expected: "—", note: "Položka nebyla v tabulce jasně popsaná." },
  { id: "18", name: "Nastavení alarmu", expected: "Zapnutý",
    note: "Doporučeno pro diagnostiku. Pozn.: mapováno na QFLAG 'A' (buzzer) — semantiku ověřit dumpem.",
    flag: { letter: "A", enabled: true } },
  { id: "19", name: "Automatický návrat na výchozí stránku", expected: "Návrat na výchozí (ESP)",
    note: "Jen chování LCD.",
    flag: { letter: "K", enabled: true } },
  { id: "20", name: "Podsvícení", expected: "Zapnuto", note: "Jen LCD.",
    flag: { letter: "X", enabled: true } },
  { id: "21", name: "(neuvedeno v manuálu)", expected: "—", note: "Položka nebyla v tabulce jasně popsaná." },
  { id: "22", name: "Pípnutí při výpadku primárního zdroje", expected: "Zapnuto",
    note: "V ostrovním režimu to pomůže hlídat stav.",
    flag: { letter: "Y", enabled: true } },
  { id: "23", name: "Bypass při přetížení", expected: "Bypass zakázán",
    note: "Bypass = síť, ale žádnou nemáš.",
    flag: { letter: "B", enabled: false } },
  { id: "24", name: "(neuvedeno v manuálu)", expected: "—", note: "Položka nebyla v tabulce jasně popsaná." },
  { id: "25", name: "Log chyb", expected: "Povolen", note: "Pomůže při diagnostice.",
    flag: { letter: "Z", enabled: true } },
  { id: "26", name: "Nabíjecí napětí v „bulk“ fázi", expected: "55.2 V",
    note: "Doporučené Bulk/Absorb pro 48V VRLA (4×12V).",
    qpiri: { idx: 10, type: "num", value: 55.2, unit: "V" } },
  { id: "27", name: "Udržovací (Float) napětí baterie", expected: "54.5 V",
    note: "Odpovídá štítku PowerSafe (cca 54.5–55.0V dle teploty).",
    qpiri: { idx: 11, type: "num", value: 54.5, unit: "V" } },
  { id: "28", name: "(neuvedeno v manuálu)", expected: "—", note: "Položka nebyla v tabulce jasně popsaná." },
  { id: "29", name: "Nízké stejnosměrné přerušení (LVD)", expected: "46.5 V",
    note: "Šetrné minimum pro životnost baterií.",
    qpiri: { idx: 9, type: "num", value: 46.5, unit: "V" } },
  { id: "30", name: "(neuvedeno v manuálu)", expected: "—", note: "V manuálu zmíněno v kontextu ekvalizace, v tabulce neuvedeno." },
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
// Returns { read: <display string>, status: "ok" | "mismatch" | "na" }.
function evalRow(row, toks, flags) {
  if (row.qpiri) {
    const r = row.qpiri;
    if (!toks || toks.length <= r.idx || toks[r.idx] === undefined) {
      return { read: "—", status: "na" };
    }
    const raw = toks[r.idx];
    if (r.type === "num") {
      const v = parseFloat(raw);
      if (isNaN(v)) return { read: raw, status: "na" };
      const ok = Math.abs(v - r.value) <= FLOAT_EPS;
      return { read: `${raw}${r.unit ? " " + r.unit : ""}`, status: ok ? "ok" : "mismatch" };
    }
    if (r.type === "enum") {
      const label = r.map[raw] !== undefined ? r.map[raw] : "?";
      const ok = raw === r.code;
      return { read: `${label} (${raw})`, status: ok ? "ok" : "mismatch" };
    }
  }
  if (row.flag) {
    if (!flags || flags[row.flag.letter] === undefined) {
      return { read: "—", status: "na" };
    }
    const on = flags[row.flag.letter];
    const ok = on === row.flag.enabled;
    return { read: on ? "Zapnuto (E)" : "Vypnuto (D)", status: ok ? "ok" : "mismatch" };
  }
  return { read: "—", status: "na" };
}

function esc(s) {
  return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}

function render(data) {
  const toks = (data.qpiri || "").trim().split(/\s+/).filter(s => s.length);
  const flags = parseFlags(data.qflag || "");

  const status = document.getElementById("status");
  if (!data.ok) {
    status.textContent = "Čtení selhalo nebo neúplné — měnič neodpověděl na QPIRI/QFLAG. Zkus Refresh.";
    status.className = "pill err";
  } else {
    status.textContent = `Načteno z měniče (${toks.length} QPIRI tokenů). Mód: ${esc(data.qmod || "?")}`;
    status.className = "pill ok";
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
      <td class="note">${esc(row.note)}</td>
    </tr>`;
  }).join("");

  document.querySelector("#tbl tbody").innerHTML = rows;

  document.getElementById("raw").textContent =
    `QPIRI: ${data.qpiri || "(prázdné)"}\n` +
    `QFLAG: ${data.qflag || "(prázdné)"}\n` +
    `QMOD:  ${data.qmod || "(prázdné)"}`;
}

async function load() {
  const status = document.getElementById("status");
  status.textContent = "Načítám z měniče… (může trvat několik sekund)";
  status.className = "pill";
  try {
    const resp = await fetch("/inv_config", { cache: "no-store" });
    if (!resp.ok) {
      status.textContent = "HTTP chyba: " + resp.status;
      status.className = "pill err";
      return;
    }
    render(await resp.json());
  } catch (e) {
    status.textContent = "Chyba načítání: " + e;
    status.className = "pill err";
  }
}

document.getElementById("refresh").addEventListener("click", load);
window.addEventListener("DOMContentLoaded", load);
