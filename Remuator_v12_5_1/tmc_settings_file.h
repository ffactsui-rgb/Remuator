#pragma once
#include "tmc_config.h"
#include <cmath>
#include <cstring>

// Явные 32-битные поля исключают зависимость файла от выравнивания структур и bool.
struct TmcFileValues {
  TmcUartSettings y{TmcModel::Tmc2208, 0}, z{TmcModel::Tmc2209, 1};
  uint32_t yAcceleration=10000, ySpeed=5000, zAcceleration=10000, zSpeed=5000;
};
inline bool tmcFileAxisValid(const TmcUartSettings &c) {
  return c.baud>=9600 && c.baud<=500000 && c.rmsCurrent>=50 && c.rmsCurrent<=tmcMaxRms(c.model) &&
    std::isfinite(c.holdMultiplier) && c.holdMultiplier>=0.1f && c.holdMultiplier<=1 &&
    c.microsteps>=1 && c.microsteps<=256 && !(c.microsteps & (c.microsteps-1)) &&
    c.toff>=1 && c.toff<=15 && (c.blankTime==16 || c.blankTime==24 || c.blankTime==36 || c.blankTime==54) &&
    c.hysteresisEnd>=-3 && c.hysteresisEnd<=12 && c.hysteresisStart>=1 && c.hysteresisStart<=8 &&
    c.pwmFreq<=3 && c.pwmReg>=1 && c.pwmReg<=15 && c.pwmLim<=15 && c.freewheel<=3 &&
    c.sendDelay>=2 && c.sendDelay<=15 && c.pdnDisable;
}
inline bool tmcFileValuesValid(const TmcFileValues &v) {
  return !tmcBusError(v.y,v.z) && v.y.baud==v.z.baud && tmcFileAxisValid(v.y) && tmcFileAxisValid(v.z) &&
    v.yAcceleration>0 && v.yAcceleration<=2000000000UL && v.ySpeed>0 && v.ySpeed<=2000000000UL &&
    v.zAcceleration>0 && v.zAcceleration<=2000000000UL && v.zSpeed>0 && v.zSpeed<=2000000000UL;
}
template<class Storage> class TmcSettingsFile {
  struct Packet { uint32_t magic, schema, sequence, crc, axes[2][28], motion[4]; };
  Storage &storage;
  uint32_t sequence=0;
  int active=-1;
  const char *slot(int i) const { return i==0 ? "/settings/uart.bin" : "/settings/uart-backup.bin"; }
  static uint32_t hash(Packet p) {
    p.crc=0; uint32_t h=2166136261UL;
    for(size_t i=0;i<sizeof(p);++i) h=(h^reinterpret_cast<const uint8_t*>(&p)[i])*16777619UL;
    return h;
  }
  static void encode(const TmcUartSettings &c,uint32_t *w) {
    uint32_t hold;std::memcpy(&hold,&c.holdMultiplier,4);
    const uint32_t a[]={uint32_t(c.model),c.address,c.baud,c.rmsCurrent,hold,c.microsteps,c.toff,c.blankTime,
      uint32_t(int32_t(c.hysteresisEnd)),c.hysteresisStart,c.stealthChop,c.pwmAutoscale,c.pwmAutograd,
      c.pwmOfs,c.pwmGrad,c.pwmFreq,c.pwmReg,c.pwmLim,c.freewheel,c.intpol,c.pdnDisable,
      c.iScaleAnalog,c.internalRsense,c.multistepFilter,c.sendDelay,c.holdEnabled,0,0};
    std::memcpy(w,a,sizeof(a));
  }
  static bool decode(const uint32_t *w,TmcUartSettings &c) {
    c.model=TmcModel(w[0]);c.address=w[1];c.baud=w[2];c.rmsCurrent=w[3];
    std::memcpy(&c.holdMultiplier,&w[4],4);c.microsteps=w[5];c.toff=w[6];c.blankTime=w[7];
    c.hysteresisEnd=int32_t(w[8]);c.hysteresisStart=w[9];c.stealthChop=w[10];c.pwmAutoscale=w[11];
    c.pwmAutograd=w[12];c.pwmOfs=w[13];c.pwmGrad=w[14];c.pwmFreq=w[15];c.pwmReg=w[16];
    c.pwmLim=w[17];c.freewheel=w[18];c.intpol=w[19];c.pdnDisable=w[20];c.iScaleAnalog=w[21];
    c.internalRsense=w[22];c.multistepFilter=w[23];c.sendDelay=w[24];c.holdEnabled=w[25];
    uint32_t check[28];encode(c,check);
    return std::memcmp(w,check,sizeof(check))==0 && tmcFileAxisValid(c);
  }
  bool read(const char *path,Packet &p,TmcFileValues &v) {
    if(!storage.read(path,&p,sizeof(p)) || p.magic!=0x55415254UL || p.schema!=1 || !p.sequence || p.crc!=hash(p)) return false;
    if(!decode(p.axes[0],v.y) || !decode(p.axes[1],v.z)) return false;
    v.yAcceleration=p.motion[0];v.ySpeed=p.motion[1];v.zAcceleration=p.motion[2];v.zSpeed=p.motion[3];
    return tmcFileValuesValid(v);
  }
public:
  explicit TmcSettingsFile(Storage &s):storage(s){}
  bool load(TmcFileValues &v) {
    sequence=0;active=-1;
    for(int i=0;i<2;++i) {
      Packet p{};TmcFileValues candidate;
      if(read(slot(i),p,candidate) && p.sequence>sequence) {sequence=p.sequence;active=i;v=candidate;}
    }
    return active>=0;
  }
  bool save(const TmcFileValues &v) {
    if(!tmcFileValuesValid(v) || sequence==UINT32_MAX || !storage.ensureDirectory()) return false;
    Packet p{};p.magic=0x55415254UL;p.schema=1;p.sequence=sequence+1;
    encode(v.y,p.axes[0]);encode(v.z,p.axes[1]);
    p.motion[0]=v.yAcceleration;p.motion[1]=v.ySpeed;p.motion[2]=v.zAcceleration;p.motion[3]=v.zSpeed;p.crc=hash(p);
    const char *tmp="/settings/uart.tmp";Packet check{};TmcFileValues decoded;
    if(!storage.write(tmp,&p,sizeof(p)) || !read(tmp,check,decoded) || std::memcmp(&p,&check,sizeof(p))) return false;
    const int next=active==0?1:0;
    // Предыдущая целая копия остаётся доступной при отключении питания во время записи.
    if(!storage.replace(tmp,slot(next))) return false;
    active=next;sequence=p.sequence;return true;
  }
};
