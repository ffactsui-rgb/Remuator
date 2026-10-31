import { $, request, populate, bindForm, showError } from "./app.js";

const bus = $("bus");

function updateTopology() {
  for (const [axis, label] of [
    ["nak", "Y"],
    ["pov", "Z"],
  ]) {
    const model = bus.elements[axis + "Model"].value;
    const address = bus.elements[axis + "Addr"];
    address.disabled = model === "2208";
    address.closest("label").classList.toggle("inactive", address.disabled);
    if (address.disabled) address.value = "0";
    $(axis + "AddressHint").textContent =
      model === "2208"
        ? `${label}: TMC2208 · фиксированный адрес 0.`
        : `${label}: TMC2209 · MS1=${Number(address.value) & 1}, MS2=${(Number(address.value) >> 1) & 1}.`;
  }
  const conflict = bus.elements.nakAddr.value === bus.elements.povAddr.value;
  $("busConflict").textContent =
    "Адреса совпадают. На общей UART нужны разные адреса; два TMC2208 подключить независимо нельзя.";
  $("busConflict").hidden = !conflict;
  bus.querySelector("button").disabled = conflict;
}

async function load() {
  const data = await request("/api/settings/uart");

  if (!data.nak?.model || !data.pov?.model)
    throw new Error(
      "Для выбора модели драйвера прошивка и веб-файлы должны быть одной версии.",
    );
  populate(bus, {
    baud: data.baud,
    nakModel: data.nak.model,
    nakAddr: data.nak.address,
    povModel: data.pov.model,
    povAddr: data.pov.address,
  });
  for (const [axis, label] of [
    ["nak", "Наклон Y"],
    ["pov", "Поворот Z"],
  ]) {
    const config = data[axis];
    populate($(axis), {...config, na:data.na, ns:data.ns, pa:data.pa, ps:data.ps});
    $(axis).elements.rms.max = config.model === 2208 ? "1400" : "2000";
    $(axis + "Heading").textContent =
      `${label} · TMC${config.model} · адрес ${config.address}`;
    const connected = data[axis + "Connected"];
    const applied = data[axis + "Applied"];
    const sent = data[axis + "WritesSent"];
    const accepted = data[axis + "WritesAccepted"];
    $(axis + "Connection").textContent = !connected
      ? "ОШИБКА UART: драйвер не ответил нужной моделью (IOIN). Изменения в веб-настройках не означают изменения драйвера."
      : applied
        ? `UART подтверждён: драйвер принял ${accepted}/${sent} записей (IFCNT).`
        : `UART ответил, но применение параметров НЕ подтверждено (IFCNT ${accepted}/${sent}).`;
  }
  updateTopology();
  updateModes();
}

// Заполняем стартовый профиль только по нажатию; отправка и применение
// выполняются отдельной кнопкой «Сохранить двигатель» при остановленных осях.
const torqueProfile = {
  rms: 900, holdPercent: 50, micro: 16, toff: 5, blank: 24,
  hend: 0, hstart: 4, pwmo: 36, pwmgrad: 14, pwmfreq: 1,
  pwmreg: 8, pwmlim: 12, freewheel: 0, delay: 2,
  stealth: false, autoscale: true, autograd: false, intpol: true,
  iscale: false, irsense: false, msfilter: false,
};
for (const axis of ["nak", "pov"]) {
  const form = $(axis);
  form.querySelector("[data-torque-profile]").addEventListener("click", () => {
    populate(form, torqueProfile);
    updateModes();
    $("status").textContent =
      "Профиль введён для оси " + (axis === "nak" ? "Y" : "Z") +
      ". Проверьте ток NEMA17 и нажмите «Сохранить двигатель».";
  });
}

bus.addEventListener("change", updateTopology);
for (const id of ["bus", "nak", "pov"]) bindForm($(id), load);
load().catch(showError);

