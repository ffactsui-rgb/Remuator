#pragma once
// Подключается после обработчиков CSV и файловой системы; изменения защищены ControlGuard.

inline String editorJsonQuote(const String &text) {
  String out = "\"";
  for (size_t i = 0; i < text.length(); i++) {
    unsigned char c = text[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += (char)c;
    } else if (c == '\n')
      out += "\\n";
    else if (c == '\r')
      out += "\\r";
    else if (c == '\t')
      out += "\\t";
    else if (c < 0x20)
      out += ' ';
    else
      out += (char)c;
  }
  return out + '"';
}

String draftToken;

inline uint32_t editorFileHash(const String &path, bool &found) {
  File f = LittleFS.open(path, "r");
  found = f && !f.isDirectory();
  if (!found) {
    if (f)
      f.close();
    return 0;
  }
  uint32_t h = 2166136261UL;
  uint8_t buf[256];
  while (f.available()) {
    size_t n = f.read(buf, sizeof(buf));
    for (size_t i = 0; i < n; i++) {
      h ^= buf[i];
      h *= 16777619UL;
    }
  }
  f.close();
  return h;
}

inline bool editorName(String &name, String &error) {
  name.trim();
  String lower = name;
  lower.toLowerCase();
  if (lower.endsWith(".csv"))
    name.remove(name.length() - 4);
  name.trim();
  if (!name.length() || name.length() > 110 || name.indexOf("..") >= 0) {
    error = "Имя файла: от 1 до 110 байт UTF-8, без '..'";
    return false;
  }
  for (size_t i = 0; i < name.length(); i++) {
    if ((uint8_t)name[i] < 32 || strchr("/\\:*?\"<>|#", name[i])) {
      error = "В имени недопустимы / \\ : * ? кавычки < > | # и управляющие символы";
      return false;
    }
  }
  return true;
}

bool openProgramDraft(const String &path, String &error) {
  if (draftLoaded && path == draftPath) {
    if (draftDirty)
      return true;
    bool found = false;
    if (editorFileHash(path, found) == draftSourceHash && found)
      return true;
    // Чистый черновик подхватывает внешние изменения CSV при следующем открытии или выборе.
  }
  if (draftLoaded && draftDirty) {
    error = "Есть несохранённый черновик " + draftPath +
            ". Откройте его, сохраните файл или нажмите «Отменить изменения».";
    return false;
  }
  std::unique_ptr<RemuageProgram> next(new RemuageProgram{});
  if (!readProgramCsv(path, *next, &error))
    return false;
  bool found = false;
  uint32_t hash = editorFileHash(path, found);
  if (!found) {
    error = "Файл исчез во время чтения";
    return false;
  }
  draftProgram = *next;
  draftPath = path;
  draftLoaded = true;
  draftDirty = false;
  draftRevision = 1;
  draftSourceHash = hash;
  draftToken = String(esp_random(), HEX) + String(esp_random(), HEX);
  return true;
}

inline String editorSnapshot() {
  String json =
      "{\"ok\":true,\"token\":" + editorJsonQuote(draftToken) +
      ",\"revision\":" + String(draftRevision) + ",\"path\":" + editorJsonQuote(draftPath) +
      ",\"name\":" + editorJsonQuote(draftProgram.name) +
      ",\"dirty\":" + (draftDirty ? "true" : "false") +
      ",\"maxSteps\":" + String(MAX_PROGRAM_STEPS) +
      ",\"running\":" + (programRunning ? "true" : "false") +
      ",\"durationSeconds\":" + String(programDurationSeconds(draftProgram)) + ",\"rows\":[";
  for (uint16_t i = 0; i < draftProgram.count; i++) {
    const ProgramStep &s = draftProgram.steps[i];
    if (i)
      json += ',';
    json += "{\"number\":" + String(s.number) + ",\"incY\":" + String(s.incAngleCenti / 100.0f, 2) +
            ",\"y\":" + String(s.angleNaklon, 2) + ",\"incRot\":" + String(s.incRot) +
            ",\"rot\":" + String(s.anglePovorot, 1) +
            ",\"orientation\":" + String(programOrientationParts(s.rotationParts) * 22.5f, 1) +
            ",\"pause\":" + editorJsonQuote(programPauseText(s.pauseSeconds).c_str()) +
            ",\"startSeconds\":" + String(s.startSecond) + "}";
  }
  return json + "]}";
}

inline void editorError(int code, const String &message) {
  server.send(code, "application/json; charset=utf-8",
              "{\"ok\":false,\"error\":" + editorJsonQuote(message) +
                  ",\"draftPath\":" + editorJsonQuote(draftPath) + "}");
}

