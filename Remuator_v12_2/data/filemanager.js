let cwd = "/",
  active = "";
const $ = (id) => document.getElementById(id);
$("back").href =
  `${location.protocol}//${location.hostname}/?reload=${Date.now()}`;
function msg(t, bad = false) {
  $("message").textContent = bad ? t + " Как исправить: " + (/fetch|network|Load failed/i.test(String(t))
    ? "Проверьте питание, подключение к Wi-Fi контроллера и его IP; файловый менеджер работает на порту 8080. Повторите запрос."
    : "Проверьте путь, имя файла и свободное место; остановите двигатели перед изменением файлов и повторите действие.") : t;
  $("message").className = bad ? "error" : "";
}
function esc(s) {
  return String(s).replace(
    /[&<>"']/g,
    (c) =>
      ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[
        c
      ],
  );
}
function validName(s) {
  return s && !s.includes("/") && !s.includes("\\") && s !== "." && s !== "..";
}
function join(dir, name) {
  return (dir === "/" ? "" : dir) + "/" + name;
}
async function fetchApi(url, opts) {
  let r = await fetch(url, { cache: "no-store", ...opts });
  if (!r.ok) {
    let t = await r.text();
    throw Error(t || "HTTP " + r.status);
  }
  return r;
}
async function refresh() {
  try {
    let r = await fetchApi("/api/list?dir=" + encodeURIComponent(cwd));
    let j = await r.json();
    const url = new URL(location.href);
    url.search = "?dir=" + encodeURIComponent(cwd);
    history.replaceState(null, "", url);
    $("folder").textContent =
      cwd + " (" + j.used + "/" + j.total + " байт занято)";
    const rows = j.entries
      .map(
        (f, i) =>
          `<tr><td>${f.directory ? "📁 " : "📄 "}<a href="#" data-open="${i}">${esc(f.name)}</a></td><td>${f.directory ? "—" : f.size}</td><td>${f.directory ? "Папка" : "Файл"}</td><td class="actions">${f.directory ? "" : `<button class="small" data-edit="${i}">Текст</button> <button class="small" data-download="${i}">↓</button>`} <button class="small" data-rename="${i}">Имя</button> <button class="small danger" data-delete="${i}">Удалить</button></td></tr>`,
      )
      .join("");
    $("entries").innerHTML =
      rows || '<tr><td colspan="4">Здесь нет файлов</td></tr>';
    $("entries")
      .querySelectorAll(
        "a[data-open],button[data-edit],button[data-download],button[data-rename],button[data-delete]",
      )
      .forEach((x) =>
        x.addEventListener("click", async (e) => {
          e.preventDefault();
          let type = ["open", "edit", "download", "rename", "delete"].find(
            (k) => x.dataset[k] !== undefined,
          );
          let f = j.entries[+x.dataset[type]];
          let path = join(cwd, f.name);
          if (type === "open") {
            if (f.directory) {
              cwd = path;
              refresh();
            } else edit(path);
          }
          if (type === "edit") edit(path);
          if (type === "download")
            location.href = "/api/download?path=" + encodeURIComponent(path);
          if (type === "rename") rename(path, f.name);
          if (type === "delete") del(path, f.directory);
        }),
      );
    msg("");
  } catch (e) {
    msg("Ошибка списка: " + e.message, true);
  }
}
function up() {
  if (cwd !== "/") {
    cwd = cwd.substring(0, cwd.lastIndexOf("/")) || "/";
    refresh();
  }
}
async function post(endpoint, args) {
  try {
    let r = await fetchApi(endpoint + "?" + new URLSearchParams(args), {
      method: "POST",
    });
    msg(await r.text());
    await refresh();
  } catch (e) {
    msg("Ошибка: " + e.message, true);
  }
}
async function mkdir() {
  let name = $("newfolder").value.trim();
  if (!validName(name)) return msg("Введите имя папки без / и \\", true);
  await post("/api/mkdir", { path: join(cwd, name) });
  $("newfolder").value = "";
}
async function rename(path, name) {
  let n = prompt("Новое имя файла/папки", name);
  if (n === null) return;
  n = n.trim();
  if (!validName(n)) return msg("Неправильное имя", true);
  await post("/api/rename", { from: path, to: join(cwd, n) });
}
async function del(path, isdir) {
  if (
    !confirm(
      "Удалить " + path + "?" + (isdir ? " Папка должна быть пустой." : ""),
    )
  )
    return;
  await post("/api/delete", { path });
}
async function edit(path) {
  try {
    let r = await fetchApi("/api/read?path=" + encodeURIComponent(path));
    $("edittext").value = await r.text();
    active = path;
    $("editname").textContent = path;
    $("editor").style.display = "block";
    $("editor").scrollIntoView();
  } catch (e) {
    msg("Нельзя открыть в текстовом редакторе: " + e.message, true);
  }
}
function closeEditor() {
  $("editor").style.display = "none";
  active = "";
}
async function saveText() {
  if (!active) return;
  try {
    let r = await fetchApi("/api/save", {
      method: "POST",
      headers: {
        "Content-Type": "application/x-www-form-urlencoded;charset=UTF-8",
      },
      body: new URLSearchParams({ path: active, content: $("edittext").value }),
    });
    msg(await r.text());
    closeEditor();
    await refresh();
  } catch (e) {
    msg("Ошибка сохранения: " + e.message, true);
  }
}
async function upload() {
  const fs = [...$("files").files];
  if (!fs.length) return msg("Выберите файлы", true);
  for (const f of fs) {
    let form = new FormData();
    form.append("file", f, f.name);
    try {
      await fetchApi("/api/upload?dir=" + encodeURIComponent(cwd), {
        method: "POST",
        body: form,
      });
      msg("Загружен: " + f.name);
    } catch (e) {
      msg("Ошибка загрузки " + f.name + ": " + e.message, true);
      break;
    }
  }
  $("files").value = "";
  await refresh();
}
const drop = $("drop");
drop.addEventListener("dragover", (e) => {
  e.preventDefault();
  drop.classList.add("drag");
});
drop.addEventListener("dragleave", () => drop.classList.remove("drag"));
drop.addEventListener("drop", (e) => {
  e.preventDefault();
  drop.classList.remove("drag");
  if (e.dataTransfer.files.length) {
    $("files").files = e.dataTransfer.files;
    upload();
  }
});
function validPath(path) {
  return (
    typeof path === "string" &&
    path.startsWith("/") &&
    !path.includes("..") &&
    !/[\\:#?]/.test(path) &&
    !path.includes("//") &&
    path.length <= 128
  );
}
(async () => {
  const query = new URLSearchParams(location.search);
  const requestedFile = query.get("edit"),
    requestedDir = query.get("dir");
  if (requestedFile && validPath(requestedFile)) {
    cwd = requestedFile.substring(0, requestedFile.lastIndexOf("/")) || "/";
    await refresh();
    await edit(requestedFile);
  } else {
    cwd =
      requestedDir && validPath(requestedDir)
        ? requestedDir.replace(/\/$/, "") || "/"
        : "/";
    await refresh();
  }
})();

// Bind controls here so the HTML contains no inline JavaScript.
$("button-up").addEventListener("click", up);
$("button-refresh").addEventListener("click", refresh);
$("button-upload").addEventListener("click", upload);
$("button-mkdir").addEventListener("click", mkdir);
$("button-saveText").addEventListener("click", saveText);
$("button-closeEditor").addEventListener("click", closeEditor);
