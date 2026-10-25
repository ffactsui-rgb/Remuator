#pragma once

// Included after the controller globals and clock helpers.
bool ensureRunLog() {
  if (!LittleFS.exists("/Logs") && !LittleFS.mkdir("/Logs")) {
    logError = "Не удалось создать папку /Logs";
    return false;
  }
  if (!runMeta.logPath[0]) {
    do {
      snprintf(runMeta.logPath, sizeof(runMeta.logPath), "/Logs/Remuage_%lld_%08lx.log",
               (long long)runMeta.startedEpoch, (unsigned long)esp_random());
    } while (LittleFS.exists(runMeta.logPath));
  }
  return true;
}

bool appendRunLog(const char *event, String details, time_t epoch = 0, uint16_t stepNumber = 0) {
  if (!ensureRunLog())
    return false;
  details.replace("\n", " ");
  details.replace("\r", " ");
  File log = LittleFS.open(runMeta.logPath, "a");
  if (!log) {
    logError = "Не удалось открыть журнал /Logs";
    return false;
  }
  if (!epoch && timeValid())
    epoch = nowEpoch();
  String line =
      (epoch > 1700000000 ? dateTimeString(epoch) : String("Время не установлено")) + " | " +
      event + " | Шаг " + String(stepNumber ? stepNumber : currentProgramStep + 1) +
      " | Y=" + String(motor_Naklon.getCurrent()) + " Z=" + String(motor_Povorot.getCurrent()) +
      " | Наклон=" + String(execState.naklonCompleted ? 1 : 0) +
      " Поворот=" + String(execState.povorotCompleted ? 1 : 0) + " | " + details + "\n";
  bool ok = log.write((const uint8_t *)line.c_str(), line.length()) == line.length();
  log.flush();
  log.close();
  logError = ok ? "" : "Не удалось дописать журнал: проверьте место в LittleFS";
  return ok;
}

String tiltRangeWarning(const RemuageProgram &program) {
  if (!programExceedsTiltRange(program))
    return "";
  return "В CSV есть Y выше 85°. Файл допущен; наклон выполняется до верхнего концевика "
         "GPIO11 (85°), не более 108410 шагов поиска.";
}
