"""Проверка установленной GyverStepper, включая ошибочные импульсы прежнего направления до исправления."""
from pathlib import Path
import argparse,subprocess,tempfile
r=Path(__file__).resolve().parent.parent
p=argparse.ArgumentParser();p.add_argument('--library',type=Path,required=True);args=p.parse_args()
s=(r/'calibration_y.h').read_text();start=s.index('void calMoveFromRest(');end=s.index('\n}',start)+2
helper=s[start:end]
code=r'''
#include <GyverStepper.h>
#include <cassert>
#include <iostream>
void pulse(uint8_t){}
GStepper<STEPPER2WIRE,STEPPER_VIRTUAL> motor_Naklon(200);
uint32_t NAKLON_MAX_SPEED=6000;bool motorNaklonMoving=false;
void enableDrivers(){}
void tickFor(uint32_t us){for(uint32_t i=0;i<us;i+=10){testMicros+=10;motor_Naklon.tick();}}
void warmup(int direction,uint16_t acceleration,uint32_t speed){
 motor_Naklon.brake();motor_Naklon.setCurrent(0);
 motor_Naklon.setAcceleration(acceleration);motor_Naklon.setMaxSpeed(1);motor_Naklon.setMaxSpeed(speed);
 motor_Naklon.setTarget(direction*200000000);tickFor(5000000);
 motor_Naklon.brake();motor_Naklon.setCurrent(108410);motor_Naklon.setTarget(108410);testMicros+=350000;
}
'''+helper+r'''
int main(){
 motor_Naklon.attachStep(pulse);
 warmup(1,10000,6000);
 motor_Naklon.setMaxSpeed(6000);motor_Naklon.setTarget(-199891590);
 tickFor(1000);assert(motor_Naklon.getCurrent()>108410);
 std::cout<<"REPRODUCED pre-fix wrong-way restart: "<<motor_Naklon.getCurrent()-108410<<" steps into upper switch in 1 ms\n";
 for(auto speed:{1u,50u,1000u,6000u,20000u})for(uint16_t accel:{0,300,10000})for(int before:{-1,1}){
  warmup(before,accel,speed);NAKLON_MAX_SPEED=speed;
  calMoveFromRest(108410-before*200000000);
  auto previous=motor_Naklon.getCurrent();int steps=0;
  for(int t=0;t<2000000;t+=10){testMicros+=10;motor_Naklon.tick();auto now=motor_Naklon.getCurrent();
   assert(before>0 ? now<=previous : now>=previous);if(now!=previous)++steps;previous=now;
  }
  assert(steps>0);
 }
 std::cout<<"PASS actual GyverStepper: both restart directions, 5 speed limits, zero/nonzero accelerations; no wrong-way STEP\n";
}
'''
with tempfile.TemporaryDirectory(prefix='real-stepper-') as w:
 cpp=Path(w)/'test.cpp';out=Path(w)/'test';cpp.write_text(code)
 subprocess.run(['g++','-std=c++17','-O1','-I',str(r/'tests/arduino'),'-I',str(args.library),str(cpp),'-o',str(out)],check=True)
 subprocess.run([str(out)],check=True)
