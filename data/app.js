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

let ws;
let demoMode = false;
const limEl = $("lim");
let lastServerLimit = -1;
const dutyEl = $("duty");
let lastServerDuty = -1;

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

function send(obj){
  const s = JSON.stringify(obj);
  logln("SEND: " + s);
  if(ws && ws.readyState === 1) ws.send(s);
}

function connect(){
  const url = `ws://${location.hostname}:81/`;
  logln("Connecting: " + url);
  ws = new WebSocket(url);

  ws.onopen = () => {
    setConn(true, "WS connected");
    send({type:"hello", role:"ui"});
  };

  ws.onclose = () => {
    setConn(false, "WS disconnected");
    logln("WS closed, reconnecting…");
    setTimeout(connect, 1000);
  };

  ws.onmessage = (ev) => {
    try{
      const j = JSON.parse(ev.data);

      if(j.type === "status"){
        $("pv").textContent = Math.round(j.pv_w);
        $("soc").textContent = Number(j.batt_soc).toFixed(1);
        $("bv").textContent = Number(j.batt_v).toFixed(2);
        $("load").textContent = Math.round(j.load_w);
        $("grid").textContent = j.grid_ok ? "OK" : "FAIL";
        $("state").textContent = j.state || "—";
        $("ts").textContent = j.ts_ms;

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

      if(j.type === "ack"){
        logln("ACK: " + (j.msg || "ok"));
        return;
      }

      if(j.type === "err"){
        logln("ERR: " + (j.code || "err") + " / " + (j.msg || ""));
        return;
      }

      logln("MSG: " + ev.data);
    }catch(e){
      logln("RAW: " + ev.data);
    }
  };
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

connect();
