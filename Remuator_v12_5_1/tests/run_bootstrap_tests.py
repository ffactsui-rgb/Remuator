"""Проверка настоящих правил начальной настройки и маршрутов с имитациями сети и файловой системы."""
from pathlib import Path
import os,re,subprocess,tempfile
root=Path(__file__).resolve().parent.parent
source=next(root.glob('*.ino')).read_text()
def function(name):
 m=re.search(r'^(?:void|String) '+name+r'\([^\n]*\) \{',source,re.M)
 start=m.start();pos=m.end()-1;depth=0;quote=None;escape=False
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
prefix=r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>
struct String:std::string {using std::string::string;String(const std::string&s):std::string(s){}String()=default;size_t length()const{return size();}};
struct File {bool present=false,dir=false;size_t bytes=0;explicit operator bool(){return present;}bool isDirectory(){return dir;}size_t size(){return bytes;}void close(){}};
struct FS {std::map<std::string,File>files;File open(const char*p,const char*){return files.count(p)?files[p]:File{};}}LittleFS;
#include "bootstrap_policy.h"
const char BOOTSTRAP_WIFI_HTML[]="EMBEDDED_WIFI";
const char BOOTSTRAP_MANAGER_HTML[]="EMBEDDED_MANAGER";
bool moving=false;
bool stepMotionPending(){return moving;}
struct ControlGuard {template<class T>ControlGuard(T*,bool=true){} explicit operator bool()const{return true;}};
struct AsyncWebServerResponse {bool memory=false;void addHeader(const char*,const char*){}};
struct AsyncWebServerRequest {
 bool authenticated=false,fromMemory=false;int code=0;AsyncWebServerResponse response;
 void send(int n,const char*,const char*){code=n;}
 void send(AsyncWebServerResponse*r){code=200;fromMemory=r->memory;}
 AsyncWebServerResponse*beginResponse(int,const char*,const char*){response.memory=true;return &response;}
 AsyncWebServerResponse*beginResponse(FS&,const char*,const char*,bool){response.memory=false;return &response;}
};
bool fmAuthenticated(AsyncWebServerRequest*r){if(r->authenticated)return true;r->code=401;return false;}
struct Server {std::map<std::string,String>args;int code=0;String location,body;bool saved=false;bool hasArg(const char*k){return args.count(k);}String arg(const char*k){return args[k];}
void sendHeader(const char*k,const char*v){if(std::string(k)=="Location")location=v;}void send(int n){code=n;}void send(int n,const char*,String b){code=n;body=b;}}server;
void sendLittleFSFile(String p){server.code=200;server.body=p;}
void handleNetworkSave(){server.saved=true;server.code=200;}
struct IP {String text;String toString(){return text;}};
constexpr int WL_CONNECTED=3;
struct Wifi {bool connected=false;int status(){return connected?3:0;}IP localIP(){return {"192.168.1.25"};}IP softAPIP(){return {"192.168.4.1"};}}WiFi;
'''
funcs='\n'.join(function(n) for n in ['handleRoot','handleBootstrapSetup','handleNetwork','handleBootstrapWifiSave','bootstrapStatusJson','serveBootstrapFileManager'])
cases=r'''
int main(){
 handleRoot();assert(server.code==303&&server.location=="/setup");
 handleBootstrapSetup();assert(server.code==200&&server.body=="EMBEDDED_WIFI");
 handleNetwork();assert(server.body=="EMBEDDED_WIFI");
 assert(!bootstrapWebReady());assert(bootstrapStatusJson().find("\"webReady\":false")!=String::npos);
 bootstrapFsMounted=true;
 for(auto path:BOOTSTRAP_REQUIRED_FILES)LittleFS.files[path]={true,false,10};
 assert(bootstrapWebReady()&&bootstrapManagerReady());
 AsyncWebServerRequest req;serveBootstrapFileManager(&req,false);assert(req.code==401);
 req.authenticated=true;serveBootstrapFileManager(&req,false);assert(req.code==200&&!req.fromMemory);
 serveBootstrapFileManager(&req,true);assert(req.fromMemory);
 moving=true;serveBootstrapFileManager(&req,true);assert(req.code==423);moving=false;
 handleRoot();assert(server.code==200&&server.body=="/index.html");
 LittleFS.files["/home.js"].bytes=0;serveBootstrapFileManager(&req,false);assert(req.fromMemory);assert(!bootstrapWebReady());handleRoot();assert(server.code==303);
 LittleFS.files["/home.js"].bytes=10;
 LittleFS.files["/filemanager.js"].present=false;assert(!bootstrapManagerReady()&&!bootstrapWebReady());
 LittleFS.files["/filemanager.js"]={true,true,10};assert(!bootstrapManagerReady());
 LittleFS.files["/filemanager.js"]={true,false,10};assert(bootstrapWebReady());
 WiFi.connected=true;auto status=bootstrapStatusJson();assert(status.find("\"staConnected\":true")!=String::npos&&status.find("192.168.1.25")!=String::npos);
 assert(status.find("pass")==String::npos);
 assert(bootstrapCredentialsValid("Open", ""));assert(bootstrapCredentialsValid("Home", "abcdefgh"));
 assert(!bootstrapCredentialsValid("", "abcdefgh"));assert(!bootstrapCredentialsValid(String(33,'x'), "abcdefgh"));
 assert(!bootstrapCredentialsValid("Home", "short"));assert(bootstrapCredentialsValid("Home", String(64,'A')));assert(!bootstrapCredentialsValid("Home", String(64,'Z')));
 server.args={{"ssid","Home"},{"pass","short"},{"homeen","1"}};handleBootstrapWifiSave();assert(server.code==422&&!server.saved);
 server.args["pass"]="abcdefgh";handleBootstrapWifiSave();assert(server.code==200&&server.saved);
 std::vector<uint8_t> bytes(1025,0xFF);bool readable=true;
 auto read=[&](size_t off,uint8_t*out,size_t n){if(!readable)return false;memcpy(out,bytes.data()+off,n);return true;};
 assert(bootstrapPartitionErased(bytes.size(),read));bytes.back()=0;assert(!bootstrapPartitionErased(bytes.size(),read));bytes.back()=0xFF;
 readable=false;assert(!bootstrapPartitionErased(bytes.size(),read));assert(!bootstrapPartitionErased(0,read));
 std::cout<<"PASS: empty/partial/complete filesystem, memory setup routes, credential validation, connected Wi-Fi status, erased-only initialization and read failure\n";
}
'''
with tempfile.TemporaryDirectory(prefix='bootstrap-tests-') as w:
 cpp=Path(w)/'test.cpp';binary=Path(w)/'test';cpp.write_text(prefix+funcs+cases)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-fsanitize=address,undefined','-I',str(root),str(cpp),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'},check=True)
