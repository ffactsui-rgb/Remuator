"""Проверка файла UART и прежнего модуля; активный фильтр измерений проверяется run_sensor_tests.py."""
from pathlib import Path
import subprocess, tempfile, os
root=Path(__file__).resolve().parent.parent
code=r'''
#include "tmc_settings_file.h"
#include "y_angle_filter.h"
#include <map>
#include <vector>
#include <string>
#include <cassert>
#include <iostream>
struct Storage {
 std::map<std::string,std::vector<unsigned char>> files;
 bool failWrite=false,failRename=false;
 bool ensureDirectory(){return true;}
 bool read(const char*p,void*b,size_t n){if(!files.count(p)||files[p].size()!=n)return false;std::memcpy(b,files[p].data(),n);return true;}
 bool write(const char*p,const void*b,size_t n){const auto*c=static_cast<const unsigned char*>(b);files[p]={c,c+(failWrite?n/2:n)};return !failWrite;}
 bool replace(const char*a,const char*b){if(failRename)return false;files[b]=files[a];files.erase(a);return true;}
};
int main(){
 Storage s;TmcFileValues v,loaded;TmcSettingsFile<Storage> store(s);
 assert(!store.load(loaded));v.y.holdEnabled=true;v.z.stealthChop=true;v.z.microsteps=64;
 v.yAcceleration=1234;v.zSpeed=6789;assert(store.save(v));
 TmcSettingsFile<Storage> reboot(s);assert(reboot.load(loaded));assert(loaded.y.holdEnabled&&loaded.z.microsteps==64&&loaded.zSpeed==6789);
 v.y.rmsCurrent=1000;assert(reboot.save(v));
 TmcSettingsFile<Storage> newest(s);assert(newest.load(loaded)&&loaded.y.rmsCurrent==1000);
 // Оборванная запись и сбой переименования сохраняют последнюю целую копию.
 v.y.rmsCurrent=1100;s.failWrite=true;assert(!newest.save(v));s.failWrite=false;
 TmcSettingsFile<Storage> cut(s);assert(cut.load(loaded)&&loaded.y.rmsCurrent==1000);
 s.failRename=true;assert(!cut.save(v));s.failRename=false;
 TmcSettingsFile<Storage> cut2(s);assert(cut2.load(loaded)&&loaded.y.rmsCurrent==1000);
 // Повреждённая новая копия откатывается к предыдущей проверенной.
 s.files["/settings/uart-backup.bin"][20]^=1;TmcSettingsFile<Storage> broken(s);
 assert(broken.load(loaded)&&loaded.y.rmsCurrent==900);
 auto before=s.files;v.y.microsteps=3;assert(!broken.save(v)&&s.files==before);
 v.y.microsteps=16;v.z.address=0;assert(!broken.save(v));
 s.files["/settings/uart.bin"].resize(4);TmcSettingsFile<Storage> bothBad(s);assert(!bothBad.load(loaded));
 // Дрожание ±0,1° на месте не видно; одиночный выброс не поворачивает индикатор.
 YAngleFilter f;float y=f.update(30,1000,false);
 for(unsigned i=1;i<150;++i){y=f.update(30+(i%2?0.1f:-0.1f),1000+i*20,false);assert(std::fabs(y-30)<0.16f);}
 y=f.update(60,4000,false);assert(std::fabs(y-30)<0.16f);
 // Устойчивое реальное изменение отражается, а после паузы старое измерение не тянется.
 for(unsigned i=1;i<100;++i)y=f.update(40,4000+i*20,true);
 assert(std::fabs(y-40)<0.1f);assert(f.update(55,10000,false)==55);
 std::cout<<"PASS: UART file restart, both axes, backup/CRC, interrupted write/rename, invalid fields; Y jitter, outlier, motion and stale reset\n";
}
'''
with tempfile.TemporaryDirectory(prefix='remuator-settings-') as tmp:
 p=Path(tmp);(p/'test.cpp').write_text(code)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-fsanitize=address,undefined','-I',str(root),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'},check=True)
