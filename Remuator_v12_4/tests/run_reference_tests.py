"""Проверка применения сохранённых таблиц и задания только нуля без движения двигателя."""
from pathlib import Path
import re,subprocess,tempfile,os
r=Path(__file__).resolve().parent.parent
s=next(r.glob('*.ino')).read_text()
def fun(name):
 start=s.index('void '+name+'(') if 'void '+name+'(' in s else s.index('bool '+name+'(')
 end=s.index('\n}',start)+2;return s[start:end]
prefix=(r/'tests/calibration_cases.cpp').read_text().split('int main(){')[0].replace('/* REAL_ENDSTOPS: настоящий обработчик концевиков для тестовой среды. */','')
prefix=prefix.replace('  void flush(){}void close(){}','  void flush(){}void close(){}')
prefix=prefix.replace(' float getFloat(', ''' int32_t getInt(const char*k,int32_t d){if(!blob.count(k))return d;int32_t v;memcpy(&v,blob[k].data(),4);return v;}
 uint16_t getUShort(const char*k,uint16_t d){if(!blob.count(k))return d;uint16_t v;memcpy(&v,blob[k].data(),2);return v;}
 uint32_t getUInt(const char*k,uint32_t d){return (uint32_t)getInt(k,d);}
 size_t putInt(const char*k,int32_t v){return putBytes(k,&v,4);}
 size_t putUInt(const char*k,uint32_t v){return putBytes(k,&v,4);}
 size_t putUShort(const char*k,uint16_t v){return putBytes(k,&v,2);}
 float getFloat(''')
prefix=prefix.replace('  String readStringUntil', '  String readStringUntil')
# Результат сложения имитации Arduino String преобразуется существующим конструктором.
extra=r'''
#include "tmc_config.h"
TmcUartSettings uartPovorot;
constexpr int32_t DEFAULT_Z_STEPS_360=40392,Z_CAL_MAX_ABS_STEPS=1000000;
int32_t zCalSteps360=40392;uint16_t zCalMicrosteps=0;bool zCalStored=false,zCalStarted=false,zCalJogging=false;
uint32_t zCalSavedHash=0;std::atomic<int32_t>zCalStepSnapshot{0};
uint32_t zCalibrationDigest(int32_t st,uint16_t micro){return uint32_t(st)^uint32_t(micro)*2654435761UL;}
bool zCalibrationValid(){return zCalStored&&zCalSteps360>=100&&zCalSteps360<=Z_CAL_MAX_ABS_STEPS&&zCalMicrosteps==uartPovorot.microsteps&&zCalSavedHash==zCalibrationDigest(zCalSteps360,zCalMicrosteps);}
bool recoveryRequired=false;
bool stepMotionPending(){return calibrationBusy()||motor_Naklon.getState()||motor_Povorot.getState();}
struct Server {std::map<std::string,String>args;int code=0;String response;String arg(const char*k){return args[k];}void send(int n,const char*,String b){code=n;response=b;}}server;
'''
z=(r/'calibration_z.h').read_text();start=z.index('void zCalibrationLoad()');end=z.index('\n}',start)+2
cases=r'''
int main(){
 tms=100;calibrationInit();calXStored=true;calRefX=calCurrentX;assert(calSaveXFile());
 calProposal={};calProposal.n=2;calProposal.rows[0]={84,0};calProposal.rows[1]={85,2000};calProposal.correctedMin=84;calProposal.crc=calHash(calProposal);
 assert(calValidate(calProposal));String why;assert(calibrationSave(why));
 LittleFS.files["/calibration_z.csv"]="microsteps,stepsPer360\n16,40392\n";
 calTableValid=false;zCalStored=false;server.args["action"]="apply";handleReferenceAction();
 assert(server.code==200&&calibrationValid()&&zCalibrationValid());
 const auto turn=zCalSteps360;const auto micro=zCalMicrosteps;const auto digest=zCalSavedHash;
 motor_Povorot.current=12345;motor_Povorot.target=12345;
 server.args["action"]="zeroZ";handleReferenceAction();
 assert(server.code==200&&motor_Povorot.current==0&&motor_Povorot.target==0);
 assert(zCalibrationValid()&&zCalSteps360==turn&&zCalMicrosteps==micro&&zCalSavedHash==digest);
 zCalStored=false;LittleFS.files["/calibration_z.csv"]="microsteps,stepsPer360\n32,40392\n";
 server.args["action"]="applyZ";handleReferenceAction();assert(server.code==422&&!zCalStored);
 LittleFS.files["/calibration_z.csv"]="microsteps,stepsPer360\n16,40392\n";
 LittleFS.files.erase("/calibration_y.csv");
 handleReferenceAction();assert(server.code==200&&zCalibrationValid());
 motor_Naklon.setTarget(100);handleReferenceAction();assert(server.code==409);
 std::cout<<"PASS: actual Y/Z table application, Z file microstep compatibility, zero-only preserves STEP/360 and calibration digest, movement guard\n";
}
'''
with tempfile.TemporaryDirectory(prefix='reference-tests-') as w:
 cpp=Path(w)/'test.cpp';out=Path(w)/'test'
 cpp.write_text(prefix+extra+z[start:end]+fun('freshMeasuredY')+fun('handleReferenceAction')+cases)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-I',str(r),str(cpp),'-o',str(out)],check=True)
 subprocess.run([str(out)],check=True)
