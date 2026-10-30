#pragma once
// JSON only. All layout, labels, styles and browser behaviour live in /data.
inline String jsonBool(bool value) { return value ? "true" : "false"; }

bool acceptBrowserClock(bool force) {
  if (timeValid() && !force)
    return true;
  if ((programRunning && !recoveryRequired) || motorNaklonMoving || motorPovorotMoving ||
      motor_Naklon.getState() || motor_Povorot.getState()) {
    server.send(409, "text/plain; charset=utf-8",
                "Установить часы можно при остановленной механике");
    return false;
  }
  String input = server.arg("epoch");
  if (input.length() < 10 || input.length() > 11) {
    server.send(422, "text/plain; charset=utf-8",
                "Нужно корректное время браузера или синхронизация NTP");
    return false;
  }
  for (size_t i = 0; i < input.length(); ++i)
    if (input[i] < '0' || input[i] > '9') {
      server.send(422, "text/plain", "INVALID_EPOCH");
      return false;
    }
  int64_t epoch = strtoll(input.c_str(), nullptr, 10);
  if (epoch < 1700000000LL || epoch > 4102444800LL) {
    server.send(422, "text/plain", "INVALID_EPOCH");
    return false;
  }
  struct timeval value = {};
  value.tv_sec = epoch;
  if (settimeofday(&value, nullptr) != 0) {
    server.send(500, "text/plain", "CLOCK_SET_FAILED");
    return false;
  }
  lastBrowserSyncEpoch = (uint32_t)epoch;
  receivedNtpEpoch.store(0);
  clockSource = 2;
  rtcWritePending = true;
  lastRtcWriteAttemptMs = 0; // сохранить UTC в DS3231 при простое механики
  applyTimezone();
  oledMarkDirty();
  return true;
}

void handleApiTime() {
  if (!acceptBrowserClock(true))
    return;
  server.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
}

