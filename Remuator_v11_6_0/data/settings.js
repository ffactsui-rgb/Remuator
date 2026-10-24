import { $, request, populate, bindForm, showError, clearError, status } from "./app.js";

async function load() {
  const data = await request("/api/settings");
  populate($("settingsForm"), data);
  $("controllerClock").textContent =
    `Часы контроллера: ${data.localTime} · ${data.tz} · источник: ${data.clockSource}`;
  $("rtcStatus").textContent =
    !data.rtcPresent ? "DS3231 не обнаружен: проверьте I²C GPIO2/GPIO1 и питание" :
    !data.rtcValid ? "DS3231 обнаружен, но время недостоверно (OSF или неверная дата)" :
    "DS3231: подключён, время достоверно";
  if (!$("manualTime").value) {
    const local = new Date();
    const shifted = new Date(local.getTime() - local.getTimezoneOffset() * 60000);
    $("manualTime").value = shifted.toISOString().slice(0, 19);
  }
}

bindForm($("settingsForm"), load);

for (const action of ["manualSet", "browserSet", "ntpSync"]) {
  $(action).addEventListener("click", async () => {
    clearError();
    $(action).disabled = true;
    try {
      let response;
      if (action === "ntpSync") {
        response = await request("/network/ntp");
      } else {
        const value = action === "browserSet" ? Date.now() : new Date($("manualTime").value).getTime();
        if (!Number.isFinite(value) || value < 1700000000000 || value > 4102444799000)
          throw new Error("Некорректная дата и время");
        response = await request("/api/time", { epoch: String(Math.floor(value / 1000)) });
      }
      status(action === "ntpSync" ? "Запрос NTP отправлен. Обновите статус после ответа сервера." :
        "Часы ESP32 установлены; DS3231 обновится, когда двигатели остановлены.");
      await load();
    } catch (error) {
      showError(error);
    } finally {
      $(action).disabled = false;
    }
  });
}
load().catch(showError);
