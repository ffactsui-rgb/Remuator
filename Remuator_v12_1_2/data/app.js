// Shared browser helpers. All calendar values from the controller are UTC epochs.
export const $ = (id) => document.getElementById(id);
export const escape = (value) =>
  String(value).replace(
    /[&<>"']/g,
    (c) =>
      ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[
        c
      ],
  );
const calendar = new Intl.DateTimeFormat("ru-RU", {
  day: "2-digit",
  month: "2-digit",
  year: "numeric",
  hour: "2-digit",
  minute: "2-digit",
});
export const date = (epoch) =>
  epoch > 1700000000 ? calendar.format(epoch * 1000) : "Время не установлено";
const calendarSeconds = new Intl.DateTimeFormat("ru-RU", {
  day: "2-digit",
  month: "2-digit",
  year: "numeric",
  hour: "2-digit",
  minute: "2-digit",
  second: "2-digit",
});
export const dateSeconds = (epoch) =>
  epoch > 1700000000 ? calendarSeconds.format(epoch * 1000) : "—";
export const duration = (seconds) => {
  const value = Math.max(0, Math.floor(seconds));
  return [Math.floor(value / 3600), Math.floor(value / 60) % 60, value % 60]
    .map((x) => String(x).padStart(2, "0"))
    .join(":");
};
export const zone = Intl.DateTimeFormat().resolvedOptions().timeZone;
export const epoch = () => String(Math.floor(Date.now() / 1000));
export function showError(error) {
  const message = String(error?.message || error);
  const help = /fetch|network|сеть|Load failed/i.test(message)
    ? "Проверьте питание контроллера и подключение к его Wi-Fi, откройте страницу по текущему IP и повторите запрос. После перезагрузки дождитесь запуска контроллера."
    : /UART|IFCNT|IOIN/i.test(message)
    ? "Проверьте питание VIO/VMOT, общий GND, UART GPIO21/47 и разные аппаратные адреса драйверов."
    : /движ|BUSY|RUNNING/i.test(message)
    ? "Остановите движение или дождитесь его завершения и повторите действие."
    : /калибр|ADXL|горизонт/i.test(message)
    ? "Откройте настройки калибровки, проверьте датчик и используйте сохранённую таблицу либо выполните измерение."
    : /файл|LittleFS|CSV|HTTP|ответ/i.test(message)
    ? "Проверьте файл и свободное место в файловом менеджере; убедитесь, что прошивка и веб-файлы одной версии."
    : "Проверьте введённые значения и состояние контроллера, затем повторите действие.";
  $("error").textContent = message + " Как исправить: " + help;
  $("error").hidden = false;
}
export function clearError() {
  $("error").hidden = true;
}
export function status(message) {
  $("status").textContent = message;
}
export async function request(url, data) {
  const response = await fetch(url, {
    method: data ? "POST" : "GET",
    cache: "no-store",
    headers: {
      "X-Requested-With": "XMLHttpRequest",
      ...(data
        ? { "Content-Type": "application/x-www-form-urlencoded;charset=UTF-8" }
        : {}),
    },
    body: data ? new URLSearchParams(data) : undefined,
  });
  const content = await response.text();
  let value;
  try {
    value = JSON.parse(content);
  } catch {
    value = content;
  }
  if (!response.ok || value?.ok === false)
    throw new Error(
      value?.error || content || "Ошибка HTTP " + response.status,
    );
  return value;
}
export function populate(form, data) {
  if (!data || typeof data !== "object")
    throw new Error(
      "Неверный ответ контроллера. Обновите прошивку и веб-файлы одной версии.",
    );
  for (const input of form.querySelectorAll("input[name], select[name]")) {
    if (!(input.name in data)) continue;
    if (input.type === "checkbox") input.checked = Boolean(data[input.name]);
    else input.value = data[input.name];
  }
  const fieldset = form.querySelector("fieldset");
  if (fieldset) fieldset.disabled = false;
}
export function bindForm(form, after) {
  form.addEventListener("submit", async (event) => {
    event.preventDefault();
    clearError();
    const data = new URLSearchParams(new FormData(form));
    for (const input of form.querySelectorAll('[data-mode-disabled="1"]'))
      if (input.type !== "checkbox" || input.checked) data.set(input.name, input.value);
    const fieldset = form.querySelector("fieldset");
    if (fieldset) fieldset.disabled = true;
    status("Сохранение…");
    try {
      await request(form.getAttribute("action"), data);
      status("Настройки сохранены");
      if (after) await after();
    } catch (error) {
      showError(error);
      status("Нет подтверждения сохранения");
    } finally {
      if (fieldset) fieldset.disabled = false;
    }
  });
}
export const managerUrl = (path) => {
  const url = new URL(location.href);
  url.port = "8080";
  url.pathname = "/";
  url.search = path ? "?edit=" + encodeURIComponent(path) : "";
  url.hash = "";
  return url.href;
};
for (const link of document.querySelectorAll("[data-manager]"))
  link.href = managerUrl();
for (const link of document.querySelectorAll("[data-manager-dir]")) {
  const url = new URL(managerUrl());
  url.searchParams.set("dir", link.dataset.managerDir);
  link.href = url.href;
}
export function programCards(data, full = false, canStart = false) {
  return (
    data.programs
      .map(
        (p) =>
          `<article class="card ${p.path === data.selectedPath ? "selected-program" : ""}"><h2>${escape(p.name)}</h2><p><code>${escape(p.path)}</code></p><p>${p.count} шагов · ${duration(p.seconds)} ${p.path === data.selectedPath ? "· ✓ Выбрана" : ""}</p>${p.warning ? `<p class="warning">${escape(p.warning)}</p>` : ""}<div class="toolbar"><button data-select="${escape(p.path)}" ${data.running ? "disabled" : ""}>Выбрать</button>${canStart ? `<button class="primary" data-start-program="${escape(p.path)}" ${data.running || data.homing || !data.calibrationReady ? "disabled" : ""}>▶ Запустить</button>` : ""}${full ? `<button class="danger" data-restore="${escape(p.path)}" ${data.running ? "disabled" : ""}>Восстановить шаблон</button>` : ""}</div></article>`,
      )
      .join("") +
    data.invalid
      .map(
        (p) =>
          `<article class="card"><h2>Ошибка CSV</h2><p><code>${escape(p.path)}</code></p><p>${escape(p.error)}</p><a class="button" href="${escape(managerUrl(p.path))}">Исправить CSV</a><button class="danger" data-restore="${escape(p.path)}" ${data.running ? "disabled" : ""}>Восстановить шаблон</button></article>`,
      )
      .join("")
  );
}
export function bindCatalog(container, reload) {
  container.addEventListener("click", async (event) => {
    const button = event.target.closest("[data-select],[data-restore]");
    if (!button) return;
    const restore = button.hasAttribute("data-restore"),
      path = restore ? button.dataset.restore : button.dataset.select;
    if (
      restore &&
      !confirm("Заменить содержимое " + path + " исходным шаблоном?")
    )
      return;
    button.disabled = true;
    clearError();
    try {
      await request(
        "/program/" +
          (restore ? "restore" : "select") +
          "?path=" +
          encodeURIComponent(path),
      );
      await reload();
    } catch (error) {
      showError(error);
      button.disabled = false;
    }
  });
}