void handleApiHome() {
  pollTimeSync();
  refreshPrograms();
  const uint64_t mono = (uint64_t)esp_timer_get_time();
  if (programRunning && !recoveryRequired)
    rebuildRemainingSchedule();
  String json =
      "{\"ok\":true,\"version\":" + editorJsonQuote(REMUATOR_VERSION) + ",\"refreshMs\":" +
      String(programRunning ? min(WEB_PAGE_REFRESH_PERIOD_MS, 1000UL)
                            : min(WEB_PAGE_REFRESH_PERIOD_MS, 2000UL)) +
      ",\"epoch\":" + String((long long)nowEpoch()) + ",\"timeValid\":" + jsonBool(timeValid()) +
      ",\"timezone\":" + editorJsonQuote(TZ_INFO) + ",\"running\":" + jsonBool(programRunning) +
      ",\"paused\":" + jsonBool(programPaused) +
      ",\"recoveryRequired\":" + jsonBool(recoveryRequired) +
      ",\"motionUncertain\":" + jsonBool(recoveryMotionUncertain) +
      ",\"message\":" + editorJsonQuote(executionMessage) +
      ",\"logError\":" + editorJsonQuote(logError) +
      ",\"noticeVisible\":" + jsonBool(runMeta.noticeVisible) +
      ",\"outageEpoch\":" + String((long long)runMeta.outageEpoch) +
      ",\"powerOnEpoch\":" + String((long long)runMeta.powerOnEpoch) +
      ",\"finishedEpoch\":" + String((long long)runMeta.finishedEpoch) +
      ",\"calibrationReady\":" + jsonBool(overallCalibrationReady()) +
      ",\"calibrationReason\":" + editorJsonQuote(overallCalibrationReason()) +
      ",\"logPath\":" + editorJsonQuote(runMeta.logPath) + ",\"homing\":" + jsonBool(homingActive) +
      ",\"lowerY\":" + jsonBool(endstopYActive) + ",\"upperY\":" + jsonBool(endstopUpperActive) +
      ",\"yEndstopFault\":" + jsonBool(yEndstopFault) +
      ",\"autoResume\":" + jsonBool(recoveryAutoPending) + ",\"resumeInSeconds\":" +
      String((unsigned long)(recoveryAutoPending && recoveryReadyUs > mono
                                 ? (recoveryReadyUs - mono + 999999) / 1000000
                                 : 0)) +
      ",\"warning\":" +
      editorJsonQuote(programRunning
                          ? tiltRangeWarning(runProgram)
                          : (draftLoaded && draftPath == selectedProgramPath
                                 ? tiltRangeWarning(draftProgram)
                             : programs.empty() ? String()
                                                : tiltRangeWarning(programs[selectedProgram]))) +
      ",\"selectedPath\":" + editorJsonQuote(selectedProgramPath) +
      ",\"draftDirty\":" + jsonBool(draftLoaded && draftDirty && draftPath == selectedProgramPath) +
      ",\"selectedCount\":" + String(programs.empty() ? 0 :
           (draftLoaded && draftPath == selectedProgramPath ? draftProgram.count : programs[selectedProgram].count)) +
      ",\"selectedSeconds\":" + String(programs.empty() ? 0 :
           programDurationSeconds(draftLoaded && draftPath == selectedProgramPath ? draftProgram : programs[selectedProgram])) +
      ",\"name\":" +
      editorJsonQuote(
          programRunning
              ? String(runProgram.name)
              : (draftLoaded && draftPath == selectedProgramPath
                     ? String(draftProgram.name)
                     : (programs.empty() ? String() : String(programs[selectedProgram].name)))) +
      ",\"currentStep\":" + String(currentProgramStep + 1) +
      ",\"startedAt\":" + String((long long)runMeta.startedEpoch) +
      ",\"endsAt\":" + String((long long)scheduledTimeForStep(runProgram.count)) +
      ",\"clockSource\":" +
      editorJsonQuote(clockSource == 1   ? "ntp"
                      : clockSource == 2 ? "browser"
                      : clockSource == 3 ? "rtc"
                                         : "unknown") +
      ",\"lastSyncEpoch\":" +
      String(clockSource == 1   ? lastNtpSyncEpoch
             : clockSource == 2 ? lastBrowserSyncEpoch
             : clockSource == 3 ? lastRtcConfirmedEpoch
                                : 0) +
      ",\"rtcPresent\":" + jsonBool(rtcPresent) +
      ",\"rtcValid\":" + jsonBool(rtcTimeValid) +
      ",\"ntpPending\":" + jsonBool(ntpRequested && millis() - lastNtpAttemptMs < 15000UL) +
      ",\"ntpNoResponse\":" + jsonBool(ntpRequested && millis() - lastNtpAttemptMs >= 15000UL) +
      ",\"wifiConnected\":" + jsonBool(WiFi.status() == WL_CONNECTED) +
      ",\"phase\":" + String((uint8_t)programState) +
      ",\"lastCompletedStep\":" + String(programRunning ? execState.lastCompletedStep : 0) +
      ",\"lastCompletedEpoch\":" +
      String((long long)(programRunning ? execState.lastCompletedEpoch : 0)) + ",\"nextStep\":" +
      String(programRunning && currentProgramStep + 1 < runProgram.count ? currentProgramStep + 2
                                                                         : 0) +
      ",\"actualY\":" + (calAdxlPresent && calLastOkMs && (uint32_t)(millis()-calLastOkMs)<3000 && isfinite(calAngle) ? String(calAngle,2) : String("null")) +
      ",\"targetY\":" + String(stepsToAngleNaklon(motor_Naklon.getTarget()),2) +
      ",\"poseY\":" + String(stepsToAngleNaklon(motor_Naklon.getCurrent()), 2) +
      ",\"poseRotation\":" +
      String((double)motor_Povorot.getCurrent() * 360.0 / currentZSteps360(), 2) + ",\"programs\":[";
  for (size_t i = 0; i < programs.size(); ++i) {
    if (i)
      json += ',';
    json += "{\"name\":" + editorJsonQuote(programs[i].name) +
            ",\"path\":" + editorJsonQuote(programPaths[i]) +
            ",\"count\":" + String(programs[i].count) +
            ",\"seconds\":" + String(programDurationSeconds(programs[i])) +
            ",\"warning\":" + editorJsonQuote(tiltRangeWarning(programs[i])) + "}";
  }
  json += "],\"invalid\":[";
  for (size_t i = 0; i < invalidProgramFiles.size(); ++i) {
    if (i)
      json += ',';
    json += "{\"path\":" + editorJsonQuote(invalidProgramFiles[i]) + ",\"error\":" +
            editorJsonQuote(i < invalidProgramErrors.size() ? invalidProgramErrors[i]
                                                            : String("CSV_ERROR")) +
            "}";
  }
  json += "],\"rows\":[";
  if (programRunning)
    for (uint16_t i = 0; i < runProgram.count; ++i) {
      if (i)
        json += ',';
      const ProgramStep &row = runProgram.steps[i];
      json += "{\"number\":" + String(row.number) +
              ",\"start\":" + String((long long)scheduledTimeForStep(i)) +
              ",\"skipped\":" + jsonBool(i < currentProgramStep && scheduledEpoch[i] == 0) +
              ",\"incY\":" + String(row.incAngleCenti / 100.0f, 2) +
              ",\"y\":" + String(row.angleNaklon, 2) + ",\"incRot\":" + String(row.incRot) +
              ",\"rot\":" + String(row.anglePovorot, 1) +
              ",\"orientation\":" + String(programOrientationParts(row.rotationParts) * 22.5f, 1) +
              ",\"pause\":" + editorJsonQuote(programPauseText(row.pauseSeconds).c_str()) +
              ",\"naklonDone\":" +
              jsonBool((i < currentProgramStep && scheduledEpoch[i]!=0) ||
                       (i == currentProgramStep && execState.naklonCompleted)) +
              ",\"rotationDone\":" +
              jsonBool((i < currentProgramStep && scheduledEpoch[i]!=0) ||
                       (i == currentProgramStep && execState.povorotCompleted)) +
              "}";
    }
  server.send(200, "application/json; charset=utf-8", json + "]}");
}

