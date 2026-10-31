"""Проверка фильтра в измерительном тракте, десятых градуса и раздельных сохранённых допусков X."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parent.parent
prefix=(root/'tests/calibration_cases.cpp').read_text().split('int main(){')[0]
prefix=prefix.replace('/* REAL_ENDSTOPS: настоящий обработчик концевиков для тестовой среды. */','')
prefix=prefix.replace('struct FakeFS {\n','struct FakeFS {\n bool fail=false;\n')
prefix=prefix.replace("File open(const char*k,const char*mode){", "File open(const char*k,const char*mode){if(fail&&mode[0]=='w')return {};")
prefix=prefix.replace('bool rename(const char*a,const char*b){','bool rename(const char*a,const char*b){if(fail)return false;')
main=r'''
int main(){
 calibrationInit();Wire.angle=40;programRunning=true;
 for(int i=0;i<80;++i){tms+=20;assert(calSample(true));}
 assert(fabsf(calAngle-40)<=0.2f);assert(calCurrentX==0);
 // Выброс подавляется до расчёта X: не только на картинке интерфейса.
 Wire.xG=0.6f;tms+=20;assert(calSample(true));assert(fabsf(calCurrentX)<0.1f);
 Wire.xG=0;for(int i=0;i<10;++i){tms+=20;assert(calSample(true));}
 for(int i=0;i<100;++i){Wire.xG=(i%2?0.001f:-0.001f);tms+=20;assert(calSample(true));assert(fabsf(calCurrentX)<=0.1f);}
 // Устойчивый реальный наклон проходит фильтр и становится виден рабочей защите.
 Wire.xG=tanf(25.f/CAL_RAD2DEG);
 int crossedMs=-1;
 for(int i=0;i<80;++i){tms+=20;assert(calSample(true));if(calCurrentX>20&&crossedMs<0)crossedMs=(i+1)*20;}
 assert(crossedMs>0&&crossedMs<=300);
 calXStored=true;calRefX=0;calToleranceX=3;calRunToleranceX=20;
 assert(calCurrentX>24.8f&&!calXWithinRunTolerance());
 assert(fabsf(calAngle*10-roundf(calAngle*10))<0.001f);
 assert(fabsf(calCurrentX*10-roundf(calCurrentX*10))<0.001f);
 // Свежая история после отсутствия данных, а не смесь с последним большим наклоном.
 Wire.online=false;tms+=1500;assert(!calSample(true));Wire.online=true;Wire.xG=0;Wire.angle=35;tms+=25;assert(calSample(true));
 assert(fabsf(calAngle-35)<=0.1f&&calCurrentX==0);
 programRunning=false;calCurrentX=3;assert(calXWithinTolerance());calCurrentX=3.1f;assert(!calXWithinTolerance());assert(calXWithinRunTolerance());
 programRunning=true;calCurrentX=10;assert(calXWithinRunTolerance());calCurrentX=-20.1f;assert(!calXWithinRunTolerance());programRunning=false;
 String why;assert(calSetXSettings(1,0,3,25,why));assert(calToleranceX==3&&calRunToleranceX==25);
 assert(prefs.getFloat("calXtol",0)==3&&prefs.getFloat("calXrun",0)==25);
 const auto saved=LittleFS.files["/calibration_x.csv"];
 assert(!calSetXSettings(1,0,3,2,why));assert(calRunToleranceX==25&&LittleFS.files["/calibration_x.csv"]==saved);
 LittleFS.fail=true;assert(!calSetXSettings(1,0,4,30,why));assert(calToleranceX==3&&calRunToleranceX==25);LittleFS.fail=false;
 calToleranceX=1;calRunToleranceX=2;assert(calLoadXFile());assert(calToleranceX==3&&calRunToleranceX==25);
 calXStored=true;calTableValid=true;assert(calSetXSettings(2,0,3,25,why));assert(!calXStored&&!calTableValid);
 std::cout<<"PASS: filtered sensor vector before angle/protection, 0.1-degree values, outlier/noise/real tilt/gap, X boundaries, file/NVS/rollback and axis invalidation\n";
}
'''
with tempfile.TemporaryDirectory(prefix='remuator-sensor-') as work:
 p=Path(work);(p/'test.cpp').write_text(prefix+main)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-fsanitize=address,undefined','-I',str(root),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'},check=True)
