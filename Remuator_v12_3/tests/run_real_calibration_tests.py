"""Проверка настоящей калибровки и обработчика NC с установленным планировщиком GyverStepper."""
from pathlib import Path
import argparse,re,subprocess,tempfile
r=Path(__file__).resolve().parent.parent
p=argparse.ArgumentParser();p.add_argument('--library',type=Path,required=True);p.add_argument('--baseline',type=Path);a=p.parse_args()
prefix=(r/'tests/calibration_cases.cpp').read_text().split('int main(){')[0]
prefix='#include <GyverStepper.h>\n'+prefix
prefix=re.sub(r'unsigned tms=0; unsigned millis\(\)\{return tms;\}', '',prefix)
prefix=prefix.replace('return (uint64_t)tms*1000ULL;', 'return testMicros;').replace('tms+=dt;', 'testMicros+=dt*1000;')
prefix=re.sub(r'template<typename T,typename A,typename B>T constrain[^\n]+\n','',prefix)
prefix=re.sub(r'struct Motor \{.*?\} motor_Naklon,motor_Povorot;', 'GStepper<STEPPER2WIRE,STEPPER_VIRTUAL> motor_Naklon(200),motor_Povorot(200);',prefix,flags=re.S)
ino=next(r.glob('*.ino')).read_text();start=ino.index('void checkEndstops() {');end=ino.index('\n}\n',start)+2
prefix=prefix.replace('/* REAL_ENDSTOPS: настоящий обработчик концевиков для тестовой среды. */',ino[start:end])
main=r'''
void pulse(uint8_t){}
void runFor(uint32_t us){for(uint32_t i=0;i<us;i+=100){testMicros+=100;checkEndstops();motor_Naklon.tick();if(i%1000==0)calibrationService();}}
int main(){
 motor_Naklon.attachStep(pulse);motor_Povorot.attachStep(pulse);
 motor_Naklon.setAcceleration(10000);motor_Naklon.setMaxSpeed(6000);
 calibrationInit();calXStored=true;calRefX=calCurrentX;String why;
 assert(calibrationBegin(why));runFor(5000000);
 upperPin=HIGH;checkEndstops();assert(calPhase==CAL_START_DOWN);
 runFor(450000);
#ifdef EXPECT_FIXED
 assert(calPhase==CAL_SEEK_23);assert(motor_Naklon.getCurrent()<calUpperCoord);
 std::cout<<"PASS actual calibration + NC + GyverStepper: reversal proceeds, no incorrect-step error\n";
#else
 assert(calPhase==CAL_ERROR);assert(calMessage.find("Некорректный счёт шагов Y")!=String::npos);
 std::cout<<"REPRODUCED exact reported error with baseline: incorrect Y step count after 85-degree switch\n";
#endif
}
'''
with tempfile.TemporaryDirectory(prefix='real-calibration-') as w:
 cpp=Path(w)/'test.cpp';cpp.write_text(prefix+main)
 for label,root in ([('baseline',a.baseline)] if a.baseline else [])+[('fixed',r)]:
  binary=Path(w)/label
  cmd=['g++','-std=c++17','-O1','-I',str(r/'tests/arduino'),'-I',str(a.library),'-I',str(root),str(cpp),'-o',str(binary)]
  if label=='fixed':cmd.insert(1,'-DEXPECT_FIXED')
  subprocess.run(cmd,check=True);subprocess.run([str(binary)],check=True)
