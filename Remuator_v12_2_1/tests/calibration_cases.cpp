#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>
using std::uint8_t;using std::int16_t; using std::int32_t; using std::uint32_t; using std::uint64_t;
struct String:std::string {
  using std::string::string;
  String()=default; String(const std::string&v):std::string(v){}
  bool isEmpty()const{return empty();}
  void trim(){size_t a=find_first_not_of(" \t\n\r");if(a==npos){clear();return;}size_t b=find_last_not_of(" \t\n\r");*this=substr(a,b-a+1);}
};
unsigned tms=0; unsigned millis(){return tms;}
uint64_t esp_timer_get_time(){return (uint64_t)tms*1000ULL;}
void delay(unsigned dt){tms+=dt;}
template<typename T,typename A,typename B>T constrain(T v,A lo,B hi){return v<lo?(T)lo:v>hi?(T)hi:v;}
struct WireMock {
 float angle=85, xG=0;uint8_t reg=0;std::vector<uint8_t> bytes;int pos=0;
 void beginTransmission(uint8_t){}void write(uint8_t v){reg=v;}
 int endTransmission(bool=true){return 0;}
 uint8_t requestFrom(uint8_t,uint8_t){
  bytes.clear();pos=0;
  float g[3]={xG,sinf(angle/57.29577951308232f),cosf(angle/57.29577951308232f)};
  for(int i=0;i<3;i++){int16_t r=lroundf(g[i]/0.00390625f);bytes.push_back(r&255);bytes.push_back(((uint16_t)r)>>8);}
  return 6;
 }
 int read(){return bytes.at(pos++);}
} Wire;
struct FakePrefs {
 std::map<std::string,std::vector<uint8_t>> blob;
 uint8_t getUChar(const char*k,uint8_t d){return blob.count(k)?blob[k][0]:d;}
 bool getBool(const char*k,bool d){return blob.count(k)?blob[k][0]:d;}
 float getFloat(const char*k,float d){if(!blob.count(k))return d;float v;memcpy(&v,blob[k].data(),4);return v;}
 size_t putBool(const char*k,bool v){blob[k]={(uint8_t)v};return 1;}
 size_t putFloat(const char*k,float v){blob[k]=std::vector<uint8_t>(4);memcpy(blob[k].data(),&v,4);return 4;}
 size_t putUChar(const char*k,uint8_t v){blob[k]={v};return 1;}
 size_t getBytesLength(const char*k){return blob.count(k)?blob[k].size():0;}
 size_t getBytes(const char*k,void*p,size_t sz){if(getBytesLength(k)!=sz)return 0;memcpy(p,blob[k].data(),sz);return sz;}
 size_t putBytes(const char*k,const void*p,size_t sz){blob[k].assign((const uint8_t*)p,(const uint8_t*)p+sz);return sz;}
} prefs;
struct FakeFS;
struct File {
  std::string *buf=nullptr;unsigned off=0;
  explicit operator bool()const{return buf!=nullptr;}
  String readStringUntil(char c){if(!buf)return {};size_t k=buf->find(c,off);if(k==std::string::npos)k=buf->size();String r=buf->substr(off,k-off);off=k+1;return r;}
  int printf(const char*fmt,...){char b[1024];va_list args;va_start(args,fmt);int n=vsnprintf(b,sizeof b,fmt,args);va_end(args);if(buf&&n>0)buf->append(b,n);return n;}
  void flush(){}void close(){}
};
struct FakeFS {
 std::map<std::string,std::string> files;
 File open(const char*k,const char*mode){if(mode[0]=='r'&&!files.count(k))return {};if(mode[0]=='w')files[k].clear();return {&files[k],0};}
 bool rename(const char*a,const char*b){if(!files.count(a))return false;files[b]=files[a];files.erase(a);return true;}
 bool remove(const char*k){return files.erase(k)>0;}
 bool exists(const char*k){return files.count(k);}
} LittleFS;
struct Motor {int32_t current=0,target=0;uint32_t speed=6000;bool forcedStop=false;
  int32_t getCurrent(){return current;}int32_t getTarget(){return target;}
  void setCurrent(int32_t n){current=n;}
  void setTarget(int32_t n){target=n;forcedStop=false;}
  void setMaxSpeed(uint32_t n){speed=n;}
  void brake(){forcedStop=true;target=current;}
  bool getState(){return !forcedStop&&target!=current;}
} motor_Naklon,motor_Povorot;
void enableDrivers(){}void disableDrivers(){}
constexpr uint8_t Y_END_PIN=14,Y_UPPER_END_PIN=13;
constexpr int32_t Y_HOME_TRAVEL_STEPS=108410,Y_ENDSTOP_SEARCH_STEPS=200000000;
constexpr uint32_t Y_LIMIT_SEARCH_TIMEOUT_SEC=180;
uint32_t NAKLON_MAX_SPEED=6000,MAX_ACTION_TIME_SEC=3600;
bool programRunning=false,homingActive=false,motorNaklonMoving=false,motorPovorotMoving=false,serialMoveNaklonActive=false;
constexpr int HIGH=1,LOW=0;int lowerPin=LOW,upperPin=LOW;
int digitalRead(uint8_t p){return p==Y_END_PIN?lowerPin:upperPin;}
std::atomic<bool> programTiltSamplingActive{false};
#include "calibration_y.h"
#include "nc_endstop.h"
struct {bool upperReached=false;}runMeta;
String executionMessage;
bool recoveryAutoPending=false,recoveryMotionUncertain=false,executionReadError=false;
bool homingZPhase=false,serialMovePovorotActive=false,programPaused=false;
bool endstopYActive=false,endstopUpperActive=false,yEndstopFault=false;
int moveDirectionNaklon=0,moveDirectionPovorot=0;
NcEndstop lowerEndstop,upperEndstop;
void beginZHome(){}
void pauseProgram(){programPaused=true;}
void appendRunLog(const char*,const String&){}
void oledMarkDirty(){}
void releaseDrivers(){}
bool saveExecution(){assert(!calibrationBusy());return true;}
/* REAL_ENDSTOPS: настоящий обработчик концевиков для тестовой среды. */
int main(){
 calibrationInit();assert(calAdxlPresent);assert(!calibrationValid());assert(!calibrationRunReady());
 calXStored=true;calRefX=calCurrentX;
 String reason;
 // Интеграционная проверка: обработчик NC останавливает у верхнего концевика, выдерживает паузу и отходит от него.
 assert(calibrationBegin(reason));assert(calPhase==CAL_SEEK_85);
 tms+=170000; // Длительный первый ход не должен расходовать тайм-аут обратного хода.
 upperPin=HIGH;motor_Naklon.current=90000;
 checkEndstops();assert(calPhase==CAL_START_DOWN);assert(!motor_Naklon.getState());
 const auto settledAt=calUpperSettlingMs;
 tms+=300;checkEndstops();calibrationService();assert(calPhase==CAL_START_DOWN&&calUpperSettlingMs==settledAt);
 tms+=60;checkEndstops();calibrationService();assert(calPhase==CAL_SEEK_23);
 const auto returnTarget=motor_Naklon.getTarget();
 checkEndstops();assert(calPhase==CAL_SEEK_23&&motor_Naklon.getTarget()==returnTarget&&motor_Naklon.getState());
 tms+=11000;calibrationService();assert(calPhase==CAL_SEEK_23); // Общее время больше 180 с, но тайм-аут обратного хода ещё не истёк.
 calStopWithError("reset integration");upperPin=LOW;checkEndstops();tms+=25;checkEndstops();
 calPhase=CAL_IDLE;motorNaklonMoving=false;
 assert(calibrationBegin(reason));assert(calPhase==CAL_SEEK_85);
 motor_Naklon.current=Y_HOME_TRAVEL_STEPS; motor_Naklon.target=Y_HOME_TRAVEL_STEPS;
 calibrationOnUpper();
 tms+=400;calibrationService();assert(calPhase==CAL_SEEK_23);
 for(int i=1;i<=150;i++){
   float f=i/150.f;Wire.angle=85.f-f*62.f;
   motor_Naklon.current=Y_HOME_TRAVEL_STEPS-lroundf(f*Y_HOME_TRAVEL_STEPS);
   tms+=25;calibrationService();
 }
 lowerPin=HIGH;checkEndstops();
 assert(!motor_Naklon.getState()&&motor_Naklon.getCurrent()==0);
 lowerPin=LOW;checkEndstops();tms+=25;checkEndstops();
 assert(calPhase==CAL_REVIEW);assert(!calibrationRunReady());
 std::cout<<"raw="<<(int)calRawCount<<" upper="<<calStartAngle<<" lower="<<calLowerAngle<<" steps="<<calTotalSteps<<"\n";
 assert(calRawCount>25);
 assert(calibrationBuild(23,reason));
 assert(calibrationSave(reason));
 assert(calibrationValid());assert(calibrationRunReady());
 assert(calActive.rows[0].angle==23&&calActive.rows[0].steps==0);
 assert(calActive.rows[calActive.n-1].angle==85&&calActive.rows[calActive.n-1].steps==108410);
 assert(calibratedAngleToSteps(85)==108410);
 assert(calibratedAngleToSteps(23)==0);
 assert(calibratedStepsToAngle(0)==23);
 assert(LittleFS.files.count("/calibration_y.csv"));
 assert(calLoadFile(calProposal));

 // Сохранение и загрузка файла X и повторный запуск с корректными X/Y не требуют перекалибровки.
 assert(calSaveXFile());
 const float reference=calRefX;
 calXStored=false;calRefX=91;
 assert(calLoadXFile());assert(calXStored && fabsf(calRefX-reference)<0.00001f);
 calTableValid=false;calXStored=false;
 calibrationInit();assert(calibrationValid());assert(calXStored);assert(calibrationRunReady());
 auto goodX=LittleFS.files["/calibration_x.csv"];
 LittleFS.files["/calibration_x.csv"]="CAL_X_V1,1,1,0,2,1\n";
 assert(!calLoadXFile());
 LittleFS.files["/calibration_x.csv"]="CAL_X_V1,1,0,nan,2,1\n";
 assert(!calLoadXFile());
 LittleFS.files["/calibration_x.csv"]=goodX;
 calActive.crc^=1;assert(!calValidate(calActive));
 // Неожиданная остановка или достижение дальней условной цели завершают калибровку ошибкой.
 calPhase=CAL_SEEK_85;motor_Naklon.brake();calibrationService();assert(calPhase==CAL_ERROR);
 calPhase=CAL_SEEK_23;motor_Naklon.brake();calibrationService();assert(calPhase==CAL_ERROR);
 std::cout<<"PASS CALIBRATION HEADER UNIT TEST"<<std::endl;
}
