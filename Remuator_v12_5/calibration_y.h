#pragma once
// Калибровка наклона ADXL345 для Remuator 11.5. Подключается после общих объектов среды выполнения.
// Сторонняя библиотека ADXL не нужна; Wire используется совместно с OLED и часами.
#include <math.h>
#include <stdint.h>
#include <atomic>
#include "accel_filter.h"
#ifdef ARDUINO
#include <freertos/queue.h>
#include <freertos/task.h>
#endif

constexpr uint8_t CAL_ADXL_ADDR = 0x53; // Подключение ADXL345: SDO к GND, CS к 3,3 В.
constexpr uint8_t CAL_MAX_POINTS = 96;
constexpr uint8_t CAL_MAX_RAW = 100;
constexpr uint32_t CAL_MAGIC = 0x59313135UL; // "Y115"
constexpr float CAL_RAD2DEG = 57.29577951308232f;

struct CalPoint {
  int16_t angle;     // Фактический угол наклона в градусах, с интервалом 1°.
  int32_t steps;     // Полное число импульсов STEP от НИЖНЕГО концевика, принятого за ноль.
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
float calRefX = 0, calToleranceX = 3.0f, calRunToleranceX = 20.0f;
float calG[3] = {0, 0, 0};
float calAngle = NAN, calCurrentX = NAN, calStartAngle = NAN, calLowerAngle = NAN;
float calLastRaw = NAN;
AccelFilter3 calSensorFilter;
std::atomic<bool> calSensorBackground{false};
uint32_t calLastRepairMs=0;
int32_t calLastStep = 0, calUpperCoord = 0, calTotalSteps = 0;
int calNextDegree = 0;
uint32_t calLastPollMs = 0, calLastOkMs = 0, calUpperSettlingMs = 0;
uint64_t calStartedUs = 0;
// Во время быстрого движения ADXL считывается по I2C на другом ядре. Генерация STEP
// не должна ждать I2C: чтение на длинном кабеле CAT5e может занимать миллисекунды.
bool calReadRaw(float out[3]);
bool calibrationBusy();
bool calRepairSensor();
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
    if (calSensorQueue && (calibrationBusy() || programTiltSamplingActive.load()) && calSensorBackground.load()) {
      CalSensorFrame f={};
      if (calI2cLock && xSemaphoreTake(calI2cLock,pdMS_TO_TICKS(8))==pdTRUE) {
        f.ok=calReadRaw(f.gravity);
        if(!f.ok && uint32_t(millis()-calLastRepairMs)>=1000) {
          calLastRepairMs=millis();calRepairSensor();
        }
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
bool calRepairSensor() {
  // После просадки питания ADXL может отвечать по I2C, оставаясь в режиме ожидания.
  return calWriteReg(0x2D,0x00) && calWriteReg(0x31,0x08) &&
    calWriteReg(0x2C,0x09) && calWriteReg(0x2D,0x08);
}
bool calReadRaw(float out[3]) {
  Wire.beginTransmission(CAL_ADXL_ADDR); Wire.write((uint8_t)0x32);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(CAL_ADXL_ADDR, (uint8_t)6) != 6) return false;
  for (uint8_t i=0; i<3; ++i) {
    uint8_t lo=Wire.read(), hi=Wire.read();
    int16_t raw=(int16_t)((uint16_t)lo | ((uint16_t)hi << 8));
    out[i] = raw * 0.00390625f; // Полное разрешение ADXL345: примерно 3,9 мг на младший разряд.
  }
  const float magnitude=sqrtf(out[0]*out[0]+out[1]*out[1]+out[2]*out[2]);
  return isfinite(magnitude) && magnitude>=0.65f && magnitude<=1.40f;
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
  if((calibrationBusy() || programTiltSamplingActive.load()) && calSensorBackground.load() && calSensorQueue) {
    CalSensorFrame sample;
    if(xQueueReceive(calSensorQueue,&sample,0)!=pdTRUE) return false;
    // Для программы берём самый новый кадр; при калибровке нужны все точки пути.
    if(!calibrationBusy()) {CalSensorFrame newer;while(xQueueReceive(calSensorQueue,&newer,0)==pdTRUE) sample=newer;}
    t=sample.atMs;
    if(!sample.ok) {if(uint32_t(millis()-calLastOkMs)>=1200) calAdxlPresent=false;return false;}
    memcpy(raw,sample.gravity,sizeof(raw));
    frameStep=sample.motorStep;
  } else {
#else
  {
    frameStep=motor_Naklon.getCurrent();
#endif
    if(!immediate && (uint32_t)(t-calLastPollMs)<(programRunning?20u:200u) && !calibrationBusy()) return false;
#ifdef ARDUINO
    if (calI2cLock && xSemaphoreTake(calI2cLock,pdMS_TO_TICKS(25))!=pdTRUE) return false;
#endif
    calLastPollMs=t;
    bool ok=calReadRaw(raw);
    if(!ok && uint32_t(t-calLastRepairMs)>=1000) {
      calLastRepairMs=t;
#ifdef ARDUINO
      // Сброс зависшей общей I2C выполняется только при остановленных двигателях,
      // под мьютексом: задача STEP и работающие OLED/RTC не прерываются.
      if(!calibrationBusy() && !calSensorBackground.load()) {
        Wire.end();Wire.begin(I2C_SDA_PIN,I2C_SCL_PIN);Wire.setClock(100000);Wire.setTimeOut(20);
      }
#endif
      calRepairSensor();
    }
#ifdef ARDUINO
    if(calI2cLock) xSemaphoreGive(calI2cLock);
#endif
    if(!ok) {if(uint32_t(t-calLastOkMs)>=1200) calAdxlPresent=false;return false;}
  }
  calLatestSampleStep=frameStep;
  calLastPollMs=t;
  float mag=sqrtf(raw[0]*raw[0]+raw[1]*raw[1]+raw[2]*raw[2]);
  if (mag<0.65f || mag>1.40f) return false;
  if(!calSensorFilter.update(raw,calG,t,calibrationBusy() || programRunning || motor_Naklon.getState())) return false;
  // Эти же отфильтрованные значения используются в калибровке, контроле X и восстановлении Y.
  calAngle=sensorAngleTenth(calMeasureY());calCurrentX=sensorAngleTenth(calMeasureX());
  if(!isfinite(calAngle) || !isfinite(calCurrentX)) return false;
  calAdxlPresent=true;calLastOkMs=t;
  return true;
}
bool calXFresh() {
  return calAdxlPresent && calLastOkMs && uint32_t(millis()-calLastOkMs)<1200 && isfinite(calCurrentX);
}
bool calXWithinTolerance() {
  return calXStored && calXFresh() && fabsf(calCurrentX-calRefX)<=calToleranceX+0.0001f;
}
bool calXWithinRunTolerance() {
  return calXStored && calXFresh() && fabsf(calCurrentX-calRefX)<=calRunToleranceX+0.0001f;
}
String calibrationRunReason() {
  if (calibrationBusy()) return "Калибровка Y ещё выполняется";
  if (calPhase == CAL_REVIEW) return "Проверьте и сохраните таблицу новой калибровки Y";
  if (calPhase == CAL_ERROR) return "Ошибка калибровки Y: повторите её или отмените для старой таблицы";
  if (!calibrationValid()) return "Сначала выполните калибровку Y: Настройки → Калибровка Y";
  if (!calXStored) return "Сначала установите горизонт и нажмите «Запомнить X»";
  if(!programRunning && !calXFresh()) return "Перед запуском нужны свежие показания X: проверьте ADXL345";
  if(!programRunning && !calXWithinTolerance()) return "X вне допуска выравнивания перед запуском: выровняйте корпус";
  if(programRunning && calXFresh() && !calXWithinRunTolerance()) return "X вне рабочего допуска: защита от опрокидывания";
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
  // Заголовок CAL_Y_V1: оси Y/X, минимум, число точек, CRC; далее строки угол,шаги.
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
// Эталон X имеет отдельную проверяемую резервную копию с атомарной заменой.
bool calSaveXFile() {
  File f=LittleFS.open("/calibration_x.tmp","w");
  if(!f) return false;
  f.printf("CAL_X_V2,%u,%u,%.6f,%.6f,%.6f,%u\n",(unsigned)calYAxis,(unsigned)calXAxis,
      (double)calRefX,(double)calToleranceX,(double)calRunToleranceX,calXStored?1u:0u);
  f.flush();f.close();
  File check=LittleFS.open("/calibration_x.tmp","r");
  String line=check.readStringUntil('\n');check.close();
  unsigned y,x,on;float ref,tol,run;
  if(sscanf(line.c_str(),"CAL_X_V2,%u,%u,%f,%f,%f,%u",&y,&x,&ref,&tol,&run,&on)!=6 ||
     y!=calYAxis || x!=calXAxis || on!=(calXStored?1u:0u) ||
     !isfinite(ref) || !isfinite(tol) || !isfinite(run) || fabsf(ref-calRefX)>0.00001f ||
     fabsf(tol-calToleranceX)>0.00001f || fabsf(run-calRunToleranceX)>0.00001f) return false;
  return LittleFS.rename("/calibration_x.tmp","/calibration_x.csv");
}
bool calSetXSettings(uint8_t y,uint8_t x,float start,float run,String &why) {
  if(y>2 || x>2 || y==x || !isfinite(start) || start<0.2f || start>15 ||
     !isfinite(run) || run<start || run>89) {why="Некорректные оси или допуски X";return false;}
  const uint8_t oldY=calYAxis,oldX=calXAxis;
  const float oldStart=calToleranceX,oldRun=calRunToleranceX;
  const bool oldStored=calXStored,changed=y!=oldY || x!=oldX;
  calYAxis=y;calXAxis=x;calToleranceX=start;calRunToleranceX=run;
  if(changed) calXStored=false;
  // Сначала атомарная файловая запись. Ошибка не меняет действующие допуски/оси.
  if(!calSaveXFile()) {
    calYAxis=oldY;calXAxis=oldX;calToleranceX=oldStart;calRunToleranceX=oldRun;calXStored=oldStored;
    why="Не удалось сохранить файл X; прежние настройки оставлены";return false;
  }
  if(changed) {
    calTableValid=false;calRawCount=0;calPhase=CAL_IDLE;calSensorFilter.reset();
    calMessage="Выбраны новые оси: требуется запомнить X и повторить калибровку Y";
  }
  const bool ySaved=prefs.putUChar("calYaxis",y)==sizeof(uint8_t);
  const bool xSaved=prefs.putUChar("calXaxis",x)==sizeof(uint8_t);
  const bool startSaved=prefs.putFloat("calXtol",start)==sizeof(float);
  const bool runSaved=prefs.putFloat("calXrun",run)==sizeof(float);
  const bool referenceSaved=!changed || prefs.putBool("calXset",false)==sizeof(bool);
  if(!ySaved || !xSaved || !startSaved || !runSaved || !referenceSaved) {
    why="Файл X сохранён и настройки применены, но резерв NVS записать не удалось";return false;
  }
  return true;
}
bool calLoadXFile() {
  File f=LittleFS.open("/calibration_x.csv","r");if(!f)return false;
  String line=f.readStringUntil('\n');f.close();
  unsigned y,x,on;float ref,tol,run=calRunToleranceX;
  if(sscanf(line.c_str(),"CAL_X_V2,%u,%u,%f,%f,%f,%u",&y,&x,&ref,&tol,&run,&on)!=6 &&
     sscanf(line.c_str(),"CAL_X_V1,%u,%u,%f,%f,%u",&y,&x,&ref,&tol,&on)!=5) return false;
  if(y>2 || x>2 || x==y || on>1 || !isfinite(ref) || fabsf(ref)>180 ||
     !isfinite(tol) || tol<0.2f || tol>15 || !isfinite(run) || run<tol || run>89) return false;
  calYAxis=y;calXAxis=x;calRefX=ref;calToleranceX=tol;calRunToleranceX=run;calXStored=on;
  return true;
}
void calibrationInit() {
  calYAxis=(uint8_t)constrain(prefs.getUChar("calYaxis",1),0,2);
  calXAxis=(uint8_t)constrain(prefs.getUChar("calXaxis",0),0,2);
  if(calYAxis==calXAxis) {calYAxis=1; calXAxis=0;}
  calToleranceX=constrain(prefs.getFloat("calXtol",3.0f),0.2f,15.0f);
  calRunToleranceX=constrain(prefs.getFloat("calXrun",20.0f),calToleranceX,89.0f);
  calXStored=prefs.getBool("calXset",false);
  calRefX=prefs.getFloat("calXref",0.0f);
  calLoadXFile(); // Восстановить сохранённые оси и эталон без повторной калибровки.
  calTableValid=false;
  if(prefs.getBytesLength("yCalBlob")==sizeof(CalBlob) &&
     prefs.getBytes("yCalBlob",&calActive,sizeof(CalBlob))==sizeof(CalBlob) && calValidate(calActive))
    calTableValid=true;
  else if(calLoadFile(calActive)) {
    calTableValid=true; prefs.putBytes("yCalBlob",&calActive,sizeof(CalBlob));
  }
  if(calTableValid && (calActive.yAxis!=calYAxis || calActive.xAxis!=calXAxis)) calTableValid=false;
  if(calXStored && !LittleFS.exists("/calibration_x.csv")) calSaveXFile();
#ifdef ARDUINO
  Wire.setTimeOut(20);
#endif
  Wire.beginTransmission(CAL_ADXL_ADDR);
  if(Wire.endTransmission()==0 && calWriteReg(0x31,0x08) && // Полное разрешение FULL_RES, диапазон ±2g.
     calWriteReg(0x2C,0x09) && // Частота выдачи измерений 50 Гц.
     calWriteReg(0x2D,0x08)) { // Режим измерения.
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
    calUpperSettlingMs=millis(); // Выдержка для установления фильтра в неподвижной верхней точке.
    calMessage="Концевик 85° найден; подготовка прохода к 23°";
  }
}
void calibrationOnLower(int32_t coordinateBeforeReset) {
  if(calPhase!=CAL_SEEK_23) return;
  calTotalSteps=calUpperCoord-coordinateBeforeReset;
  // Нижняя точка фиксируется на физическом NC-концевике, без выхода за него.
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
  // GyverStepper 2.7.x сохраняет период STEP и направление после
  // brake(). При включённом ускорении setTarget() не задаёт их заново немедленно.
  // Через общедоступный режим малой скорости задаём длинный начальный период, затем возвращаем
  // нужный предел скорости. До первого STEP планировщик выставит направление
  // и разгон с места, поэтому импульсы не пойдут в сторону нажатого концевика.
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
  if(!moving || calibrationBusy() || programTiltSamplingActive.load()) {
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
      // Пересечение каждого градуса интерполируется между двумя измерениями датчика.
      while(calNextDegree>0 && calAngle<=calNextDegree &&
            calLastRaw>calNextDegree && calRawCount<CAL_MAX_RAW) {
        float f=(calLastRaw-calNextDegree)/fmaxf(0.001f,calLastRaw-calAngle);
        calRecord((float)calNextDegree,calLastStep+lroundf((steps-calLastStep)*f));
        --calNextDegree;
      }
      // Монотонная огибающая: вибрация может временно увеличить измеренный угол.
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

// Скорректированная таблица с шагом 1° строится по измеренной нелинейной зависимости шагов от угла.
// Оба механических предела служат опорными точками: нижний уточняется вручную, верхний равен 85°.
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
        // Угол исходных измерений уменьшается, а число шагов fromUpper увеличивается.
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
  // После полной записи файл атомарно переименовывается; NVS служит резервной копией.
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
