#pragma once
// Ручная калибровка полного оборота Z. Основная запись находится в NVS, CSV — читаемая копия.
// Во время выдачи STEP строго запрещены записи и обмен с TMC по UART.
void zCalibrationLoad() {
  zCalSteps360 = (int32_t)prefs.getInt("zCalSteps", DEFAULT_Z_STEPS_360);
  zCalMicrosteps = prefs.getUShort("zCalMicro", 0);
  zCalStored = prefs.getBool("zCalValid", false);
  zCalSavedHash = prefs.getUInt("zCalHash",0);
  if (!zCalibrationValid()) zCalStored = false;
  zCalStarted = false;
  zCalJogging = false;
  zCalStepSnapshot.store(motor_Povorot.getCurrent(), std::memory_order_relaxed);
}
void zCalibrationStopMotion() {
  motor_Povorot.brake();
  motor_Povorot.setTarget(motor_Povorot.getCurrent());
  zCalJogging = false;
  zCalDirection = 0;
  motorPovorotMoving = false;
  serialMovePovorotActive = false;
  releaseDrivers();
  zCalStepSnapshot.store(motor_Povorot.getCurrent(), std::memory_order_relaxed);
}
void zCalibrationService() {
  if (!zCalJogging) {
    // После подводки на ±1/±10 STEP отпускаем EN, не сбрасывая счётчик Z.
    if (motorPovorotMoving && !motor_Povorot.getState() &&
        motor_Povorot.getCurrent() == motor_Povorot.getTarget()) {
      motorPovorotMoving=false;
      releaseDrivers();
    }
    // Незавершённая подводка также останавливается по тому же тайм-ауту.
    if (motorPovorotMoving && (uint32_t)(millis()-zCalJogAt)>Z_CAL_DEADMAN_MS)
      zCalibrationStopMotion();
    return;
  }
  const int32_t position = motor_Povorot.getCurrent();
  if ((uint32_t)(millis() - zCalHeartbeat) > Z_CAL_DEADMAN_MS ||
      (uint32_t)(millis() - zCalJogAt) > 120000UL ||
      abs(position) >= Z_CAL_MAX_ABS_STEPS) {
    zCalibrationStopMotion();
  }
}
void handleZCalibrationPage() { sendLittleFSFile("/calibration-z.html"); }
void handleZCalibrationStatus() {
  // Только быстро получаемые значения: во время STEP не читать UART, I2C, NVS и LittleFS.
  String json = "{\"ok\":true,\"micro\":" + String(uartPovorot.microsteps) +
      ",\"steps360\":" + String(zCalibrationValid() ? zCalSteps360 : 0) +
      ",\"valid\":" + (zCalibrationValid() ? "true" : "false") +
      ",\"started\":" + (zCalStarted ? "true" : "false") +
      ",\"jogging\":" + (zCalJogging ? "true" : "false") +
      ",\"steps\":" + String(zCalStepSnapshot.load(std::memory_order_relaxed)) +
      ",\"programRunning\":" + (programRunning ? "true" : "false") + "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200,"application/json; charset=utf-8",json);
}
void handleZCalibrationAction() {
  String action = server.arg("action");
  if (action == "cancel") {
    if (programRunning) {server.send(409,"text/plain","PROGRAM_RUNNING");return;}
    if (zCalStarted) zCalibrationStopMotion();
    zCalStarted=false;
    server.send(200,"application/json","{\"ok\":true}");
    return;
  }
  if (action == "stop") {
    if (zCalStarted && !programRunning) zCalibrationStopMotion();
    server.send(200,"application/json","{\"ok\":true}");
    return;
  }
  if (action == "heartbeat") {
    if (zCalStarted && zCalJogging && !programRunning) zCalHeartbeat = millis();
    server.send(200,"application/json","{\"ok\":true}");
    return;
  }
  if (programRunning || calibrationBusy() || homingActive || motorNaklonMoving ||
      motor_Naklon.getState() || serialMoveNaklonActive ||
      (motor_Povorot.getState() && !zCalJogging)) {
    server.send(409,"text/plain; charset=utf-8","Остановите программу, калибровку Y и другие движения");
    return;
  }
  if (action == "micro") {
    if (zCalJogging || motor_Povorot.getState()) {
      server.send(409,"text/plain; charset=utf-8","Остановите двигатель Z"); return;
    }
    int32_t micro;
    if (!programInteger(server.arg("micro").c_str(),micro,1,256) || (micro & (micro-1))) {
      server.send(422,"text/plain; charset=utf-8","Микрошаг должен быть степенью двойки от 1 до 256");return;
    }
    if ((uint16_t)micro != uartPovorot.microsteps) {
      TmcUartSettings next = uartPovorot;
      next.microsteps = (uint16_t)micro;
      disableDrivers();
      if (!applyTmcSettingsVerified(tmcPovorot,next)) {
        applyTmcSettingsVerified(tmcPovorot,uartPovorot);
        server.send(503,"text/plain; charset=utf-8","Драйвер Z не подтвердил микрошаг по UART. Старый микрошаг сохранён.");
        return;
      }
      if(!persistTmcFile(uartNaklon,next,NAKLON_ACCELERATION,NAKLON_MAX_SPEED,POVOROT_ACCELERATION,POVOROT_MAX_SPEED)) {
        applyTmcSettingsVerified(tmcPovorot,uartPovorot);releaseDrivers();
        server.send(507,"text/plain; charset=utf-8","Не удалось сохранить файл UART; микрошаг не изменён");return;
      }
      uartPovorot = next;
      saveTmcSettings(next,"tp");
      zCalibrationInvalidate();
      zCalStarted=false;
      motor_Povorot.setCurrent(0);
      motor_Povorot.setTarget(0);
      zCalStepSnapshot.store(0,std::memory_order_relaxed);
    }
  } else if (action == "start") {
    if (zCalJogging || motor_Povorot.getState()) {
      server.send(409,"text/plain; charset=utf-8","Остановите двигатель Z");return;
    }
    zCalStarted = true;
    motor_Povorot.setCurrent(0);
    motor_Povorot.setTarget(0);
    zCalStepSnapshot.store(0,std::memory_order_relaxed);
    // Предыдущая таблица остаётся в памяти, но при сохранении будет заменена.
    // Перед обновлением не отменяем старую проверенную калибровку.
  } else if (action == "left" || action == "right") {
    if (zCalJogging || motorPovorotMoving) {
      server.send(409,"text/plain; charset=utf-8","Сначала остановите текущее движение");return;
    }
    if (!zCalStarted) {
      server.send(409,"text/plain; charset=utf-8","Сначала нажмите «Запуск» для фиксации нуля");return;
    }
    if (abs(motor_Povorot.getCurrent()) >= Z_CAL_MAX_ABS_STEPS - 1000) {
      server.send(409,"text/plain; charset=utf-8","Предельное число шагов калибровки; нажмите Стоп");return;
    }
    zCalDirection = action == "right" ? 1 : -1;
    zCalHeartbeat = zCalJogAt = millis();
    zCalJogging = true;
    motorPovorotMoving = true;
    enableDrivers();
    motor_Povorot.setTarget(motor_Povorot.getCurrent() + zCalDirection * (Z_CAL_MAX_ABS_STEPS - abs(motor_Povorot.getCurrent())));
  } else if (action == "nudge") {
    if (!zCalStarted || zCalJogging || motorPovorotMoving || motor_Povorot.getState() ||
        motor_Povorot.getCurrent() != motor_Povorot.getTarget()) {
      server.send(409,"text/plain; charset=utf-8","Остановите двигатель и нажмите Запуск");return;
    }
    int32_t count=0;
    if (!programInteger(server.arg("steps").c_str(),count,-10,10) ||
        (count != -10 && count != -1 && count != 1 && count != 10)) {
      server.send(422,"text/plain; charset=utf-8","Разрешены только ±1 и ±10 STEP");return;
    }
    const int32_t target=motor_Povorot.getCurrent()+count;
    if (abs(target)>Z_CAL_MAX_ABS_STEPS) {
      server.send(409,"text/plain; charset=utf-8","Достигнут предел калибровки");return;
    }
    motorPovorotMoving=true;
    enableDrivers();
    motor_Povorot.setTarget(target);
    zCalHeartbeat=zCalJogAt=millis();
  } else if (action == "save") {
    if (!zCalStarted || zCalJogging || motorPovorotMoving || motor_Povorot.getState() ||
        motor_Povorot.getCurrent() != motor_Povorot.getTarget()) {
      server.send(409,"text/plain; charset=utf-8","Нажмите Стоп и дождитесь остановки для сохранения");return;
    }
    const int32_t count = abs(motor_Povorot.getCurrent());
    if (count < 100 || count > Z_CAL_MAX_ABS_STEPS) {
      server.send(422,"text/plain; charset=utf-8","Нужно пройти полный оборот 360° (не менее 100 STEP)");return;
    }
    // Сначала подготавливается читаемый файл. При ошибке прежняя калибровка NVS сохраняется.
    File f = LittleFS.open("/calibration_z.csv.tmp","w");
    if (!f) {server.send(507,"text/plain; charset=utf-8","Не удалось открыть файл калибровки Z");return;}
    f.println("microsteps,stepsPer360");
    f.print(uartPovorot.microsteps); f.print(','); f.println(count);
    f.flush();
    bool fileOk = f.size() > 20;
    f.close();
    if (!fileOk) {
      LittleFS.remove("/calibration_z.csv.tmp");
      server.send(507,"text/plain; charset=utf-8","Ошибка записи файла калибровки Z");return;
    }
    if (LittleFS.exists("/calibration_z.csv")) LittleFS.remove("/calibration_z.csv");
    if (!LittleFS.rename("/calibration_z.csv.tmp","/calibration_z.csv")) {
      server.send(507,"text/plain; charset=utf-8","Ошибка переименования файла калибровки Z");return;
    }
    // Признак достоверности записывается ПОСЛЕДНИМ; частичная запись NVS не принимается.
    const uint32_t digest=zCalibrationDigest(count,uartPovorot.microsteps);
    prefs.putBool("zCalValid",false);
    if (prefs.putInt("zCalSteps", count) != sizeof(int32_t) ||
        prefs.putUShort("zCalMicro",uartPovorot.microsteps) != sizeof(uint16_t) ||
        prefs.putUInt("zCalHash",digest) != sizeof(uint32_t) ||
        prefs.putBool("zCalValid",true) != 1 ||
        prefs.getInt("zCalSteps",0) != count ||
        prefs.getUShort("zCalMicro",0) != uartPovorot.microsteps ||
        prefs.getUInt("zCalHash",0) != digest) {
      zCalibrationInvalidate();
      server.send(507,"text/plain; charset=utf-8","Ошибка подтверждения записи калибровки Z в NVS");return;
    }
    zCalSteps360 = count;
    zCalMicrosteps = uartPovorot.microsteps;
    zCalSavedHash = digest;
    zCalStored = true;
    zCalStarted = false;
  } else {
    server.send(400,"text/plain; charset=utf-8","Неизвестная команда калибровки Z");return;
  }
  server.send(200,"application/json; charset=utf-8","{\"ok\":true}");
}
