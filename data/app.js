const $ = (id) => document.getElementById(id);
function logln(s){
  const el = $("log");
  el.textContent = (el.textContent === "—" ? "" : el.textContent + "\n") + s;
  el.scrollTop = el.scrollHeight;
}
function setConn(ok, msg){
  const el = $("conn");
  el.textContent = msg;
  el.className = "pill " + (ok ? "ok" : "err");
}

// Format milliseconds (e.g. from millis()) to HH:MM:SS
function formatMsToHMS(ms){
  const totalSec = Math.floor(Number(ms || 0) / 1000);
  const h = Math.floor(totalSec / 3600);
  const m = Math.floor((totalSec % 3600) / 60);
  const s = totalSec % 60;
  const hh = String(h).padStart(2, '0');
  const mm = String(m).padStart(2, '0');
  const ss = String(s).padStart(2, '0');
  return `${hh}:${mm}:${ss}`;
}

let demoMode = false;
const limEl = $("lim");
let lastServerLimit = -1;
const dutyEl = $("duty");
let lastServerDuty = -1;
let resetReasonLogged = false;

function updateModified(){
  const modified = Number(limEl.value) !== lastServerLimit;
  limEl.classList.toggle('modified', modified);
}

function updateModifiedDuty(){
  const modified = Number(dutyEl.value) !== lastServerDuty;
  dutyEl.classList.toggle('modified', modified);
}

updateModified();
updateModifiedDuty();

async function send(obj){
  const s = JSON.stringify(obj);
  logln("SEND: " + s);
  try{
    const resp = await fetch('/cmd', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: s
    });
    const txt = await resp.text();
    logln("RX: " + txt);
    if (resp.ok) {
      try { const j = JSON.parse(txt); if (j.msg) logln("ACK: " + j.msg); }
      catch(e){}
    }
  }catch(e){
    logln("ERR send: " + e);
  }
}

async function fetchStatus(){
  try{
    const resp = await fetch('/status', { cache: 'no-store' });
    if (!resp.ok) {
      setConn(false, `HTTP ${resp.status}`);
      logln(`HTTP status ${resp.status}`);
      return;
    }
    const j = await resp.json();
    setConn(true, "HTTP OK");

    if(j.type === "status"){
      $("pv").textContent = Math.round(j.pv_w);
      $("soc").textContent = Number(j.batt_soc).toFixed(1);
      $("bv").textContent = Number(j.batt_v).toFixed(2);
      $("load").textContent = Math.round(j.load_w);
      $("grid").textContent = j.grid_ok ? "OK" : "FAIL";
      $("state").textContent = j.state || "—";
      $("ts").textContent = formatMsToHMS(j.ts_ms);

      if (!resetReasonLogged && (j.reset_reason !== undefined || j.reset_reason_str !== undefined)){
        const rr = (j.reset_reason_str || "").toString();
        const rrn = (j.reset_reason !== undefined) ? String(j.reset_reason) : "";
        const msg = rr || rrn ? `ESP reset reason: ${rr}${rr && rrn ? ` (${rrn})` : rrn ? rrn : ""}` : "ESP reset reason: (unknown)";
        logln(msg);
        resetReasonLogged = true;
      }

      demoMode = !!j.demo;
      $("demoVal").textContent = demoMode ? "true" : "false";

      const lim = Math.round(j.output_limit_w ?? -1);
      if (lastServerLimit !== lim) {
        lastServerLimit = lim;
        limEl.value = lim;
        $("limVal").textContent = lim;
      }
      const duty = j.output_duty_cycle !== undefined ? Math.round(j.output_duty_cycle * 100) : -1;
      if (lastServerDuty !== duty) {
        lastServerDuty = duty;
        dutyEl.value = duty;
        $("dutyVal").textContent = (duty >= 0 ? (duty + ' %') : "—");
      }
      updateModified();
      updateModifiedDuty();
      return;
    }
    logln("MSG: " + JSON.stringify(j));
  }catch(e){
    setConn(false, "Fetch error");
    logln("Fetch error: " + e);
  }
}

$("btnDemo").addEventListener("click", () => {
  demoMode = !demoMode;
  send({type:"cmd", name:"set_demo", value: demoMode});
});

limEl.addEventListener("input", () => {
  $("limVal").textContent = limEl.value;
  updateModified();
});

dutyEl.addEventListener("input", () => {
  $("dutyVal").textContent = dutyEl.value + ' %';
  updateModifiedDuty();
});

$("btnApply").addEventListener("click", () => {
  const v = Number($("lim").value);
  send({type:"cmd", name:"set_output_limit_w", value: v});
});

$("btnApplyDuty").addEventListener("click", () => {
  const pct = Number($("duty").value);
  const v = pct / 100.0;
  send({type:"cmd", name:"set_output_duty_cycle", value: v});
});

// Initial fetch and schedule polling
fetchStatus();
setInterval(fetchStatus, 1250);
