"""Проверка настоящего ControlGuard: HTTP не блокирует AsyncTCP при долгом OTA, включая гонку старта."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parent.parent
source=next(root.glob('*.ino')).read_text()
guard=source[source.index('class ControlGuard {'):source.index('class AsyncCompatServer {')]
code=r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <iostream>
using TickType_t=uint32_t;
constexpr TickType_t portMAX_DELAY=0xffffffffu;
constexpr int pdTRUE=1;
TickType_t pdMS_TO_TICKS(uint32_t ms){return ms;}
struct Mutex {std::recursive_timed_mutex value;};Mutex storage;Mutex *controlMutex=&storage;
std::atomic<bool> otaInProgress{false};
std::atomic<unsigned> releases{0};bool raceOnTake=false;
int xSemaphoreTakeRecursive(Mutex*m,TickType_t wait){
 if(wait==portMAX_DELAY){m->value.lock();return pdTRUE;}
 if(!m->value.try_lock_for(std::chrono::milliseconds(wait)))return 0;
 if(raceOnTake)otaInProgress=true;
 return pdTRUE;
}
void xSemaphoreGiveRecursive(Mutex*m){++releases;m->value.unlock();}
struct AsyncWebServerRequest {int code=0;void send(int c,const char*,const char*){code=c;}};
'''+guard+r'''
int main(){
 AsyncWebServerRequest r;
 // OTA уже идёт: запрос получает 503 без ожидания мьютекса.
 otaInProgress=true;auto before=releases.load();{ControlGuard g(&r);assert(!g&&r.code==503);}assert(releases==before);
 r.code=0;{ControlGuard g(&r,false);assert(!g&&r.code==0);}
 // Долгое владение управлением: новая HTTP-блокировка ограничена 25 мс даже до onStart.
 otaInProgress=false;std::atomic<bool> held=false;
 std::thread updating([&](){ControlGuard mainGuard;held=true;std::this_thread::sleep_for(std::chrono::milliseconds(350));});
 while(!held)std::this_thread::yield();
 auto start=std::chrono::steady_clock::now();{ControlGuard g(&r);assert(!g&&r.code==503);}
 auto waited=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
 assert(waited<200);updating.join();
 // Гонка старта OTA после первого чтения флага освобождает только реально взятый мьютекс.
 raceOnTake=true;r.code=0;before=releases.load();{ControlGuard g(&r);assert(!g&&r.code==503);}assert(releases==before+1);
 raceOnTake=false;otaInProgress=false;r.code=0;before=releases.load();{ControlGuard g(&r);assert(g&&r.code==0);}assert(releases==before+1);
 controlMutex=nullptr;{ControlGuard g(&r);assert(g);}controlMutex=&storage;
 std::cout<<"PASS: actual HTTP guard returns promptly during OTA/busy mutex, upload chunks stay silent, race releases once, normal/main guards remain valid\n";
}
'''
# Все реальные HTTP-входы используют ограниченный вариант; в loop остаётся обычный guard.
http=source[source.index('class AsyncCompatServer {'):source.index('AsyncCompatServer server')]
assert 'ControlGuard guard;' not in http
manager=source[source.index('void serveBootstrapFileManager('):source.index('void setupWebServer()')]
assert 'ControlGuard guard;' not in manager
assert 'ArduinoOTA.setTimeout(5000)' in source
with tempfile.TemporaryDirectory(prefix='remuator-ota-') as work:
 p=Path(work);(p/'test.cpp').write_text(code)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-fsanitize=address,undefined','-pthread',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'},check=True)
