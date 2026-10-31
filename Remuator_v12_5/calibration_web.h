#pragma once
// REST и страница калибровки Y. Подключается после web_api.h.
void handleApiVersion() {
  server.send(200,"application/json; charset=utf-8",String("{\"version\":")+editorJsonQuote(REMUATOR_VERSION)+"}");
}
void handleCalibrationPage() {sendLittleFSFile("/calibration-y.html");}
String calPhaseName() {
  switch(calPhase) {
    case CAL_SEEK_85: return "seek85";
    case CAL_START_DOWN: return "prepare";
    case CAL_SEEK_23: return "seek23";
    case CAL_REVIEW: return "review";
    case CAL_ERROR: return "error";
    default: return "idle";
  }
}
void handleCalibrationStatus() {
  // Длинные таблицы формируются только после остановки, чтобы не задерживать STEP.
  const bool busy = calibrationBusy();
  String json="{\"ok\":true,\"version\":"+editorJsonQuote(REMUATOR_VERSION)+
    ",\"phase\":"+editorJsonQuote(calPhaseName())+
    ",\"message\":"+editorJsonQuote(calMessage)+
    ",\"present\":"+jsonBool(calAdxlPresent)+
    ",\"valid\":"+jsonBool(calibrationValid())+
    ",\"runReady\":"+jsonBool(calibrationRunReady())+
    ",\"runReason\":"+editorJsonQuote(calibrationRunReason())+
    ",\"yAxis\":"+String(calYAxis)+",\"xAxis\":"+String(calXAxis)+
    ",\"xStored\":"+jsonBool(calXStored)+",\"xTolerance\":"+String(calToleranceX,1)+
    ",\"xRunTolerance\":"+String(calRunToleranceX,1)+
    ",\"xRunOk\":"+jsonBool(calXWithinRunTolerance())+
    ",\"xReference\":"+String(calRefX,3)+
    ",\"xDeviation\":"+String(calXStored && isfinite(calCurrentX)?fabsf(calCurrentX-calRefX):-1,3)+
    ",\"xOk\":"+jsonBool(calXWithinTolerance())+
    ",\"accel\":["+String(calG[0],4)+","+String(calG[1],4)+","+String(calG[2],4)+"]"+
    ",\"angleY\":"+(isfinite(calAngle)?String(calAngle,1):String("null"))+
    ",\"angleX\":"+(isfinite(calCurrentX)?String(calCurrentX,1):String("null"))+
    ",\"measuredMin\":"+(isfinite(calLowerAngle)?String(calLowerAngle,2):String("null"))+
    ",\"measuredMax\":"+(isfinite(calStartAngle)?String(calStartAngle,2):String("null"))+
    ",\"rawCount\":"+String(calRawCount)+",\"totalSteps\":"+String(calTotalSteps)+
    ",\"lowerLimit\":"+jsonBool(digitalRead(Y_END_PIN)==HIGH)+
    ",\"upperLimit\":"+jsonBool(digitalRead(Y_UPPER_END_PIN)==HIGH)+
    ",\"raw\":[";
  if(!busy) for(int i=0;i<calRawCount;++i) {
    if(i)json+=',';
    json+="{\"angle\":"+String(calRaw[i].rawAngle,2)+",\"stepsFrom85\":"+
       String(calRaw[i].fromUpper)+",\"stepsFrom23\":"+String(calTotalSteps-calRaw[i].fromUpper)+"}";
  }
  json+="],\"saved\":[";
  if(!busy && calibrationValid()) for(int i=0;i<calActive.n;++i) {
    if(i)json+=',';
    json+="{\"angle\":"+String(calActive.rows[i].angle)+",\"steps\":"+String(calActive.rows[i].steps)+"}";
  }
  json+="]}";
  server.sendHeader("Cache-Control","no-store");
  server.send(200,"application/json; charset=utf-8",json);
}
void handleCalibrationAction() {
  String action=server.arg("action");
  if (zCalStarted) {
    server.send(409,"text/plain; charset=utf-8","Сначала завершите калибровку Z");
    return;
  }
  if(action=="cancel") {
    if(calibrationBusy()) {
      calStopWithError("Калибровка остановлена оператором");
      server.send(200,"application/json","{\"ok\":true}");return;
    }
    calPhase=CAL_IDLE;
    server.send(200,"application/json","{\"ok\":true}");return;
  }
  if(programRunning || motor_Povorot.getState() || homingActive ||
     (motor_Naklon.getState() && !calibrationBusy())) {
    server.send(409,"text/plain; charset=utf-8","Сначала остановите все двигатели и программу");return;
  }
  if(action=="axes") {
    if(calibrationBusy()) {server.send(409,"text/plain","CALIBRATION_BUSY");return;}
    int y=server.arg("yAxis").toInt(), x=server.arg("xAxis").toInt();
    if(y<0 || y>2 || x<0 || x>2 || x==y) {
      server.send(422,"text/plain; charset=utf-8","Оси X и Y должны быть разными");return;
    }
    float tol=server.arg("xTolerance").toFloat();
    float run=server.hasArg("xRunTolerance") ? server.arg("xRunTolerance").toFloat() : calRunToleranceX;
    if(!isfinite(tol) || tol<0.2f || tol>15.0f || !isfinite(run) || run<tol || run>89.0f) {
      server.send(422,"text/plain; charset=utf-8","X до запуска: 0,2..15°; при работе: от допуска запуска до 89°");return;
    }
    String why;
    if(!calSetXSettings(y,x,tol,run,why)) {server.send(503,"text/plain; charset=utf-8",why);return;}
  } else if(action=="rememberX") {
    if(calibrationBusy() || motor_Naklon.getState()) {
      server.send(409,"text/plain; charset=utf-8","Не меняйте эталон X при движении");return;
    }
    // Нажатие кнопки явно подтверждает физическое выравнивание оператором.
    if(!calSample(true) || !isfinite(calCurrentX)) {
      server.send(503,"text/plain; charset=utf-8","ADXL345 не отвечает");return;
    }
    const float oldRef=calRefX;const bool oldStored=calXStored;
    calRefX=calCurrentX;calXStored=true;
    if(!calSaveXFile()) {calRefX=oldRef;calXStored=oldStored;server.send(503,"text/plain; charset=utf-8","Не удалось сохранить файл X; прежний эталон оставлен");return;}
    const bool refSaved=prefs.putFloat("calXref",calRefX)==sizeof(float);
    const bool flagSaved=prefs.putBool("calXset",true)==sizeof(bool);
    if(!refSaved || !flagSaved) {server.send(503,"text/plain; charset=utf-8","Эталон применён и записан в файл, но резерв NVS не сохранён");return;}
    calMessage="Эталон горизонтали X сохранён в NVS и /calibration_x.csv";
  } else if(action=="use") {
    if(calibrationBusy()) {server.send(409,"text/plain","CALIBRATION_BUSY");return;}
    CalBlob stored;
    if(!calLoadFile(stored) || stored.yAxis!=calYAxis || stored.xAxis!=calXAxis) {
      server.send(422,"text/plain; charset=utf-8","Файл калибровки Y отсутствует, повреждён или не соответствует выбранным осям");return;
    }
    calActive=stored;calTableValid=true;calPhase=CAL_IDLE;
    prefs.putBytes("yCalBlob",&calActive,sizeof(calActive));
    calMessage="Используется сохранённая калибровка. Проверьте горизонт X и положение механики.";
  } else if(action=="start") {
    String why;
    if(!calibrationBegin(why)) {server.send(409,"text/plain; charset=utf-8",why);return;}
  } else if(action=="save") {
    String why;
    int32_t mn=0;
    if(!programInteger(server.arg("minAngle").c_str(),mn,0,40) ||
       !calibrationBuild(mn,why) || !calibrationSave(why)) {
      server.send(422,"text/plain; charset=utf-8",why.isEmpty()?String("Неверный минимальный угол"):why);return;
    }
  } else {
    server.send(400,"text/plain","UNKNOWN_ACTION");return;
  }
  server.send(200,"application/json","{\"ok\":true}");
}
