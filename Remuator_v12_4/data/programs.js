import { $, request, showError, programCards, bindCatalog } from "./app.js";
async function load() {
  const data = await request("/api/home");
  $("summary").textContent =
    `Найдено CSV: ${data.programs.length + data.invalid.length}. Готово к работе: ${data.programs.length}.`;
  $("programList").innerHTML = programCards(data, true);
}
bindCatalog($("programList"), load);
load().catch(showError);
