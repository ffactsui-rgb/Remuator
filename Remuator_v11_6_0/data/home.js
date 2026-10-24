import {
  $,
  request,
  showError,
  clearError,
  date,
  dateSeconds,
  zone,
  epoch,
  duration,
  escape,
  programCards,
  bindCatalog,
} from "./app.js";
let state = null,
  busy = false,
  loading = null,
  clockReceivedAt = 0,
  pollTimer = 0,
  recoveryClockSent = false;
const number = (value) =>
  new Intl.NumberFormat("ru-RU", { maximumFractionDigits: 2 }).format(value);
const signed = (value) => (value > 0 ? "+" : "") + number(value);
function updateClock() {
  if (!state) return;
  const current = state.timeValid
    ? state.epoch + Math.floor((performance.now() - clockReceivedAt) / 1000)
    : 0;
  $("clock").textContent = current
    ? dateSeconds(current) + " · " + zone
    : "Время не установлено";
}
function renderSync(data) {
  let text =
    data.clockSource === "ntp"
      ? "Синхронизировано по NTP"
      : data.clockSource === "browser"
        ? "Установлено из браузера"
        : data.clockSource === "rtc"
          ? "Восстановлено из DS3231 (резервная батарея)"
          : "Источник времени не подтверждён";
  if (data.lastSyncEpoch) text += " · " + dateSeconds(data.lastSyncEpoch);
  if (data.ntpPending) text += " · ожидается ответ NTP";
  else if (data.ntpNoResponse) text += " · NTP пока не ответил";
  else if (!data.wifiConnected) text += " · домашняя Wi-Fi не подключена";
  if (!data.timeValid)
    text =
      "Часы не установлены. При веб-пуске будет использовано время браузера." +
      (data.ntpPending ? " Ожидается NTP." : "");
  $("syncStatus").textContent = text;
}
function renderPose(data) {
  $("posePanel").hidden = !data.running;
  if (!data.running) return;
  const y = Number(data.poseY),
    r = Number(data.poseRotation),
    orientation = ((r % 360) + 360) % 360;
  if (!Number.isFinite(y) || !Number.isFinite(r)) return;
  $("tiltValue").textContent = number(y) + "°";
  $("rotationValue").textContent = number(orientation) + "°";
  $("bottleTilt").setAttribute("transform", `rotate(${-y} 65 170)`);
  const radians = (y * Math.PI) / 180,
    x = 65 + 46 * Math.cos(radians),
    endY = 170 - 46 * Math.sin(radians);
  $("tiltArc").setAttribute("d", `M111 170 A46 46 0 0 0 ${x} ${endY}`);
  $("bottleRotation").setAttribute(
    "transform",
    `rotate(${orientation} 130 100)`,
  );
  $("poseNote").textContent =
    "Положение по сохранённым счётчикам шагов. Суммарный поворот: " +
    signed(r) +
    "°. Ноль зафиксирован при запуске.";
}
function mark(done, axis, step) {
  return `<input class="completion-check" type="checkbox" disabled ${done ? "checked" : ""} aria-label="Шаг ${step}: ${axis} ${done ? "выполнен" : "не завершён"}" title="${axis}: ${done ? "выполнен" : "не завершён"}">`;
}
function render(data) {
  if (state !== data) clockReceivedAt = performance.now();
  state = data;
  $("programName").textContent = data.name || "Программа не выбрана";
  $("programSummary").hidden = Boolean(data.running || !data.selectedCount);
  $("selectedDuration").textContent = duration(data.selectedSeconds || 0);
  $("selectedCount").textContent = String(data.selectedCount || 0);
  $("selectedFinish").textContent = data.selectedCount
    ? dateSeconds((data.timeValid ? data.epoch : Math.floor(Date.now()/1000)) + (data.selectedSeconds || 0))
    : "—";
  $("startStep").max = String(data.selectedCount || 1);
  $("startAtPanel").hidden = Boolean(data.running);
  $("startAt").disabled = busy || data.homing || !data.calibrationReady || !data.selectedCount;
  // The independent editor is mounted once per selected CSV, not recreated on every poll.
  const editor = $("inlineEditor");
  editor.hidden = Boolean(data.running || !data.selectedPath);
  const frame = $("editorFrame");
  if (!data.running && data.selectedPath && frame.dataset.path !== data.selectedPath) {
    frame.dataset.path = data.selectedPath;
    frame.src = "/program/edit?embed=1&path=" + encodeURIComponent(data.selectedPath);
  }
  $("state").textContent = data.running
    ? data.paused
      ? "На паузе"
      : "Выполняется"
    : "Остановлена";
  updateClock();
  renderSync(data);
  $("calibrationNotice").hidden=Boolean(data.calibrationReady);
  $("calibrationNoticeText").textContent=data.calibrationReason || "Проверьте ADXL345 и эталон горизонтали X";
  $("executionMessage").textContent = [data.message, data.logError]
    .filter(Boolean)
    .join(" · ");
  $("executionNotice").hidden = !data.noticeVisible;
  $("noticeText").textContent = data.finishedEpoch
    ? "Ремюаж окончен " + date(data.finishedEpoch)
    : "Отключение ≈ " +
      dateSeconds(data.outageEpoch) +
      " (последняя запись). Включение: " +
      (data.powerOnEpoch
        ? dateSeconds(data.powerOnEpoch)
        : "ожидается установка часов");
  $("recoveryStatus").hidden = !data.recoveryRequired;
  $("recoveryStatus").textContent = data.motionUncertain
    ? "Ошибка записи состояния: движение остановлено."
    : !data.timeValid
      ? "Восстановление: ожидается текущее время NTP или браузера."
      : data.autoResume
        ? "Автоматическое продолжение через " +
          data.resumeInSeconds +
          " с. Пауза или Стоп отменяют отсчёт."
        : "Восстановление на паузе. Нажмите Продолжить.";
  $("rangeWarning").hidden = !data.warning;
  $("rangeWarning").textContent = data.warning || "";
  $("homeY").disabled =
    busy || data.running || data.homing || data.yEndstopFault;
  $("endstops").textContent =
    `Y 23°: ${data.lowerY ? "разомкнут" : "замкнут"} · Y 85°: ${data.upperY ? "разомкнут" : "замкнут"}`;
  $("stopManual").disabled = data.running;
  $("power").disabled = data.homing;
  $("start").hidden = data.running;
  $("start").disabled = busy || data.homing || !data.programs.length || !data.calibrationReady;
  $("pause").hidden = !data.running || (data.paused && !data.autoResume);
  $("resume").hidden =
    !data.running ||
    !data.paused ||
    data.motionUncertain ||
    data.autoResume ||
    !data.timeValid;
  $("stop").hidden = !data.running;
  for (const id of ["pause", "resume", "stop"]) $(id).disabled = busy;
  $("runTimeline").hidden = !data.running;
  if (data.running) {
    $("startedAt").textContent = dateSeconds(data.startedAt);
    const current = data.rows.find((row) => row.number === data.currentStep);
    $("completedStep").textContent = current
      ? `№ ${current.number} — начало ${dateSeconds(current.start)}`
      : "—";
    $("currentChecks").innerHTML = current
      ? `<label>${mark(current.naklonDone, "Наклон", current.number)} Наклон</label><label>${mark(current.rotationDone, "Поворот", current.number)} Поворот</label>`
      : "";
    $("currentPhase").textContent =
      current?.naklonDone && current?.rotationDone
        ? "Обе операции выполнены" +
          (data.lastCompletedEpoch
            ? " " + dateSeconds(data.lastCompletedEpoch)
            : "") +
          "; выдержка шага ещё продолжается."
        : current?.naklonDone
          ? "Наклон выполнен; поворот ещё не завершён."
          : "Наклон ещё не завершён.";
    const next = data.rows.find((row) => row.number === data.nextStep);
    $("upcomingStep").textContent = next
      ? `№ ${next.number} — ${dateSeconds(next.start)}`
      : "Последний шаг. Далее — завершение программы";
    $("upcomingAction").textContent = next
      ? `Поворот ${signed(next.rot)}° ${next.rot > 0 ? "по часовой стрелке" : next.rot < 0 ? "против часовой стрелки" : "(без поворота)"}; наклон ${signed(next.incY)}° → Y ${number(next.y)}°.`
      : "Выдерживается пауза последнего шага";
    if (data.paused && next && !data.recoveryRequired)
      $("upcomingAction").textContent +=
        " Время указано для продолжения сейчас.";
    $("finishLabel").textContent =
      data.paused && !data.recoveryRequired
        ? "Окончание, если продолжить сейчас"
        : "Ожидаемое окончание";
    $("endsAt").textContent = dateSeconds(data.endsAt);
  }
  renderPose(data);
  // Preserve the table viewport while updating live checks and angles.
  const old = $("schedule").querySelector(".scroll"),
    top = old?.scrollTop || 0,
    left = old?.scrollLeft || 0;
  $("schedule").innerHTML = data.running
    ? `
    <p>Шаг ${data.currentStep} из ${data.rows.length} · отметки показывают завершение движения каждой оси</p>
    <div class="scroll">
      <table class="schedule-table">
        <thead><tr>
          <th>№</th><th>Начало по календарю</th><th>incAngle, °</th><th>Angle Y, °</th><th>Наклон ✓</th>
          <th>IncRot</th><th>ΔRot, °</th><th>Положение, °</th><th>Поворот ✓</th><th>Пауза ЧЧ:ММ:СС</th>
        </tr></thead>
        <tbody>${data.rows
          .map(
            (row) => `
          <tr class="${row.skipped ? "skipped" : row.number === data.currentStep ? "current" : row.number < data.currentStep ? "done" : ""}">
            <td>${row.number}${row.skipped ? " · пропущен" : ""}</td><td>${row.skipped ? "—" : dateSeconds(row.start)}</td><td>${signed(row.incY)}</td><td>${number(row.y)}</td>
            <td>${mark(row.naklonDone, "Наклон", row.number)}</td><td>${signed(row.incRot)}</td><td>${signed(row.rot)}</td>
            <td>${number(row.orientation)}</td><td>${mark(row.rotationDone, "Поворот", row.number)}</td><td>${escape(row.pause)}</td>
          </tr>`,
          )
          .join("")}
        </tbody>
      </table>
    </div>`
    : "";
  const view = $("schedule").querySelector(".scroll");
  if (view) {
    view.scrollTop = top;
    view.scrollLeft = left;
  }
  $("programList").innerHTML = data.running ? "" : programCards(data);
  for (const button of document.querySelectorAll("[data-manual]")) {
    const action = button.dataset.manual;
    const blocked =
      (action === "tilt_up" && data.upperY) ||
      (action === "tilt_down" && data.lowerY) ||
      (action.startsWith("tilt_") && data.yEndstopFault);
    button.disabled = data.running || data.homing || blocked;
  }
}
setInterval(updateClock, 250);
async function load() {
  if (loading) return loading;
  loading = request("/api/home")
    .then(render)
    .catch(e => {if(!String(e.message||e).includes("Двигатели движутся")) throw e;})
    .finally(() => {
      loading = null;
    });
  return loading;
}
async function reloadAfterAction() {
  if (loading) await loading.catch(() => {});
  await load();
  queuePoll();
}
// Before starting, flush any unsent editor input to the controller RAM draft.
async function flushEmbeddedEditor() {
  const f = $("editorFrame");
  if (!f?.contentWindow || !f.dataset.path) return;
  const token = "flush-" + Math.random().toString(36).slice(2);
  await new Promise((resolve, reject) => {
    const timeout = setTimeout(() => {cleanup(); reject(new Error("Редактор не подтвердил изменения. Проверьте черновик и повторите пуск."));}, 6500);
    function cleanup() {clearTimeout(timeout); window.removeEventListener("message", received);}
    function received(e) {
      if (e.origin !== location.origin || e.source !== f.contentWindow ||
          e.data?.type !== "remuage-flush-response" || e.data?.token !== token) return;
      cleanup(); e.data.ok ? resolve() : reject(new Error(e.data.message || "Ошибки в таблице программы"));
    }
    window.addEventListener("message", received);
    f.contentWindow.postMessage({type:"remuage-flush",token}, location.origin);
  });
}
async function action(name, firstStep = 1) {
  if (busy) return;
  busy = true;
  clearError();
  if (state) render(state);
  try {
    if (name === "stop" && !confirm("Остановить программу?")) return;
    if (name === "start") await flushEmbeddedEditor();
    const step = firstStep;
    await request("/api/program", { action: name, epoch: epoch(), ...(name === "start" ? {step} : {}) });
    await reloadAfterAction();
  } catch (error) {
    showError(error);
  } finally {
    busy = false;
    if (state) render(state);
  }
}
$("dismissNotice").onclick = () => action("dismiss");
$("startAt").onclick = async () => {
  const n = Number($("startStep").value);
  if (!Number.isInteger(n) || n < 1 || n > (state?.selectedCount || 0)) {
    showError("Введите существующий номер шага программы"); return;
  }
  if (n > 1 && !confirm(`Запустить с шага № ${n}? Предыдущие шаги НЕ будут выполнены. Убедитесь, что положение Y и Z безопасно.`)) return;
  await action("start", n);
};
for (const name of ["start", "pause", "resume", "stop"])
  $(name).onclick = () => action(name);
