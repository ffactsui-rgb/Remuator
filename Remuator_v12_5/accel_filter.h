#pragma once
#include <algorithm>
#include <cmath>
#include <stdint.h>

// Фильтруем сам вектор ускорения до расчёта углов и защит: медиана трёх
// кадров убирает одиночный выброс, сглаживание учитывает интервал измерений.
class AccelFilter3 {
  float history[3][3]={}, value[3]={};
  uint8_t count=0,index=0;
  uint32_t at=0;
public:
  void reset() {count=index=0;at=0;}
  bool update(const float raw[3],float out[3],uint32_t now,bool active) {
    for(int axis=0;axis<3;++axis) if(!std::isfinite(raw[axis])) return false;
    const bool first=!count || uint32_t(now-at)>=1200;
    if(first) {count=0;index=0;for(int axis=0;axis<3;++axis)value[axis]=raw[axis];}
    for(int axis=0;axis<3;++axis) history[axis][index]=raw[axis];
    index=(index+1)%3;if(count<3) ++count;
    const float dt=float(uint32_t(now-at))/1000.0f;
    const float alpha=first ? 1.0f : dt/(dt+(active?0.035f:0.25f));
    for(int axis=0;axis<3;++axis) {
      float sorted[3];for(uint8_t i=0;i<count;++i)sorted[i]=history[axis][i];
      for(uint8_t i=1;i<count;++i) {float v=sorted[i];uint8_t j=i;while(j && sorted[j-1]>v){sorted[j]=sorted[j-1];--j;}sorted[j]=v;}
      value[axis]+=alpha*(sorted[count/2]-value[axis]);out[axis]=value[axis];
    }
    at=now;return true;
  }
};
inline float sensorAngleTenth(float angle) {const float value=std::round(angle*10.0f)/10.0f;return value==0 ? 0.0f : value;}
