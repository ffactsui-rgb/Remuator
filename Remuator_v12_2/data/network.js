import {
  $,
  request,
  populate,
  bindForm,
  showError,
  clearError,
  date,
  zone,
  epoch,
  escape,
  status,
} from "./app.js";
async function load() {
  const data = await request("/api/network");
  populate($("networkForm"), data);
  $("addresses").textContent =
    `AP: ${data.apIP} · Домашняя Wi-Fi: ${data.staIP || "не подключена"}`;
  $("controllerClock").textContent =
    `Контроллер / OLED: ${data.localTime} · ${data.timezone}`;
  $("browserClock").textContent =
    `Календарь браузера: ${date(Number(epoch()))} · ${zone}`;
  for (const id of ["syncBrowser", "ntp", "scan"]) $(id).disabled = false;
}
bindForm($("networkForm"), load);
for (const id of ["syncBrowser", "ntp"])
  $(id).onclick = async () => {
    $(id).disabled = true;
    clearError();
    status("Синхронизация часов…");
    try {
      await request(
        id === "ntp" ? "/network/ntp" : "/api/time",
        id === "ntp" ? undefined : { epoch: epoch() },
      );
      await load();
      status(
        id === "ntp"
          ? "Запрос NTP отправлен. Часы обновятся после ответа сервера."
          : "Часы обновлены",
      );
    } catch (error) {
      showError(error);
      status("Синхронизация не подтверждена");
    } finally {
      $(id).disabled = false;
    }
  };
$("scan").onclick = async () => {
  $("scan").disabled = true;
  clearError();
  status("Поиск сетей…");
  try {
    const data = await request("/network/scan");
    $("ssidList").innerHTML = data.networks
      .map((n) => `<option value="${escape(n.ssid)}"></option>`)
      .join("");
    $("networks").innerHTML =
      '<div class="scroll"><table><thead><tr><th>SSID</th><th>RSSI</th><th>Канал</th><th>Доступ</th></tr></thead><tbody>' +
      data.networks
        .map(
          (n) =>
            `<tr><td>${escape(n.ssid)}</td><td>${n.rssi} dBm</td><td>${n.channel}</td><td>${n.open ? "Открытая" : "С паролем"}</td></tr>`,
        )
        .join("") +
      "</tbody></table></div>";
    status("Найдено сетей: " + data.networks.length);
  } catch (error) {
    showError(error);
  } finally {
    $("scan").disabled = false;
  }
};
load().catch(showError);