bindCatalog($("programList"), reloadAfterAction);
$("homeY").onclick = async () => {
  try {
    await request("/api/manual?action=home");
    await reloadAfterAction();
  } catch (error) {
    showError(error);
  }
};
$("stopManual").onclick = async () => {
  try {
    await request("/api/manual?action=stop");
    await reloadAfterAction();
  } catch (error) {
    showError(error);
  }
};
// Serialize press/release requests so a quick release cannot arrive before press.
let manualQueue = Promise.resolve(),
  held = null;
function manual(name) {
  manualQueue = manualQueue
    .catch(() => {})
    .then(() => request("/api/manual?action=" + name))
    .catch(showError);
}
for (const button of document.querySelectorAll("[data-manual]")) {
  button.addEventListener("pointerdown", (event) => {
    if (state?.running || held || event.button !== 0) return;
    event.preventDefault();
    held = button;
    button.setPointerCapture(event.pointerId);
    manual(button.dataset.manual);
  });
  for (const event of ["pointerup", "pointercancel", "lostpointercapture"])
    button.addEventListener(event, release);
  button.addEventListener("contextmenu", (event) => event.preventDefault());
}
function release() {
  if (!held) return;
  held = null;
  manual("stop");
}
window.addEventListener("blur", release);
document.addEventListener("visibilitychange", () => {
  if (document.hidden) release();
});
let powerStarted = 0;
$("power").addEventListener("pointerdown", (event) => {
  if (event.button !== 0) return;
  event.preventDefault();
  powerStarted = performance.now();
  $("power").setPointerCapture(event.pointerId);
});
$("power").addEventListener("pointerup", async () => {
  if (!powerStarted) return;
  const long = performance.now() - powerStarted >= 800;
  powerStarted = 0;
  if (long) {
    await action(state?.running ? "stop" : "start");
    return;
  }
  if (state?.running) {
    showError("Остановите программу перед обнулением координат");
    return;
  }
  if (
    !confirm(
      "Считать текущее положение механики исходным и обнулить координаты?",
    )
  )
    return;
  try {
    await request("/api/program", { action: "reset" });
    await reloadAfterAction();
  } catch (error) {
    showError(error);
  }
});
$("power").addEventListener("pointercancel", () => {
  powerStarted = 0;
});
function queuePoll() {
  clearTimeout(pollTimer);
  pollTimer = setTimeout(
    poll,
    Math.max(
      250,
      Math.min(state?.running ? 1000 : 2000, state?.refreshMs || 1500),
    ),
  );
}
async function poll() {
  try {
    await load();
    if (state?.recoveryRequired && !state.timeValid && !recoveryClockSent) {
      recoveryClockSent = true;
      await request("/api/time", { epoch: epoch() });
      await load();
    }
  } catch (error) {
    showError(error);
  } finally {
    queuePoll();
  }
}
poll();