// Аппаратная проверка в состоянии полного покоя, не опрашивается автоматически
// во время генерации STEP (UART-read может ждать ответа несколько миллисекунд).
$("uartDiagnostics").addEventListener("click", async () => {
  const button = $("uartDiagnostics");
  button.disabled = true;
  const out = $("uartDiagnosticsResult");
  out.textContent = "Чтение регистров…";
  try {
    const d = await request("/api/settings/uart/diagnostics");
    const line = (name, r) => {
      const good = r.readOk && r.version === (r.model === 2208 ? 0x20 : 0x21);
      return `${name}: TMC${r.model} адрес ${r.address} · IOIN.VERSION=0x${r.version.toString(16)} · ` +
        `${good ? "UART OK" : "НЕТ ОТВЕТА / НЕВЕРНАЯ МОДЕЛЬ"} · IFCNT=${r.ifcnt} · ` +
        `${r.settingsApplied ? "настройки применены" : "настройки НЕ подтверждены"}` +
        (r.model === 2209 && r.readOk ? ` · MS1=${r.ms1}, MS2=${r.ms2}, SPREAD=${r.spreadEn}` : "");
    };
    const found = d.scan.filter(x => x.found).map(x => String(x.address));
    out.textContent = [
      line("Y", d.y), line("Z", d.z),
      `Найдены TMC2209 по адресам 1–3: ${found.length ? found.join(", ") : "нет"}`,
      "Если Z не найден: проверьте PDN_UART, RX GPIO47, TX GPIO21 через 1 кОм, общий GND, VIO, VMOT и аппаратную установку MS1/MS2.",
      "Если Y=TMC2208@0, а Z=TMC2209 тоже имеет адрес 0, ПО UART они конфликтуют: задайте Z адрес 1, 2 или 3 физически."
    ].join("\n");
    await load();
  } catch (error) {
    out.textContent = "Ошибка диагностики: " + (error?.message || error);
    showError(error);
  } finally { button.disabled = false; }
});

function updateModes() {
  for (const axis of ["nak", "pov"]) {
    const form = $(axis), e = form.elements;
    const stealth = e.stealth.checked, automatic = e.autoscale.checked;
    for (const input of form.querySelectorAll("input[name],select[name]")) {
      const name = input.name;
      const disabled =
        (["pwmo", "pwmgrad", "pwmfreq", "pwmreg", "pwmlim", "autoscale", "autograd"].includes(name) && !stealth) ||
        (["hend", "hstart"].includes(name) && stealth) ||
        (["pwmreg", "pwmlim", "autograd"].includes(name) && !automatic) ||
        (name === "pwmgrad" && e.autograd.checked && automatic) ||
        (name === "holdPercent" && !e.holdEnabled.checked) ||
        (name === "rms" && e.iscale.checked) ||
        (name === "freewheel" && (!stealth || !e.holdEnabled.checked || Number(e.holdPercent.value) > 0));
      input.disabled = disabled;
      input.dataset.modeDisabled = disabled ? "1" : "0";
      input.closest("label")?.classList.toggle("inactive", disabled);
    }
  }
}
for (const axis of ["nak", "pov"]) $(axis).addEventListener("change", updateModes);

// Значения неактивных полей сохраняются, чтобы они оставались доступны
// при последующем переключении режима, в котором используются.
function motorData(form) {
  const data = new URLSearchParams();
  for (const e of form.querySelectorAll("input[name],select[name]"))
    if (e.type !== "checkbox" || e.checked) data.set(e.name, e.value);
  return data;
}
let testHeld = false, testTimer = 0, testQueue = Promise.resolve();
function stopTest() {
  if (!testHeld) return;
  testHeld = false; clearInterval(testTimer);
  testQueue = testQueue.catch(()=>{}).then(()=>request("/api/motor-test", {action:"stop"})).catch(showError);
}
for (const button of document.querySelectorAll("[data-test]")) {
  button.addEventListener("pointerdown", event => {
    if (event.button !== 0) return;
    event.preventDefault();
    if (button.dataset.test === "stop") { stopTest(); return; }
    if (testHeld) return;
    const form = button.closest("form");
    // Неактивные поля выбранного режима также проверяются контроллером.
    if (!form.reportValidity()) return;
    testHeld = true; button.setPointerCapture(event.pointerId);
    testQueue = testQueue.catch(()=>{}).then(async()=>{
      await request(form.action, motorData(form));
      if (!testHeld) return;
      await request("/api/motor-test", {motor:form.id, action:button.dataset.test});
      if (!testHeld) {await request("/api/motor-test",{action:"stop"});return;}
      testTimer = setInterval(()=>request("/api/motor-test", {action:"heartbeat"}).catch(e=>{stopTest();showError(e);}),500);
    }).catch(e=>{stopTest();showError(e);});
  });
  for (const name of ["pointerup","pointercancel","lostpointercapture"]) button.addEventListener(name, stopTest);
  button.addEventListener("contextmenu", e=>e.preventDefault());
}
window.addEventListener("blur", stopTest);
document.addEventListener("visibilitychange",()=>{if(document.hidden)stopTest();});
