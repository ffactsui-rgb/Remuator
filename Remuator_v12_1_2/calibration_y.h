#pragma once
// Remuator 11.5: ADXL345 tilt calibration. Included after shared runtime objects.
// No external ADXL library required; shared Wire bus with OLED and RTC.
#include <math.h>
#include <stdint.h>
#include <atomic>
#ifdef ARDUINO
#include <freertos/queue.h>
#include <freertos/task.h>
#endif

constexpr uint8_t CAL_ADXL_ADDR = 0x53; // ADXL345 SDO -> GND, CS -> 3.3 V
constexpr uint8_t CAL_MAX_POINTS = 96;
constexpr uint8_t CAL_MAX_RAW = 100;
constexpr uint32_t CAL_MAGIC = 0x59313135UL; // "Y115"
constexpr float CAL_RAD2DEG = 57.29577951308232f;

struct CalPoint {
  int16_t angle;     // real physical tilt degrees, 1-degree interval
  int32_t steps;     // full STEP pulses from LOWER limit (0)
};
struct CalBlob {
  uint32_t magic = CAL_MAGIC;
  uint16_t schema = 1;
  uint8_t n = 0;
  uint8_t yAxis = 1;
  uint8_t xAxis = 0;
  int16_t correctedMin = 23;
  uint32_t crc = 0;
  CalPoint rows[CAL_MAX_POINTS] = {};
};
struct CalRaw {
  float rawAngle = 0;
  int32_t fromUpper = 0;
};
enum CalPhase : uint8_t { CAL_IDLE, CAL_SEEK_85, CAL_START_DOWN, CAL_SEEK_23, CAL_REVIEW, CAL_ERROR };
CalBlob calActive, calProposal;
CalRaw calRaw[CAL_MAX_RAW];
uint8_t calRawCount = 0;
volatile CalPhase calPhase = CAL_IDLE;
String calMessage = "";
bool calAdxlPresent = false, calXStored = false, calTableValid = false;
uint8_t calYAxis = 1, calXAxis = 0;
float calRefX = 0, calToleranceX = 2.0f;
float calG[3] = {0, 0, 0};
float calAngle = NAN, calCurrentX = NAN, calStartAngle = NAN, calLowerAngle = NAN;
float calLastRaw = NAN;
int32_t calLastStep = 0, calUpperCoord = 0, calTotalSteps = 0;
int calNextDegree = 0;
uint32_t calLastPollMs = 0, calLastOkMs = 0, calUpperSettlingMs = 0;
uint64_t calStartedUs = 0;
// ADXL I2C works on core 0 during fast motion. STEP generation on core 1
// must never wait for I2C (a read may take milliseconds on a long CAT5e).
bool calReadRaw(float out[3]);
bool calibrationBusy();
struct CalSensorFrame { float gravity[3]; int32_t motorStep; uint32_t atMs; uint8_t ok; };
#ifdef ARDUINO
QueueHandle_t calSensorQueue = nullptr;
SemaphoreHandle_t calI2cLock = nullptr;
#endif
std::atomic<int32_t> calMotorStepSnapshot{0};
int32_t calLatestSampleStep = 0;
#ifdef ARDUINO
void calSensorWorker(void *) {
  for (;;) {
    if (calSensorQueue && calibrationBusy()) {
      CalSensorFrame f={};
      if (calI2cLock && xSemaphoreTake(calI2cLock,pdMS_TO_TICKS(8))==pdTRUE) {
        f.ok=calReadRaw(f.gravity);
        xSemaphoreGive(calI2cLock);
      }
      f.motorStep=calMotorStepSnapshot.load(std::memory_order_relaxed);
      f.atMs=millis();
      if(xQueueSend(calSensorQueue,&f,0)!=pdTRUE){
        CalSensorFrame discard;
        xQueueReceive(calSensorQueue,&discard,0);
        xQueueSend(calSensorQueue,&f,0);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
#endif

bool calibrationBusy() { return calPhase == CAL_SEEK_85 || calPhase == CAL_START_DOWN || calPhase == CAL_SEEK_23; }
bool calibrationValid() { return calTableValid && calActive.n > 1; }
int32_t calibratedUpperSteps() { return calibrationValid() ? calActive.rows[calActive.n - 1].steps : Y_HOME_TRAVEL_STEPS; }

uint32_t calHash(const CalBlob &c) {
  uint32_t v = 2166136261UL;
  auto add = [&v](uint8_t byte) { v = (v ^ byte) * 16777619UL; };
  add(c.schema); add(c.n); add(c.yAxis); add(c.xAxis);
  add((uint8_t)c.correctedMin); add((uint8_t)(c.correctedMin >> 8));
  for (uint8_t i = 0; i < c.n && i < CAL_MAX_POINTS; ++i) {
    const uint32_t step = (uint32_t)c.rows[i].steps;
    add((uint8_t)c.rows[i].angle); add((uint8_t)(c.rows[i].angle >> 8));
    for (int k = 0; k < 4; ++k) add((uint8_t)(step >> (k * 8)));
  }
  return v;
}
bool calValidate(const CalBlob &c) {
  if (c.magic != CAL_MAGIC || c.schema != 1 || c.n < 2 || c.n > CAL_MAX_POINTS ||
      c.yAxis > 2 || c.xAxis > 2 || c.yAxis == c.xAxis || c.crc != calHash(c)) return false;
  if (c.rows[0].steps < 0 || c.rows[c.n - 1].angle != 85 || c.rows[c.n - 1].steps < 1000) return false;
  for (uint8_t i = 1; i < c.n; ++i)
    if (c.rows[i].angle != c.rows[i-1].angle + 1 || c.rows[i].steps <= c.rows[i-1].steps) return false;
  return true;
}

bool calWriteReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(CAL_ADXL_ADDR); Wire.write(reg); Wire.write(value);
  return Wire.endTransmission() == 0;
}
bool calReadRaw(float out[3]) {
  Wire.beginTransmission(CAL_ADXL_ADDR); Wire.write((uint8_t)0x32);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(CAL_ADXL_ADDR, (uint8_t)6) != 6) return false;
  for (uint8_t i=0; i<3; ++i) {
    uint8_t lo=Wire.read(), hi=Wire.read();
    int16_t raw=(int16_t)((uint16_t)lo | ((uint16_t)hi << 8));
    out[i] = raw * 0.00390625f; // ADXL345 full resolution: ~3.9 mg/LSB
  }
  return true;
}
float calMagnitude() {
  return sqrtf(calG[0]*calG[0]+calG[1]*calG[1]+calG[2]*calG[2]);
}
float calMeasureY() {
  uint8_t third = 3 - calYAxis - calXAxis;
  return atan2f(fabsf(calG[calYAxis]), fabsf(calG[third])) * CAL_RAD2DEG;
}
float calMeasureX() {
  const float g=calMagnitude();
  return g > 0.2f ? asinf(constrain(calG[calXAxis]/g, -1.0f, 1.0f)) * CAL_RAD2DEG : NAN;
}
bool calSample(bool immediate = false) {
  uint32_t t=millis();
  float raw[3];
  int32_t frameStep=calMotorStepSnapshot.load(std::memory_order_relaxed);
#ifdef ARDUINO
  if(calibrationBusy() && calSensorQueue) {
    CalSensorFrame sample;
    if(xQueueReceive(calSensorQueue,&sample,0)!=pdTRUE) return false;
    t=sample.atMs;
    if(!sample.ok) { calAdxlPresent=false; return false; }
    memcpy(raw,sample.gravity,sizeof(raw));
    frameStep=sample.motorStep;
  } else {
#else
  {
    frameStep=motor_Naklon.getCurrent();
#endif
    if(!immediate && (uint32_t)(t-calLastPollMs)<200 && !calibrationBusy()) return false;
#ifdef ARDUINO
    if (calI2cLock && xSemaphoreTake(calI2cLock,pdMS_TO_TICKS(25))!=pdTRUE) return false;
#endif
    bool ok=calReadRaw(raw);
#ifdef ARDUINO
    if(calI2cLock) xSemaphoreGive(calI2cLock);
#endif
    if(!ok) {calAdxlPresent=false;return false;}
  }
  calLatestSampleStep=frameStep;
  calLastPollMs=t;
  float mag=sqrtf(raw[0]*raw[0]+raw[1]*raw[1]+raw[2]*raw[2]);
  if (mag<0.65f || mag>1.40f) return false;
  const float alpha=calLastOkMs?0.25f:1.0f;
  for(int i=0;i<3;i++) calG[i]+=alpha*(raw[i]-calG[i]);
  calAngle=calMeasureY();calCurrentX=calMeasureX();
  calAdxlPresent=true;calLastOkMs=t;
  return true;
}
bool calXWithinTolerance() {
  return calXStored && calAdxlPresent && calLastOkMs && (uint32_t)(millis()-calLastOkMs) < 3000 &&
         isfinite(calCurrentX) && fabsf(calCurrentX-calRefX) <= calToleranceX;
}
String calibrationRunReason() {
  if (calibrationBusy()) return "Калибровка Y ещё выполняется";
  if (calPhase == CAL_REVIEW) return "Проверьте и сохраните таблицу новой калибровки Y";
  if (calPhase == CAL_ERROR) return "Ошибка калибровки Y: повторите её или отмените для старой таблицы";
  if (!calibrationValid()) return "Сначала выполните калибровку Y: Настройки → Калибровка Y";
  if (!calAdxlPresent || (uint32_t)(millis()-calLastOkMs) > 3000) return "ADXL345 не отвечает; проверьте I2C";
  if (!calXStored) return "Сначала установите горизонт и нажмите «Запомнить X»";
  if (!calXWithinTolerance()) return "Горизонт X изменился: выровняйте ножками или перенастройте эталон X";
  return "";
}
bool calibrationRunReady() { return calibrationRunReason().isEmpty(); }

int32_t calibratedAngleToSteps(float angle) {
  if (!calibrationValid()) return 0;
  if (angle <= calActive.rows[0].angle) return calActive.rows[0].steps;
  for (uint8_t i=1;i<calActive.n;++i)
    if (angle <= calActive.rows[i].angle) {
      float f=(angle-calActive.rows[i-1].angle)/(float)(calActive.rows[i].angle-calActive.rows[i-1].angle);
      return lroundf(calActive.rows[i-1].steps+f*(calActive.rows[i].steps-calActive.rows[i-1].steps));
    }
  return calibratedUpperSteps();
}
float calibratedStepsToAngle(int32_t steps) {
  if (!calibrationValid()) return 23.0f;
  if (steps<=calActive.rows[0].steps) return calActive.rows[0].angle;
  for(uint8_t i=1;i<calActive.n;++i)
    if (steps<=calActive.rows[i].steps) {
      float f=(steps-calActive.rows[i-1].steps)/(float)(calActive.rows[i].steps-calActive.rows[i-1].steps);
      return calActive.rows[i-1].angle+f;
    }
  return 85.0f;
}

bool calLoadFile(CalBlob &dst) {
  File f=LittleFS.open("/calibration_y.csv","r");
  if(!f) return false;
  // Header: CAL_Y_V1,yaxis,xaxis,min,n,crc; following lines angle,steps.
  String h=f.readStringUntil('\n'); h.trim();
  unsigned y=0,x=0,n=0,crc=0; int mn=0;
  if(sscanf(h.c_str(),"CAL_Y_V1,%u,%u,%d,%u,%u",&y,&x,&mn,&n,&crc)!=5 || n>CAL_MAX_POINTS) { f.close(); return false; }
  CalBlob b={}; b.magic=CAL_MAGIC; b.schema=1; b.yAxis=y; b.xAxis=x; b.correctedMin=mn; b.n=n; b.crc=crc;
  for (unsigned i=0;i<n;i++) {
    String line=f.readStringUntil('\n');
    int ang=0; long step=0;
    if(sscanf(line.c_str(),"%d,%ld",&ang,&step)!=2) {f.close();return false;}
    b.rows[i] = {(int16_t)ang,(int32_t)step};
  }
  f.close(); if(!calValidate(b)) return false;
  dst=b; return true;
}
// The X reference has its own checked, atomically replaced backup.
bool calSaveXFile() {
  File f=LittleFS.open("/calibration_x.tmp","w");
  if(!f) return false;
  f.printf("CAL_X_V1,%u,%u,%.6f,%.6f,%u\n",(unsigned)calYAxis,(unsigned)calXAxis,
      (double)calRefX,(double)calToleranceX,calXStored?1u:0u);
  f.flush();f.close();
  File check=LittleFS.open("/calibration_x.tmp","r");
  String line=check.readStringUntil('\n');check.close();
  unsigned y,x,on;float ref,tol;
  if(sscanf(line.c_str(),"CAL_X_V1,%u,%u,%f,%f,%u",&y,&x,&ref,&tol,&on)!=5 ||
     y!=calYAxis || x!=calXAxis || on!=(calXStored?1u:0u) ||
     !isfinite(ref) || fabsf(ref-calRefX)>0.00001f || fabsf(tol-calToleranceX)>0.00001f) return false;
  return LittleFS.rename("/calibration_x.tmp","/calibration_x.csv");
}
bool calLoadXFile() {
  File f=LittleFS.open("/calibration_x.csv","r");if(!f)return false;
  String line=f.readStringUntil('\n');f.close();
  unsigned y,x,on;float ref,tol;
  if(sscanf(line.c_str(),"CAL_X_V1,%u,%u,%f,%f,%u",&y,&x,&ref,&tol,&on)!=5 ||
     y>2 || x>2 || x==y || on>1 || !isfinite(ref) || fabsf(ref)>180 ||
     !isfinite(tol) || tol<0.2f || tol>15) return false;
  calYAxis=y;calXAxis=x;calRefX=ref;calToleranceX=tol;calXStored=on;
  return true;
}
void calibrationInit() {
  calYAxis=(uint8_t)constrain(prefs.getUChar("calYaxis",1),0,2);
  calXAxis=(uint8_t)constrain(prefs.getUChar("calXaxis",0),0,2);
  if(calYAxis==calXAxis) {calYAxis=1; calXAxis=0;}
  calToleranceX=constrain(prefs.getFloat("calXtol",2.0f),0.2f,15.0f);
  calXStored=prefs.getBool("calXset",false);
  calRefX=prefs.getFloat("calXref",0.0f);
  calLoadXFile(); // Restore valid saved axes/reference without repeating calibration.
  calTableValid=false;
  if(prefs.getBytesLength("yCalBlob")==sizeof(CalBlob) &&
     prefs.getBytes("yCalBlob",&calActive,sizeof(CalBlob))==sizeof(CalBlob) && calValidate(calActive))
    calTableValid=true;
  else if(calLoadFile(calActive)) {
    calTableValid=true; prefs.putBytes("yCalBlob",&calActive,sizeof(CalBlob));
  }
  if(calTableValid && (calActive.yAxis!=calYAxis || calActive.xAxis!=calXAxis)) calTableValid=false;
  if(calXStored && !LittleFS.exists("/calibration_x.csv")) calSaveXFile();
  Wire.beginTransmission(CAL_ADXL_ADDR);
  if(Wire.endTransmission()==0 && calWriteReg(0x31,0x08) && // FULL_RES, +/-2g
     calWriteReg(0x2C,0x09) && // 50 Hz output data rate
     calWriteReg(0x2D,0x08)) { // measurement mode
    delay(25); calSample(true);
  }
  #ifdef ARDUINO
  calI2cLock=xSemaphoreCreateMutex();
  calSensorQueue=xQueueCreate(64,sizeof(CalSensorFrame));
  if(calSensorQueue && calI2cLock) {
    const BaseType_t otherCore=(xPortGetCoreID()==0)?1:0;
    BaseType_t created=xTaskCreatePinnedToCore(calSensorWorker,"ADXL345",4096,nullptr,1,nullptr,otherCore);
    if(created!=pdPASS) {vQueueDelete(calSensorQueue);calSensorQueue=nullptr;}
  }
  if(!calSensorQueue) calMessage="Ошибка создания фонового считывания ADXL345";
  #endif
  if(!calAdxlPresent) calMessage="ADXL345 0x53 не найден";
  else if(!calibrationValid()) calMessage="Требуется первичная калибровка Y";
  else if(!calXWithinTolerance()) calMessage="Проверьте горизонт X по уровню";
}

void calStopWithError(const String& message) {
  motor_Naklon.brake();
  motor_Naklon.setTarget(motor_Naklon.getCurrent());
  motorNaklonMoving=false; serialMoveNaklonActive=false;
  motor_Naklon.setMaxSpeed(NAKLON_MAX_SPEED);
  disableDrivers();
  calPhase=CAL_ERROR; calMessage=message;
}
void calRecord(float a,int32_t step) {
  if(calRawCount<CAL_MAX_RAW) calRaw[calRawCount++]={a,step};
}
void calibrationOnUpper() {
  if(calPhase==CAL_SEEK_85) {
    calPhase=CAL_START_DOWN;
    calStartedUs=(uint64_t)esp_timer_get_time();
    calUpperSettlingMs=millis(); // allow low-pass filter to settle at fixed upper endpoint
    calMessage="Концевик 85° найден; подготовка прохода к 23°";
  }
}
void calibrationOnLower(int32_t coordinateBeforeReset) {
  if(calPhase!=CAL_SEEK_23) return;
  calTotalSteps=calUpperCoord-coordinateBeforeReset;
  // Captured lower endpoint at physical NC switch, never beyond it.
  if(isfinite(calAngle) && calTotalSteps>0) calRecord(calAngle,calTotalSteps);
  calLowerAngle=calAngle;
  motor_Naklon.setMaxSpeed(NAKLON_MAX_SPEED);
  calPhase=CAL_REVIEW;
  calMessage=(calTotalSteps>1000 && calRawCount>25) ?
    "Проход завершён. Проверьте таблицу и уточните минимальный угол" :
    "Недостаточно измерений, повторите калибровку";
}
void calMoveFromRest(int32_t target) {
  motor_Naklon.brake();
  motor_Naklon.setTarget(motor_Naklon.getCurrent());
  // GyverStepper 2.7.x retains the previous STEP period and direction after
  // brake(). With acceleration enabled setTarget() does not initialize them.
  // Prime a long first period through the public low-speed API, then restore
  // the requested limit. The planner runs before any STEP and sets direction
  // and acceleration from rest, instead of pulsing into the pressed switch.
  motor_Naklon.setMaxSpeed(1);
  motor_Naklon.setMaxSpeed(NAKLON_MAX_SPEED);
  motor_Naklon.setTarget(target);
  motorNaklonMoving=true;
  enableDrivers();
}
void calibrationService() {
  bool moving=motor_Naklon.getState() || motor_Povorot.getState();
  if(calibrationBusy() && (calPhase==CAL_SEEK_85 || calPhase==CAL_SEEK_23) &&
     !motor_Naklon.getState() && motor_Naklon.getCurrent()==motor_Naklon.getTarget()) {
    calStopWithError(calPhase==CAL_SEEK_85 ? "Концевик 85° не найден: проверьте проводку и механику" :
      "Концевик 23° не найден: проверьте проводку и механику");return;
  }
  if(!moving || calibrationBusy()) {
    bool got=calSample();
    if(calibrationBusy() && (uint32_t)(millis()-calLastOkMs)>1200) {
      calStopWithError("Потеряны измерения ADXL345 при движении; привод остановлен"); return;
    }
    if(calibrationBusy() && (uint64_t)esp_timer_get_time()-calStartedUs > (uint64_t)std::min<uint32_t>(MAX_ACTION_TIME_SEC,Y_LIMIT_SEARCH_TIMEOUT_SEC)*1000000ULL) {
      calStopWithError("Таймаут поиска концевика/калибровки"); return;
    }
    if(calPhase==CAL_START_DOWN) {
      if((uint32_t)(millis()-calUpperSettlingMs)<350U) return;
      if(!calAdxlPresent || !isfinite(calAngle) || !calXWithinTolerance()) {
        calStopWithError("Измерение угла или горизонт X вне допуска"); return;
      }
      calRawCount=0; calStartAngle=calAngle; calLastRaw=calAngle;
      calUpperCoord=motor_Naklon.getCurrent(); calLastStep=0;
      calNextDegree=(int)floorf(calAngle);
      calRecord(calAngle,0);
      if(digitalRead(Y_END_PIN)==HIGH) { calStopWithError("Нижний концевик разомкнут перед ходом"); return; }
      calPhase=CAL_SEEK_23;
      calStartedUs=(uint64_t)esp_timer_get_time();
      calMoveFromRest(calUpperCoord-Y_ENDSTOP_SEARCH_STEPS);
      calMessage="Измерение: движение к концевику 23°";
    } else if(calPhase==CAL_SEEK_23 && got) {
      int32_t steps=calUpperCoord-calLatestSampleStep;
      if(steps<calLastStep) { calStopWithError("Некорректный счёт шагов Y"); return; }
      if(calAngle > calStartAngle+8.0f && steps>2000) {
        calStopWithError("Неверно выбрана ось Y или изменилось направление наклона"); return;
      }
      // Per-degree crossing interpolated between two sensor samples.
      while(calNextDegree>0 && calAngle<=calNextDegree &&
            calLastRaw>calNextDegree && calRawCount<CAL_MAX_RAW) {
        float f=(calLastRaw-calNextDegree)/fmaxf(0.001f,calLastRaw-calAngle);
        calRecord((float)calNextDegree,calLastStep+lroundf((steps-calLastStep)*f));
        --calNextDegree;
      }
      // Monotonic envelope: vibration may temporarily increase angle.
      if(calAngle < calLastRaw) {calLastRaw=calAngle; calLastStep=steps;}
    }
  }
}
bool calibrationBegin(String &why) {
  if(programRunning || homingActive || motorNaklonMoving || motorPovorotMoving ||
     motor_Naklon.getState() || motor_Povorot.getState() || calibrationBusy()) {
    why="Сначала остановите программу и двигатели"; return false;
  }
  calSample(true);
  if(!calAdxlPresent) {why="ADXL345 не отвечает по I2C";return false;}
  if(!calXStored || !calXWithinTolerance()) {why="Установите горизонт и нажмите «Запомнить X» или выровняйте ножки";return false;}
  if(digitalRead(Y_END_PIN)==HIGH && digitalRead(Y_UPPER_END_PIN)==HIGH) {why="Оба концевика Y разомкнуты";return false;}
  calRawCount=0; calStartAngle=NAN; calLowerAngle=NAN; calTotalSteps=0;
  calPhase=CAL_SEEK_85; calStartedUs=(uint64_t)esp_timer_get_time();
  motor_Naklon.setMaxSpeed(NAKLON_MAX_SPEED); // Максимальная установленная скорость.
#ifdef ARDUINO
  if (!calSensorQueue) {why="Фоновая задача ADXL345 недоступна"; calPhase=CAL_ERROR;return false;}
  xQueueReset(calSensorQueue);
#endif
  calMessage="Поиск концевика 85°";
  if(digitalRead(Y_UPPER_END_PIN)==HIGH) {
    motor_Naklon.setCurrent(calibratedUpperSteps());
    motor_Naklon.setTarget(motor_Naklon.getCurrent());
    calibrationOnUpper();
  } else {
    calMoveFromRest(motor_Naklon.getCurrent()+Y_ENDSTOP_SEARCH_STEPS);
  }
  return true;
}

// Build corrected 1-degree map using measured nonlinear motor/angle relation.
// Both mechanical endpoints are anchors: lower = manually corrected, upper = 85°.
bool calibrationBuild(int correctedMin, String &why) {
  if(calPhase!=CAL_REVIEW || calRawCount<25 || calTotalSteps<1000) {why="Нет полного прохода между двумя концевиками";return false;}
  if(correctedMin<0 || correctedMin>40) {why="Минимальный угол должен быть от 0° до 40°";return false;}
  float lo=calLowerAngle, hi=calStartAngle, measured=hi-lo;
  if(!isfinite(measured) || measured<40 || measured>80 || (85-correctedMin)/measured>1.30f ||
     (85-correctedMin)/measured<0.70f) {why="Диапазон измерений не соответствует наклону от нижнего до верхнего концевика";return false;}
  CalBlob b={}; b.magic=CAL_MAGIC; b.schema=1; b.yAxis=calYAxis; b.xAxis=calXAxis; b.correctedMin=correctedMin;
  for(int deg=correctedMin;deg<=85 && b.n<CAL_MAX_POINTS;deg++) {
    float rawTarget=lo + (deg-correctedMin)*measured/(85-correctedMin);
    float fromUpper=0;
    if(deg==correctedMin) fromUpper=(float)calTotalSteps;
    else if(deg==85) fromUpper=0;
    else {
      bool found=false;
      for(int i=1;i<calRawCount;i++) {
        // Raw samples descend in angle while fromUpper grows.
        const CalRaw &a=calRaw[i-1], &z=calRaw[i];
        if(a.rawAngle>=rawTarget && z.rawAngle<=rawTarget && a.rawAngle>z.rawAngle) {
          float f=(a.rawAngle-rawTarget)/(a.rawAngle-z.rawAngle);
          fromUpper=a.fromUpper+(z.fromUpper-a.fromUpper)*f;
          found=true;break;
        }
      }
      if(!found) {why="Пропущено измерение угла; снизьте скорость калибровки";return false;}
    }
    int32_t steps=constrain((int32_t)lroundf(calTotalSteps-fromUpper),0,calTotalSteps);
    if(b.n>0 && steps<=b.rows[b.n-1].steps) {why="Шаги не возрастают с углом: проверьте ось Y";return false;}
    b.rows[b.n++]={(int16_t)deg,steps};
  }
  b.crc=calHash(b);
  if(!calValidate(b)) {why="Таблица калибровки не прошла проверку";return false;}
  calProposal=b; return true;
}
bool calibrationSave(String&why) {
  if(!calValidate(calProposal) || calibrationBusy() || motor_Naklon.getState() || motor_Povorot.getState()) {
    why="Некорректная калибровка или двигатель ещё движется"; return false;
  }
  File f=LittleFS.open("/calibration_y.tmp","w");
  if(!f) {why="Не удалось создать /calibration_y.tmp"; return false;}
  f.printf("CAL_Y_V1,%u,%u,%d,%u,%u\n",(unsigned)calProposal.yAxis,(unsigned)calProposal.xAxis,
           (int)calProposal.correctedMin,(unsigned)calProposal.n,(unsigned)calProposal.crc);
  for(uint8_t i=0;i<calProposal.n;i++) f.printf("%d,%ld\n",(int)calProposal.rows[i].angle,(long)calProposal.rows[i].steps);
  f.flush(); f.close();
  CalBlob verify;
  // File is atomically renamed after complete write; NVS is redundant backup.
  if(!LittleFS.rename("/calibration_y.tmp","/calibration_y.csv") || !calLoadFile(verify)) {
    why="Не удалось проверить файл калибровки";return false;
  }
  if(prefs.putBytes("yCalBlob",&calProposal,sizeof(calProposal))!=sizeof(calProposal)) {
    why="Файл сохранён, но запись NVS не удалась";return false;
  }
  calActive=calProposal;calTableValid=true;calPhase=CAL_IDLE;
  motor_Naklon.setCurrent(0);motor_Naklon.setTarget(0);
  calMessage="Калибровка сохранена в NVS и /calibration_y.csv";
  return true;
}