void handleApiSettings() {
  String json = "{\"ok\":true,\"web\":" + String(WEB_REQUEST_PERIOD_MS) +
                ",\"refresh\":" + String(WEB_PAGE_REFRESH_PERIOD_MS) +
                ",\"pn\":" + String(PAUSE_AFTER_NAKLON_MS) +
                ",\"na\":" + String(NAKLON_ACCELERATION) + ",\"ns\":" + String(NAKLON_MAX_SPEED) +
                ",\"pa\":" + String(POVOROT_ACCELERATION) + ",\"ps\":" + String(POVOROT_MAX_SPEED) +
                ",\"max\":" + String(MAX_ACTION_TIME_SEC) + ",\"tz\":" + editorJsonQuote(TZ_INFO) +
                ",\"ntpPeriodMin\":" + String(NTP_SYNC_PERIOD_SEC / 60UL) +
                ",\"ntpServer1\":" + editorJsonQuote(NTP_SERVER_1) +
                ",\"ntpServer2\":" + editorJsonQuote(NTP_SERVER_2) +
                ",\"rtcPresent\":" + jsonBool(rtcPresent) +
                ",\"rtcValid\":" + jsonBool(rtcTimeValid) +
                ",\"clockSource\":" +
                editorJsonQuote(clockSource == 1 ? "NTP" : clockSource == 2 ? "браузер" :
                                clockSource == 3 ? "DS3231" : "нет") +
                ",\"epoch\":" + String((long long)nowEpoch()) +
                ",\"localTime\":" + editorJsonQuote(dateTimeString(nowEpoch())) + "}";
  server.send(200, "application/json; charset=utf-8", json);
}

