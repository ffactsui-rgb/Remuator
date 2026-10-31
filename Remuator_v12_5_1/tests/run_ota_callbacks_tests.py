"""Настоящие обработчики ArduinoOTA: тайм-аут, Wi-Fi, ошибки, прогресс и диагностика разделов."""
from pathlib import Path
import os,re,subprocess,tempfile
root=Path(__file__).resolve().parent.parent
source=next(root.glob('*.ino')).read_text()
def function(name):
 match=re.search(r'^(?:void|String|const char \*) '+name+r'\([^\n]*\) \{',source,re.M)
 if not match:match=re.search(r'^const char \*'+name+r'\([^\n]*\) \{',source,re.M)
 pos=match.end()-1;depth=0;quote=None;escaped=False
 while pos<len(source):
  c=source[pos]
  if quote:
   if escaped:escaped=False
   elif c=='\\':escaped=True
   elif c==quote:quote=None
  elif source[pos:pos+2]=='//':pos=source.index('\n',pos);continue
  elif c in "\"'":quote=c
  elif c=='{':depth+=1
  elif c=='}':
   depth-=1
   if not depth:return source[match.start():pos+1]
  pos+=1
 raise AssertionError(name)
code=r'''
#include <atomic>
#include <cassert>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <type_traits>
#include <vector>
struct String:std::string {using std::string::string;String(const std::string&s):std::string(s){}template<class T,typename std::enable_if<std::is_arithmetic<T>::value,int>::type=0>String(T n):std::string(std::to_string(n)){}};
#define REMUATOR_OTA_PASSWORD "test-ota-password"
std::vector<String>trace;
#define DBGLN(x) trace.emplace_back(x)
enum {ESP_RST_POWERON=1,ESP_RST_SW,ESP_RST_PANIC,ESP_RST_INT_WDT,ESP_RST_TASK_WDT,ESP_RST_WDT,ESP_RST_BROWNOUT};
int resetReason=ESP_RST_POWERON;int esp_reset_reason(){return resetReason;}
enum wifi_ps_type_t {WIFI_PS_NONE,WIFI_PS_MIN_MODEM,WIFI_PS_MAX_MODEM};
struct Wifi {wifi_ps_type_t sleep=WIFI_PS_MAX_MODEM;wifi_ps_type_t getSleep(){return sleep;}bool setSleep(bool on){sleep=on?WIFI_PS_MIN_MODEM:WIFI_PS_NONE;return true;}bool setSleep(wifi_ps_type_t v){sleep=v;return true;}}WiFi;
constexpr int ESP_PARTITION_TYPE_APP=0,ESP_PARTITION_SUBTYPE_APP_OTA_0=0,ESP_PARTITION_SUBTYPE_APP_OTA_1=1,U_SPIFFS=100;
struct esp_partition_t {uint32_t size;const char*label;};esp_partition_t slot0{4194304,"app0"},slot1{4194304,"app1"};bool slotsPresent=true;
const esp_partition_t*esp_partition_find_first(int,int sub,const char*){return slotsPresent?(sub?&slot1:&slot0):nullptr;}
const esp_partition_t*esp_ota_get_running_partition(){return &slot0;}
const esp_partition_t*esp_ota_get_next_update_partition(void*){return slotsPresent?&slot1:nullptr;}
enum ota_error_t {OTA_AUTH_ERROR,OTA_BEGIN_ERROR,OTA_CONNECT_ERROR,OTA_RECEIVE_ERROR,OTA_END_ERROR};
struct Ota {int timeout=0,command=0;bool begun=false;std::function<void()>start,end;std::function<void(unsigned,unsigned)>progress;std::function<void(ota_error_t)>error;
 void setTimeout(int n){timeout=n;}void setHostname(const char*){}void setPassword(const char*){}
 void onStart(std::function<void()>f){start=f;}void onEnd(std::function<void()>f){end=f;}void onProgress(std::function<void(unsigned,unsigned)>f){progress=f;}void onError(std::function<void(ota_error_t)>f){error=f;}int getCommand(){return command;}void begin(){begun=true;}}ArduinoOTA;
struct Updater {int error=0;int getError(){return error;}const char*errorString(){return "test update error";}}Update;
struct Fs {int ended=0;void end(){++ended;}}LittleFS;
unsigned mounts=0;bool drivers=true;void disableDrivers(){drivers=false;}bool mountProgramFS(){++mounts;return true;}
std::atomic<bool>otaServiceEnabled{false},otaInProgress{false},otaTransferComplete{false};
std::atomic<uint32_t>otaTransferred{0},otaTotal{0};std::atomic<int>otaLastError{-1},otaFlashError{0};
wifi_ps_type_t otaSavedSleep=WIFI_PS_NONE;bool otaSleepChanged=false;
'''
code+='\n'.join(function(n) for n in ['otaResetReasonText','otaStatusJson','setupArduinoOTA'])
code+=r'''
int main(){
 setupArduinoOTA();assert(otaServiceEnabled&&ArduinoOTA.begun&&ArduinoOTA.timeout==5000);
 assert(otaStatusJson().find("\"nextSlotBytes\":4194304")!=String::npos);
 ArduinoOTA.start();assert(otaInProgress&&!drivers&&WiFi.sleep==WIFI_PS_NONE);
 ArduinoOTA.progress(1234,9999);assert(otaTransferred==1234&&otaTotal==9999);
 assert(otaStatusJson().find("\"received\":1234")!=String::npos);
 Update.error=4;ArduinoOTA.error(OTA_RECEIVE_ERROR);assert(!otaInProgress&&otaLastError==3&&otaFlashError==4);
 assert(WiFi.sleep==WIFI_PS_MAX_MODEM&&!otaSleepChanged&&mounts==0);
 ArduinoOTA.command=U_SPIFFS;ArduinoOTA.start();assert(LittleFS.ended==1);ArduinoOTA.error(OTA_END_ERROR);assert(mounts==1&&!otaInProgress);
 ArduinoOTA.error(OTA_AUTH_ERROR);assert(mounts==1&&otaLastError==0);
 ArduinoOTA.command=0;ArduinoOTA.start();assert(otaLastError==-1&&otaTransferred==0);
 ArduinoOTA.end();assert(otaTransferComplete&&otaInProgress);
 resetReason=ESP_RST_TASK_WDT;assert(otaStatusJson().find("task_watchdog")!=String::npos);
 resetReason=ESP_RST_BROWNOUT;assert(otaStatusJson().find("brownout")!=String::npos);
 slotsPresent=false;otaServiceEnabled=false;ArduinoOTA.begun=false;setupArduinoOTA();assert(!otaServiceEnabled&&!ArduinoOTA.begun);
 std::cout<<"PASS: actual OTA callbacks, timeout, Wi-Fi sleep restoration, progress/errors, FS remount only after start, reset reason and real slot diagnostics\n";
}
'''
with tempfile.TemporaryDirectory(prefix='ota-callbacks-') as work:
 p=Path(work);(p/'test.cpp').write_text(code)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'},check=True)
