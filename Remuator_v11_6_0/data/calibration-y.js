import { $, request, escape, showError, clearError, status } from "./app.js";
const phaseRu={idle:"Ожидание",seek85:"Поиск 85°",prepare:"Разворот к 23°",seek23:"Измерение при ходе к 23°",review:"Проверка таблицы",error:"Ошибка"};
let last = null, initialized = false, loading = false;
const fmt = (v, digits=2) => v === null || v === undefined || Number.isNaN(Number(v)) ? "—" : Number(v).toFixed(digits);
const row = (cells) => `<tr>${cells.map(x=>`<td>${escape(x)}</td>`).join("")}</tr>`;
async function refresh() {
  if (loading) return;
  loading = true;
  try {
    const d = await request("/api/calibration/y"); last=d;
    if (!initialized) {
      for (const key of ["yAxis","xAxis","xTolerance"]) $("axesForm").elements[key].value=String(d[key]);
      initialized=true;
    }
    $("sensorState").textContent=d.present ? "ADXL345 доступен (I²C 0x53)" : "ADXL345 не обнаружен или сигнал вне диапазона";
    ["ax","ay","az"].forEach((id,i)=>$(id).textContent=fmt(d.accel[i],4));
    $("tilt").textContent=fmt(d.angleY); $("roll").textContent=fmt(d.angleX);
    $("xState").textContent=d.xStored ? `Эталон X: ${fmt(d.xReference)}°; отклонение ${fmt(d.xDeviation)}°; допуск ${fmt(d.xTolerance)}° — ${d.xOk?"норма":"выровняйте ножками или запомните X заново"}` : "Эталон X ещё не сохранён";
    $("phase").textContent=`Этап: ${phaseRu[d.phase] || d.phase} · ${d.message || "—"}`;
    $("summary").textContent=`Собрано точек: ${d.rawCount}; полная длина: ${d.totalSteps} STEP; измеренный верх: ${fmt(d.measuredMax)}°, низ: ${fmt(d.measuredMin)}°. ${d.runReady?"Ремюатор готов к программе.":d.runReason}`;
    const busy=["seek85","prepare","seek23"].includes(d.phase);
    $("start").disabled=busy || !d.present || !d.xOk;
    $("rememberX").disabled=busy || !d.present;
    $("axesField").disabled=busy;
    $("save").disabled=d.phase!=="review" || d.rawCount<25;
    if(d.phase==="review" && d.measuredMin!==null && !$("minAngle").dataset.edited)
      $("minAngle").value=Math.min(40,Math.max(0,Math.round(d.measuredMin)));
    $("rawRows").innerHTML=d.raw?.length ? d.raw.map(p=>row([fmt(p.angle),p.stepsFrom85,p.stepsFrom23])).join("") : row(["—","—","—"]);
    $("savedRows").innerHTML=d.saved?.length ? d.saved.map(p=>row([p.angle,p.steps])).join("") : row(["—","—"]);
    clearError();
  } catch(e) {if(!String(e.message||e).includes("Двигатели движутся"))showError(e);} finally {loading=false;}
}
async function action(fields) {
  try { clearError(); await request("/api/calibration/y",fields);await refresh(); }
  catch(e) {showError(e);}
}
$("axesForm").addEventListener("submit", async(e)=>{
  e.preventDefault(); const fd=new FormData(e.target);
  if(fd.get("xAxis")===fd.get("yAxis")){showError(new Error("Физические оси X и Y должны отличаться"));return;}
  await action({action:"axes",...Object.fromEntries(fd.entries())});
  initialized=false; status("Выбор осей и допуск сохранены");
});
$("rememberX").addEventListener("click",()=>action({action:"rememberX"}));
$("start").addEventListener("click",()=>action({action:"start"}));
$("cancel").addEventListener("click",()=>action({action:"cancel"}));
$("minAngle").addEventListener("input",()=>$("minAngle").dataset.edited="1");
$("saveForm").addEventListener("submit",async(e)=>{
  e.preventDefault();await action({action:"save",minAngle:$("minAngle").value});
});
refresh();setInterval(refresh,1000);