inline String tmcJson(const TmcUartSettings &c) {
  String json =
      "{\"model\":" + String((uint16_t)c.model) + ",\"address\":" + String(c.address) +
      ",\"rms\":" + String(c.rmsCurrent) + ",\"hold\":" + String(c.holdMultiplier, 2) +
      ",\"holdEnabled\":" + jsonBool(c.holdEnabled) + ",\"holdPercent\":" + String(c.holdMultiplier*100,0) +
      ",\"micro\":" + String(c.microsteps) + ",\"toff\":" + String(c.toff) +
      ",\"blank\":" + String(c.blankTime) + ",\"hend\":" + String(c.hysteresisEnd) +
      ",\"hstart\":" + String(c.hysteresisStart) + ",\"pwmo\":" + String(c.pwmOfs) +
      ",\"pwmgrad\":" + String(c.pwmGrad) + ",\"pwmfreq\":" + String(c.pwmFreq) +
      ",\"pwmreg\":" + String(c.pwmReg) + ",\"pwmlim\":" + String(c.pwmLim) +
      ",\"freewheel\":" + String(c.freewheel) + ",\"delay\":" + String(c.sendDelay) +
      ",\"stealth\":" + jsonBool(c.stealthChop) + ",\"autoscale\":" + jsonBool(c.pwmAutoscale) +
      ",\"autograd\":" + jsonBool(c.pwmAutograd) + ",\"intpol\":" + jsonBool(c.intpol) +
      ",\"iscale\":" + jsonBool(c.iScaleAnalog) + ",\"irsense\":" + jsonBool(c.internalRsense) +
      ",\"msfilter\":" + jsonBool(c.multistepFilter) + "}";
  return json;
}
void handleApiUart() {
  server.send(200, "application/json; charset=utf-8",
              "{\"ok\":true,\"version\":" + editorJsonQuote(REMUATOR_VERSION) +
                  ",\"na\":" + String(NAKLON_ACCELERATION) + ",\"ns\":" + String(NAKLON_MAX_SPEED) +
                  ",\"pa\":" + String(POVOROT_ACCELERATION) + ",\"ps\":" + String(POVOROT_MAX_SPEED) +
                  ",\"baud\":" + String(uartNaklon.baud) +
                  ",\"nak\":" + tmcJson(uartNaklon) + ",\"pov\":" + tmcJson(uartPovorot) +
                  ",\"nakConnected\":" + jsonBool(tmcNaklon.connected) + ",\"povConnected\":" +
                  jsonBool(tmcPovorot.connected) + ",\"nakVersion\":" + String(tmcNaklon.version) +
                  ",\"povVersion\":" + String(tmcPovorot.version) +
                  ",\"nakApplied\":" + jsonBool(tmcNaklon.settingsApplied) +
                  ",\"povApplied\":" + jsonBool(tmcPovorot.settingsApplied) +
                  ",\"nakWritesSent\":" + String(tmcNaklon.writesSent) +
                  ",\"povWritesSent\":" + String(tmcPovorot.writesSent) +
                  ",\"nakWritesAccepted\":" + String(tmcNaklon.writesAccepted) +
                  ",\"povWritesAccepted\":" + String(tmcPovorot.writesAccepted) + "}");
}
// Запрос запускается вручную на странице UART, только при полном простое.
// Читаем РЕАЛЬНЫЕ аппаратные регистры, не теневые значения TMCStepper.
void handleApiUartDiagnostics() {
  if (programRunning || stepMotionPending() || otaInProgress) {
    server.send(409, "text/plain; charset=utf-8",
                "Диагностика UART возможна только при остановленной программе и моторах");
    return;
  }
  disableDrivers();
  const uint32_t yIoin = tmcNaklon.readInputs();
  const bool yIoinOk = !tmcNaklon.driver().CRCerror;
  const uint8_t yIFCNT = tmcNaklon.driver().IFCNT();
  const bool yReadOk = yIoinOk && !tmcNaklon.driver().CRCerror;
  const uint32_t zIoin = tmcPovorot.readInputs();
  const bool zIoinOk = !tmcPovorot.driver().CRCerror;
  const uint8_t zIFCNT = tmcPovorot.driver().IFCNT();
  const bool zReadOk = zIoinOk && !tmcPovorot.driver().CRCerror;
  const uint8_t yVer = uint8_t(yIoin >> 24);
  const uint8_t zVer = uint8_t(zIoin >> 24);
  tmcNaklon.version = yVer;
  tmcPovorot.version = zVer;
  tmcNaklon.connected = yReadOk && yVer == (uartNaklon.model == TmcModel::Tmc2208 ? 0x20 : 0x21);
  tmcPovorot.connected = zReadOk && zVer == (uartPovorot.model == TmcModel::Tmc2208 ? 0x20 : 0x21);
  if (!tmcNaklon.connected) tmcNaklon.settingsApplied = false;
  if (!tmcPovorot.connected) tmcPovorot.settingsApplied = false;

  // Адрес 0 занят TMC2208/Y: ответ от второго драйвера с тем же адресом
  // может столкнуться в однопроводной шине. Его намеренно НЕ сканируем.
  String scan = "[";
  for (uint8_t addr = 1; addr <= 3; ++addr) {
    if (addr > 1) scan += ",";
    TMC2209Stepper probe(&TmcSerial, 0.11f, addr);
    const uint32_t probeIOIN = probe.IOIN();
    scan += String("{\"address\":") + String(addr) +
            ",\"found\":" + jsonBool(!probe.CRCerror && uint8_t(probeIOIN >> 24) == 0x21) + "}";
  }
  scan += "]";
  String json = "{\"ok\":true,\"y\":{";
  json += "\"model\":" + String((uint16_t)uartNaklon.model) +
          ",\"address\":" + String(uartNaklon.address) +
          ",\"readOk\":" + jsonBool(yReadOk) +
          ",\"version\":" + String(yVer) +
          ",\"ifcnt\":" + String(yIFCNT) +
          ",\"settingsApplied\":" + jsonBool(tmcNaklon.settingsApplied) +
          ",\"ms1\":" + String((yIoin >> 2) & 1U) +
          ",\"ms2\":" + String((yIoin >> 3) & 1U) +
          ",\"spreadEn\":" + String((yIoin >> 8) & 1U) + "},\"z\":{";
  json += "\"model\":" + String((uint16_t)uartPovorot.model) +
          ",\"address\":" + String(uartPovorot.address) +
          ",\"readOk\":" + jsonBool(zReadOk) +
          ",\"version\":" + String(zVer) +
          ",\"ifcnt\":" + String(zIFCNT) +
          ",\"settingsApplied\":" + jsonBool(tmcPovorot.settingsApplied) +
          ",\"ms1\":" + String((zIoin >> 2) & 1U) +
          ",\"ms2\":" + String((zIoin >> 3) & 1U) +
          ",\"spreadEn\":" + String((zIoin >> 8) & 1U) + "},\"scan\":" + scan + "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json; charset=utf-8", json);
}

void handleApiNetwork() {
  server.send(
      200, "application/json; charset=utf-8",
      "{\"ok\":true,\"apssid\":" + editorJsonQuote(apSSID) +
          ",\"appass\":" + editorJsonQuote(apPassword) +
          ",\"homeen\":" + jsonBool(homeWiFiEnabled) + ",\"ssid\":" + editorJsonQuote(savedSSID) +
          ",\"pass\":" + editorJsonQuote(savedPassword) +
          ",\"apIP\":" + editorJsonQuote(WiFi.softAPIP().toString()) + ",\"staIP\":" +
          editorJsonQuote(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String()) +
          ",\"timeValid\":" + jsonBool(timeValid()) +
          ",\"epoch\":" + String((long long)nowEpoch()) +
          ",\"localTime\":" + editorJsonQuote(dateTimeString(nowEpoch())) +
          ",\"timezone\":" + editorJsonQuote(TZ_INFO) + "}");
}
