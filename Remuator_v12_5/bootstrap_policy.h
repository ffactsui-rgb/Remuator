#pragma once
#include <stddef.h>
#include <stdint.h>
static bool bootstrapFsMounted=false;
static const char *const BOOTSTRAP_REQUIRED_FILES[] = {
  "/about.html",
  "/about.js",
  "/app.js",
  "/calibration-y.html",
  "/calibration-y.js",
  "/calibration-z.html",
  "/calibration-z.js",
  "/edit.css",
  "/edit.html",
  "/edit.js",
  "/filemanager.css",
  "/filemanager.html",
  "/filemanager.js",
  "/home.js",
  "/index.html",
  "/network.html",
  "/network.js",
  "/parameter-help.js",
  "/programs.html",
  "/programs.js",
  "/serial.html",
  "/serial.js",
  "/settings.html",
  "/settings.js",
  "/site-shell.js",
  "/style.css",
  "/uart.html",
  "/uart.js",
};
inline bool bootstrapAssetPresent(const char *path) {
  auto f=LittleFS.open(path,"r");
  const bool ok=f && !f.isDirectory() && f.size()>0;
  if(f) f.close();
  return ok;
}
inline bool bootstrapWebReady() {
  if(!bootstrapFsMounted) return false;
  for(const char *path:BOOTSTRAP_REQUIRED_FILES) if(!bootstrapAssetPresent(path)) return false;
  return true;
}
inline bool bootstrapManagerReady() {
  if(!bootstrapFsMounted) return false;
  for(const char *path:{"/filemanager.html","/filemanager.css","/filemanager.js","/style.css","/site-shell.js"})
    if(!bootstrapAssetPresent(path)) return false;
  return true;
}
// После ошибки монтирования создание файловой системы разрешено только тогда, когда каждый байт
// её раздела стёрт. Ошибка чтения или любые существующие данные запрещают форматирование.
template<class Reader> bool bootstrapPartitionErased(size_t size,Reader read) {
  if(size==0) return false;
  uint8_t bytes[256];
  for(size_t offset=0;offset<size;offset+=sizeof(bytes)) {
    size_t count=size-offset<sizeof(bytes)?size-offset:sizeof(bytes);
    if(!read(offset,bytes,count)) return false;
    for(size_t i=0;i<count;++i) if(bytes[i]!=0xFF) return false;
  }
  return true;
}
inline bool bootstrapCredentialsValid(const String &ssid,const String &password) {
  if(ssid.length()==0 || ssid.length()>32) return false;
  for(size_t i=0;i<ssid.length();++i) if((uint8_t)ssid[i]<0x20) return false;
  if(password.length()==0) return true;
  if(password.length()>=8 && password.length()<=63) return true;
  if(password.length()!=64) return false;
  for(size_t i=0;i<password.length();++i) {
    char c=password[i];
    if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))) return false;
  }
  return true;
}