inline bool editorCheckRevision() {
  int32_t revision = 0;
  if (!draftLoaded || server.arg("token") != draftToken || server.arg("path") != draftPath ||
      !programInteger(server.arg("revision").c_str(), revision, 1, INT32_MAX) ||
      (uint32_t)revision != draftRevision) {
    editorError(409, "Черновик изменён в другой вкладке или контроллер перезапущен. Скопируйте "
                     "свои изменения, затем откройте редактор заново.");
    return false;
  }
  return true;
}

void handleProgramEdit() { sendLittleFSFile("/edit.html"); }

void handleEditorGet() {
  String path = server.arg("path");
  if (!path.length() && server.hasArg("p")) {
    refreshPrograms(true);
    int32_t p = 0;
    if (programInteger(server.arg("p").c_str(), p, 0, (int32_t)programPaths.size() - 1))
      path = programPaths[p];
  }
  if (!path.length())
    path = draftLoaded ? draftPath : selectedProgramPath;
  String error;
  if (!openProgramDraft(path, error)) {
    editorError(409, error);
    return;
  }
  server.send(200, "application/json; charset=utf-8", editorSnapshot());
}

void handleEditorUpdate() {
  if (!editorCheckRevision())
    return;
  String name = server.arg("name"), error;
  if (!editorName(name, error)) {
    editorError(422, error);
    return;
  }
  std::unique_ptr<RemuageProgram> next(new RemuageProgram{});
  std::string reason;
  if (!parseProgramCsv(server.arg("csv").c_str(), *next, reason)) {
    editorError(422, reason.c_str());
    return;
  }
  copyProgramNameUtf8(next->name, sizeof(next->name), name);
  draftProgram = *next;
  draftDirty = true;
  ++draftRevision;
  server.send(200, "application/json; charset=utf-8", editorSnapshot());
}

void handleEditorSave() {
  if (!editorCheckRevision())
    return;
  bool saveAs = server.arg("saveAs") == "1";
  String name = server.arg("name"), error;
  if (!editorName(name, error)) {
    editorError(422, error);
    return;
  }
  String target = "/prog/" + name + ".csv";
  bool exists = false;
  uint32_t sourceHash = editorFileHash(draftPath, exists);
  if (!saveAs && (!exists || sourceHash != draftSourceHash)) {
    editorError(409, "Исходный файл изменён или удалён через файловый менеджер. Используйте "
                     "«Сохранить как» с новым именем.");
    return;
  }
  if ((saveAs || target != draftPath) && LittleFS.exists(target)) {
    editorError(409, "Файл с таким именем уже существует. Укажите другое имя.");
    return;
  }
  if (saveAs && target == draftPath) {
    editorError(409, "Для «Сохранить как» нужно новое имя файла");
    return;
  }
  std::unique_ptr<RemuageProgram> next(new RemuageProgram(draftProgram));
  copyProgramNameUtf8(next->name, sizeof(next->name), name);
  if (!writeProgramCsv(target, *next)) {
    editorError(507, "Не удалось записать CSV. Исходный файл сохранён.");
    return;
  }
  String oldPath = draftPath;
  // Переименование выполняется только ПОСЛЕ безопасной установки полного нового CSV. Если удаление старого файла
  // не удалось, сохраняются обе копии и выводится сообщение; данные не отбрасываются.
  bool oldRemoved = true;
  if (!saveAs && target != oldPath)
    oldRemoved = LittleFS.remove(oldPath);
  draftProgram = *next;
  draftPath = target;
  draftSourceHash = editorFileHash(target, exists);
  draftDirty = false;
  ++draftRevision;
  if (selectedProgramPath == oldPath || saveAs) {
    selectedProgramPath = target;
    prefs.putString("selectedPath", target);
  }
  programsDirty = true;
  if (!programRunning)
    refreshPrograms(true);
  String json = editorSnapshot();
  json.remove(json.length() - 1);
  json +=
      ",\"warning\":" +
      editorJsonQuote(oldRemoved ? String()
                                 : String("Новый файл сохранён, но прежний удалить не удалось: ") +
                                       oldPath) +
      "}";
  server.send(200, "application/json; charset=utf-8", json);
}

void handleEditorDiscard() {
  if (!editorCheckRevision())
    return;
  String path = draftPath;
  draftLoaded = false;
  draftDirty = false;
  String error;
  if (!openProgramDraft(path, error)) {
    editorError(404, error);
    return;
  }
  server.send(200, "application/json; charset=utf-8", editorSnapshot());
}

// Скачать проверенный черновик из ОЗУ вместе с несохранёнными правками, не изменяя CSV.
void handleEditorDownload() {
  if (!editorCheckRevision())
    return;
  String filename = String(draftProgram.name) + ".csv";
  server.sendHeader("Content-Disposition", "attachment; filename=program.csv; filename*=UTF-8''" +
                                               urlEncodePath(filename));
  server.send(200, "text/csv; charset=utf-8", serializeProgramCsv(draftProgram).c_str());
}
