"use strict";
(() => {
  const $ = (id) => document.getElementById(id),
    table = $("rows");
  let model = null,
    rows = [],
    generation = 0,
    ackGeneration = 0,
    timer = 0,
    sending = null,
    blocked = false,
    saving = false;
  const fmt = new Intl.DateTimeFormat("ru-RU", {
    day: "2-digit",
    month: "2-digit",
    year: "numeric",
    hour: "2-digit",
    minute: "2-digit",
  });
  $("zone").textContent =
    "Часовой пояс браузера: " +
    Intl.DateTimeFormat().resolvedOptions().timeZone;
  function report(text, bad = false) {
    $("status").textContent = text;
    $("status").className = bad ? "error" : "";
  }
  function showError(text, path = "") {
    $("error").replaceChildren(document.createTextNode(text + " Как исправить: " + (/fetch|network|Load failed/i.test(String(text)) ? "Проверьте питание контроллера, Wi-Fi и текущий IP, затем повторите запрос." : "Проверьте значения в таблице и ответ контроллера; при занятости дождитесь остановки и повторите действие.")));
    if (path) {
      let a = document.createElement("a");
      a.textContent = " Открыть текущий черновик";
      a.href = "/program/edit?path=" + encodeURIComponent(path);
      $("error").append(a);
    }
    $("error").style.display = "block";
  }
  function clearError() {
    $("error").style.display = "none";
  }
  function escape(s) {
    return String(s).replace(
      /[&<>"']/g,
      (c) =>
        ({
          "&": "&amp;",
          "<": "&lt;",
          ">": "&gt;",
          '"': "&quot;",
          "'": "&#39;",
        })[c],
    );
  }
  function angle(v) {
    let s = String(v).trim();
    if (!/^[+-]?\d+(?:[.,]\d{1,2})?$/.test(s)) return NaN;
    return Math.round(Number(s.replace(",", ".")) * 100);
  }
  function rotation(v) {
    let s = String(v).trim();
    return /^[+-]?\d+$/.test(s) ? Number(s) : NaN;
  }
  function pause(value) {
    const match = String(value)
      .trim()
      .match(/^(\d+)(?::(\d{2}))?(?::(\d{2}))?$/);
    if (!match || Number(match[2] || 0) > 59 || Number(match[3] || 0) > 59)
      return NaN;
    return (
      Number(match[1]) * 3600 +
      Number(match[2] || 0) * 60 +
      Number(match[3] || 0)
    );
  }
  function number(v) {
    return Number.isFinite(v)
      ? new Intl.NumberFormat("ru-RU", { maximumFractionDigits: 2 }).format(v)
      : "—";
  }
  function relative(seconds) {
    if (!Number.isFinite(seconds)) return "—";
    return (
      Math.floor(seconds / 86400) +
      "д " +
      Math.floor((seconds % 86400) / 3600) +
      "ч " +
      Math.floor((seconds % 3600) / 60) +
      "м " +
      (seconds % 60) +
      "с"
    );
  }
  function derived() {
    let y = 2300,
      rot = 0,
      time = 0,
      valid = true;
    return rows.map((r) => {
      let iy = angle(r.incY),
        ir = rotation(r.incRot),
        p = pause(r.pause);
      y += iy;
      rot += ir;
      let ok =
        Number.isFinite(iy) &&
        Math.abs(iy) <= 36099 &&
        Number.isInteger(ir) &&
        Math.abs(ir) <= 1600 &&
        Number.isFinite(p) &&
        p >= 0 &&
        y >= 2300 &&
        time + p <= 31536000;
      valid = valid && ok;
      let x = {
        y: y / 100,
        rot: ir * 22.5,
        orientation: (((rot % 16) + 16) % 16) * 22.5,
        start: time,
        pause: p,
        valid,
      };
      time += p;
      return x;
    });
  }
  function bottle(a) {
    if (!Number.isFinite(a)) return "—";
    return (
      '<span class="bottle"><svg viewBox="0 0 40 40" role="img" aria-label="Поворот ' +
      a +
      ' градусов"><circle cx="20" cy="20" r="16" fill="#dcece1" stroke="#23624a" stroke-width="2"/><line x1="20" y1="35" x2="20" y2="25" stroke="#23624a" stroke-width="3" stroke-linecap="round" transform="rotate(' +
      a +
      ' 20 20)"/><circle cx="20" cy="20" r="2" fill="#23624a"/></svg><small>' +
      number(a) +
      "°</small></span>"
    );
  }
  function recalc() {
    const d = derived(),
      now = Date.now();
    d.forEach((r, i) => {
      let tr = table.children[i];
      if (!tr) return;
      tr.querySelector('[data-calc="relative"]').textContent = relative(
        r.start,
        true,
      );
      tr.querySelector('[data-calc="date"]').textContent = Number.isFinite(
        r.start,
      )
        ? fmt.format(now + r.start * 1000)
        : "—";
      tr.querySelector('[data-calc="y"]').textContent = number(r.y);
      tr.querySelector('[data-calc="rot"]').textContent = number(r.rot);
      tr.querySelector('[data-calc="bottle"]').innerHTML = bottle(
        r.orientation,
      );
      tr.querySelector('[data-field="incY"]').classList.toggle(
        "invalid",
        !Number.isFinite(r.y) || r.y < 23,
      );
    });
    $("rangeWarning").hidden = !d.some((r) => r.y > 85);
    $("rangeWarning").textContent =
      "Y выше 85°: файл разрешён с предупреждением. Наклон Y ограничен концевиком GPIO13 (85°); поиск контролируется защитным тайм-аутом 180 секунд.";
    let total = d.length ? d[d.length - 1].start + d[d.length - 1].pause : 0;
    let ok = d.length > 0 && d.every((r) => r.valid);
    $("duration").textContent =
      number(total / 3600) + " ч · " + relative(total, false);
    $("finish").textContent =
      Number.isFinite(total) && total <= 31536000
        ? fmt.format(now + total * 1000)
        : "—";
    $("count").textContent = rows.length + " / 80";
    $("save").disabled = !model || !ok || blocked || saving;
    $("saveAs").disabled = !model || !ok || blocked || saving;
    $("download").disabled = !model || !ok || blocked || saving;
    $("add").disabled = !model || rows.length >= 80 || blocked || saving;
    return ok;
  }
  function render() {
    table.innerHTML = rows
      .map(
        (r, i) =>
          '<tr><td class="num">' +
          (i + 1) +
          '</td><td data-calc="relative"></td><td data-calc="date"></td><td><input data-field="incY" aria-label="Шаг ' +
          (i + 1) +
          ': incAngle" value="' +
          escape(r.incY) +
          '" inputmode="decimal"></td><td class="calc" data-calc="y"></td><td><input data-field="incRot" aria-label="Шаг ' +
          (i + 1) +
          ': IncRot" value="' +
          escape(r.incRot) +
          '" inputmode="text"></td><td class="calc" data-calc="rot"></td><td class="calc" data-calc="bottle"></td><td><input data-field="pause" aria-label="Шаг ' +
          (i + 1) +
          ': Пауза" value="' +
          escape(r.pause) +
          '" inputmode="text"></td><td><div class="actions"><button class="rowbtn" data-action="insert" ' +
          (rows.length >= 80 ? "disabled" : "") +
          '>+ Вставить после</button><button class="rowbtn danger" data-action="delete" ' +
          (rows.length <= 1 ? "disabled" : "") +
          ">Удалить</button></div></td></tr>",
      )
      .join("");
    recalc();
  }
  function csv() {
    return (
      "Step;incAngle;Angle;IncRot;Rot;Pause\n" +
      rows
        .map((r, i) =>
          [
            i + 1,
            String(r.incY).trim(),
            " ",
            String(r.incRot).trim(),
            " ",
            String(r.pause).trim(),
          ].join(";"),
        )
        .join("\n") +
      "\n"
    );
  }
  function args(extra = {}) {
    return {
      path: model.path,
      token: model.token,
      revision: String(model.revision),
      ...extra,
    };
  }
  async function request(url, data) {
    const response = await fetch(url, {
      method: data ? "POST" : "GET",
      cache: "no-store",
      headers: data
        ? { "Content-Type": "application/x-www-form-urlencoded;charset=UTF-8" }
        : {},
      body: data ? new URLSearchParams(data) : undefined,
    });
    let result;
    try {
      result = await response.json();
    } catch (e) {
      throw new Error("Нет корректного ответа от контроллера");
    }
    if (!response.ok || !result.ok) {
      let e = new Error(result.error || "Ошибка HTTP " + response.status);
      e.status = response.status;
      e.path = result.draftPath;
      throw e;
    }
    return result;
  }
  function changed() {
    generation++;
    clearError();
    report("Изменения ожидают передачи в контроллер…");
    recalc();
    clearTimeout(timer);
    timer = setTimeout(() => sync().catch(() => {}), 450);
  }
  async function sync() {
    clearTimeout(timer);
    if (sending) return sending;
    sending = (async () => {
      while (ackGeneration !== generation) {
        if (blocked) throw new Error("Откройте редактор заново");
        if (!recalc()) {
          report(
            "Проверьте значения: Y не ниже 23°, IncRot — целое, пауза — ЧЧ:ММ:СС.",
            true,
          );
          throw new Error("Некорректные значения");
        }
        const sent = generation;
        try {
          let next = await request(
            "/api/editor/update",
            args({ name: $("name").value, csv: csv() }),
          );
          model = next;
          ackGeneration = sent;
          report(
            sent === generation
              ? "Черновик в памяти контроллера · файл ещё не сохранён"
              : "Передаю следующие изменения…",
          );
          clearError();
        } catch (e) {
          if (e.status === 409) blocked = true;
          showError(e.message, e.status === 409 ? e.path : "");
          report("Изменения не переданы: " + e.message, true);
          throw e;
        }
      }
    })().finally(() => {
      sending = null;
      recalc();
    });
    return sending;
  }
  function accept(next) {
    model = next;
    rows = next.rows.map((r) => ({
      incY: String(r.incY),
      incRot: String(r.incRot),
      pause: r.pause,
    }));
    generation = 0;
    ackGeneration = 0;
    blocked = false;
    $("name").value = next.name;
    $("name").disabled = false;
    $("path").textContent = next.path;
    $("discard").disabled = false;
    $("running").style.display = next.running ? "block" : "none";
    history.replaceState(
      null,
      "",
      "/program/edit?" + (document.documentElement.classList.contains("embedded-editor") ? "embed=1&" : "") +
        "path=" + encodeURIComponent(next.path),
    );
    render();
    report(
      next.dirty
        ? "Черновик в памяти · файл ещё не сохранён"
        : "Файл загружен в память контроллера",
    );
  }
  function lock(on) {
    saving = on;
    $("confirmAs").disabled = on;
    table.querySelectorAll("input,button").forEach((x) => (x.disabled = on));
    $("name").disabled = on;
    $("discard").disabled = on;
    recalc();
    if (!on) render();
  }
  async function save(as, name) {
    if (saving) return;
    lock(true);
    try {
      await sync();
      let next = await request(
        "/api/editor/save",
        args({ saveAs: as ? "1" : "0", name: name || $("name").value }),
      );
      accept(next);
      clearError();
      report("Сохранено: " + next.path);
      if (next.warning) showError(next.warning);
      $("saveDialog").close();
    } catch (e) {
      showError(e.message);
      report("Файл не сохранён: " + e.message, true);
    } finally {
      lock(false);
    }
  }
  table.addEventListener("input", (e) => {
    let f = e.target.dataset.field;
    if (!f) return;
    let i = Array.from(table.children).indexOf(e.target.closest("tr"));
    rows[i][f] = e.target.value;
    changed();
  });
  table.addEventListener("click", (e) => {
    let b = e.target.closest("[data-action]");
    if (!b || blocked || saving) return;
    let i = Array.from(table.children).indexOf(b.closest("tr"));
    if (b.dataset.action === "insert" && rows.length < 80)
      rows.splice(i + 1, 0, { incY: "0", incRot: "0", pause: "01:00:00" });
    else if (b.dataset.action === "delete" && rows.length > 1)
      rows.splice(i, 1);
    render();
    changed();
    revealRow(
      Math.min(b.dataset.action === "insert" ? i + 1 : i, rows.length - 1),
    );
  });
  $("name").addEventListener("input", changed);
  $("add").onclick = () => {
    if (rows.length >= 80) return;
    rows.push({ incY: "0", incRot: "0", pause: "01:00:00" });
    render();
    changed();
    revealRow(rows.length - 1);
  };
  $("save").onclick = () => save(false);
  $("saveAs").onclick = () => {
    $("newName").value = $("name").value + " — копия";
    $("saveDialog").showModal();
    $("newName").focus();
  };
  $("cancelAs").onclick = () => $("saveDialog").close();
  $("confirmAs").onclick = () => save(true, $("newName").value);
  $("discard").onclick = async () => {
    if (
      !confirm(
        "Загрузить исходный файл заново? Изменения черновика будут потеряны.",
      )
    )
      return;
    clearTimeout(timer);
    lock(true);
    try {
      if (sending) await sending.catch(() => {});
      accept(await request("/api/editor/discard", args()));
      clearError();
    } catch (e) {
      showError(e.message);
    } finally {
      lock(false);
    }
  };
  window.addEventListener("message", async (event) => {
    if (event.origin !== location.origin || event.data?.type !== "remuage-flush") return;
    try {
      if (!model || saving || blocked) throw new Error("Редактор ещё не готов или заблокирован");
      await sync();
      event.source.postMessage({type:"remuage-flush-response",token:event.data.token,ok:true},event.origin);
    } catch (error) {
      event.source.postMessage({type:"remuage-flush-response",token:event.data.token,ok:false,message:String(error.message||error)},event.origin);
    }
  });
  window.addEventListener("beforeunload", (e) => {
    if (generation !== ackGeneration || model?.dirty) {
      e.preventDefault();
      e.returnValue = "";
    }
  });
  // Фокус не прокручивает документ; изменяется только прокрутка контейнера таблицы.
  function revealRow(index) {
    requestAnimationFrame(() => {
      const row = table.children[index],
        viewport = $("tableScroll");
      if (!row) return;
      const input = row.querySelector("input");
      input.focus({ preventScroll: true });
      input.select();
      const r = row.getBoundingClientRect(),
        v = viewport.getBoundingClientRect();
      const header = viewport
        .querySelector("thead")
        .getBoundingClientRect().height;
      if (r.bottom > v.bottom) viewport.scrollTop += r.bottom - v.bottom + 8;
      if (r.top < v.top + header)
        viewport.scrollTop -= v.top + header - r.top + 8;
      const field = input.getBoundingClientRect();
      if (field.right > v.right)
        viewport.scrollLeft += field.right - v.right + 16;
      if (field.left < v.left) viewport.scrollLeft -= v.left - field.left + 16;
      row.classList.add("just-added");
      setTimeout(() => row.classList.remove("just-added"), 1800);
    });
  }
  $("download").onclick = async () => {
    if (saving) return;
    lock(true);
    try {
      await sync();
      const response = await fetch("/api/editor/download", {
        method: "POST",
        cache: "no-store",
        headers: {
          "Content-Type": "application/x-www-form-urlencoded;charset=UTF-8",
        },
        body: new URLSearchParams(args()),
      });
      if (!response.ok) {
        let result = await response.json();
        throw new Error(result.error || "Ошибка скачивания");
      }
      const blob = await response.blob(),
        url = URL.createObjectURL(blob);
      const a = document.createElement("a");
      a.href = url;
      a.download = model.name + ".csv";
      document.body.append(a);
      a.click();
      a.remove();
      setTimeout(() => URL.revokeObjectURL(url), 30000);
      report(
        "CSV скачан" +
          (model.dirty ? " · файл в контроллере ещё не сохранён" : ""),
      );
    } catch (e) {
      showError(e.message);
      report("CSV не скачан: " + e.message, true);
    } finally {
      lock(false);
    }
  };
  setInterval(() => {
    if (model) recalc();
  }, 1000);
  (async () => {
    try {
      const q = new URLSearchParams(location.search);
      let query = q.has("path")
        ? "?path=" + encodeURIComponent(q.get("path"))
        : q.has("p")
          ? "?p=" + encodeURIComponent(q.get("p"))
          : "";
      accept(await request("/api/editor" + query));
    } catch (e) {
      showError(e.message, e.path);
      report(e.message, true);
    }
  })();
})();
