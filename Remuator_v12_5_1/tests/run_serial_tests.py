"""Проверка настоящих обработчиков выключения Serial и сохранения выбранного состояния."""
from pathlib import Path
import os, re, subprocess, tempfile
root=Path(__file__).resolve().parent.parent
source=next(root.glob('*.ino')).read_text()
def function(name):
 match=re.search(r'^(?:void|bool) '+name+r'\([^\n]*\) \{',source,re.M)
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
#include <cassert>
#include <map>
#include <string>
#include <iostream>
struct String:std::string {using std::string::string;String(const std::string&s):std::string(s){}};
struct Prefs {bool fail=false;std::map<std::string,bool> values;size_t putBool(const char*k,bool v){if(fail)return 0;values[k]=v;return sizeof(bool);}bool getBool(const char*k,bool d){return values.count(k)?values[k]:d;}}prefs;
struct Server {std::map<std::string,String>args;int code=0;String body;String arg(const char*k){return args[k];}void send(int c,const char*,String b){code=c;body=b;}}server;
bool serialOutputEnabled=true,programRunning=false,moving=false,otaInProgress=false,zCalStarted=false;
bool stepMotionPending(){return moving;}
'''
code+='\n'.join(function(n) for n in ['saveSerialOutput','handleSerialSettings','handleSerialSettingsSave'])
code+=r'''
int main(){
 handleSerialSettings();assert(server.code==200&&server.body.find("\"serialOn\":true")!=std::string::npos);
 server.args["enabled"]="0";handleSerialSettingsSave();assert(server.code==200&&!serialOutputEnabled);
 serialOutputEnabled=true;serialOutputEnabled=prefs.getBool("serialOn",true);assert(!serialOutputEnabled);
 server.args["enabled"]="1";handleSerialSettingsSave();assert(server.code==200&&serialOutputEnabled);
 prefs.fail=true;server.args["enabled"]="0";handleSerialSettingsSave();assert(server.code==507&&serialOutputEnabled&&prefs.getBool("serialOn",false));prefs.fail=false;
 programRunning=true;handleSerialSettingsSave();assert(server.code==409&&serialOutputEnabled);programRunning=false;
 moving=true;handleSerialSettingsSave();assert(server.code==409);moving=false;
 otaInProgress=true;handleSerialSettingsSave();assert(server.code==409);otaInProgress=false;
 server.args["enabled"]="garbage";handleSerialSettingsSave();assert(server.code==422&&serialOutputEnabled);
 std::cout<<"PASS: Serial off/on, saved boot state, NVS failure rollback, invalid input and movement/OTA guards\n";
}
'''
api=(root/'web_api.h').read_text()
assert '\\"serialOn\\":' in api and 'jsonBool(serialOutputEnabled)' in api
assert source.count('serialOutputEnabled = prefs.getBool("serialOn", true);')==2
assert 'if (serialOutputEnabled && !zCalStarted && !stepMotionPending())' in source
with tempfile.TemporaryDirectory(prefix='remuator-serial-') as work:
 p=Path(work);(p/'test.cpp').write_text(code)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'},check=True)
