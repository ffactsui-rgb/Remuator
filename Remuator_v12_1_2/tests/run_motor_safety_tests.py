"""Compile the actual hold/test/reboot handlers with deterministic hardware doubles."""
from pathlib import Path
import re, subprocess, tempfile, os
root=Path(__file__).resolve().parent.parent
source=next(root.glob('*.ino')).read_text()
def function(name):
    match=re.search(r'^(?:void|bool) '+name+r'\([^\n]*\) \{',source,re.M)
    start=match.start();pos=match.end()-1;depth=0;quote=None;escape=False
    while pos<len(source):
        c=source[pos]
        if quote:
            if escape:escape=False
            elif c=='\\':escape=True
            elif c==quote:quote=None
        elif source[pos:pos+2]=='//':pos=source.index('\n',pos);continue
        elif c in "\"'":quote=c
        elif c=='{':depth+=1
        elif c=='}':
            depth-=1
            if not depth:return source[start:pos+1]
        pos+=1
names=['releaseDrivers','enableDrivers','disableDrivers','stopMotorTest','handleMotorTest','handleReboot']
functions=[function(n) for n in names]
prefix=r'''
#include <cassert>
#include <cstdint>
#include <climits>
#include <map>
#include <string>
#include <iostream>
#include "tmc_config.h"
using String=std::string;
constexpr int LOW=0,HIGH=1,Y_ENABLE_PIN=6,Z_ENABLE_PIN=40;
std::map<int,int> pins;
void digitalWrite(int pin,int value){pins[pin]=value;}
struct Motor {int32_t current=0,target=0;bool getState(){return current!=target;}
int32_t getCurrent(){return current;} void brake(){target=current;} void setTarget(int32_t n){target=n;}}motor_Naklon,motor_Povorot;
struct Server {std::map<std::string,String>args;int code=0;String response;
String arg(const char*k){return args[k];}void send(int c,const char*,String s){code=c;response=s;}}server;
struct Axis {bool settingsApplied=true;}tmcNaklon,tmcPovorot;
TmcUartSettings uartNaklon,uartPovorot;
bool normalDriverPower=false,yEndstopFault=false,recoveryMotionUncertain=false;
bool motorTestActive=false,programRunning=false,homingActive=false,zCalStarted=false;
bool motorNaklonMoving=false,motorPovorotMoving=false,serialMoveNaklonActive=false,serialMovePovorotActive=false;
int moveDirectionNaklon=0,moveDirectionPovorot=0;
uint32_t rebootAt=0,motorTestHeartbeat=0,motorTestStarted=0,now=100;
uint32_t millis(){return now;}
bool calibration=false,canMove=true,saveOk=true,stopped=false;
bool calibrationBusy(){return calibration;}void calStopWithError(String){calibration=false;}
void zCalibrationStopMotion(){motor_Povorot.brake();}
bool canMoveY(int){return canMove;}
bool stepMotionPending(){return motor_Naklon.getState()||motor_Povorot.getState();}
void stopProgram(){motor_Naklon.brake();motor_Povorot.brake();programRunning=false;stopped=true;}
bool saveExecution(){assert(stopped&&!stepMotionPending());return saveOk;}
'''
cases=r'''
int main(){
 calibration=true;releaseDrivers();assert(pins[6]==LOW&&pins[40]==HIGH);
 calibration=false;releaseDrivers();assert(pins[6]==HIGH&&pins[40]==HIGH);
 uartNaklon.holdEnabled=true;releaseDrivers();assert(pins[6]==LOW&&pins[40]==HIGH);
 uartPovorot.holdEnabled=true;releaseDrivers();assert(pins[6]==LOW&&pins[40]==LOW);
 disableDrivers();assert(!normalDriverPower&&pins[6]==HIGH&&pins[40]==HIGH);
 yEndstopFault=true;releaseDrivers();assert(pins[6]==HIGH&&pins[40]==HIGH);yEndstopFault=false;
 recoveryMotionUncertain=true;releaseDrivers();assert(!normalDriverPower);recoveryMotionUncertain=false;
 server.args={{"motor","nak"},{"action","plus"}};canMove=false;handleMotorTest();assert(server.code==409&&!motorTestActive);
 canMove=true;programRunning=true;handleMotorTest();assert(server.code==409&&!motorTestActive);programRunning=false;
 tmcNaklon.settingsApplied=false;handleMotorTest();assert(server.code==409);tmcNaklon.settingsApplied=true;
 handleMotorTest();assert(server.code==200&&motorTestActive&&motor_Naklon.target==1000000);
 now=500;server.args["action"]="heartbeat";handleMotorTest();assert(motorTestHeartbeat==500);
 server.args["action"]="stop";handleMotorTest();assert(!motorTestActive&&!stepMotionPending());
 server.args["motor"]="pov";server.args["action"]="minus";handleMotorTest();assert(motor_Povorot.target==-1000000);
 handleReboot();assert(server.code==200&&rebootAt==1500&&!motorTestActive&&stopped);
 assert(pins[6]==HIGH&&pins[40]==HIGH);releaseDrivers();assert(pins[6]==HIGH&&pins[40]==HIGH);
 handleMotorTest();assert(server.code==409);
 rebootAt=0;saveOk=false;handleReboot();assert(server.code==503&&rebootAt==0&&pins[6]==HIGH);
 std::cout<<"PASS: actual hold EN, fault override, manual test guards/heartbeat/stop, reboot stop-save-disable ordering and save failure\n";
}
'''
with tempfile.TemporaryDirectory(prefix='motor-safety-') as work:
    cpp=Path(work)/'test.cpp';out=Path(work)/'test'
    cpp.write_text(prefix+'\n'.join(f[:f.index('{')].strip()+';' for f in functions)+'\n'+'\n'.join(functions)+cases)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-fsanitize=address,undefined','-I',str(root),str(cpp),'-o',str(out)],check=True)
    subprocess.run([str(out)],env={**os.environ,"ASAN_OPTIONS":"detect_leaks=0"},check=True)
