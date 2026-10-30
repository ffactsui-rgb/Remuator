#pragma once
#include <cmath>
#include <stdint.h>
#include <algorithm>

// Фильтр индикации: медиана подавляет одиночные выбросы, сглаживание и зона
// нечувствительности убирают дрожание на месте. Калибровка использует отдельный угол.
class YAngleFilter {
  float samples[5]={}, smoothed=NAN, displayed=NAN;
  uint8_t count=0,index=0;
  uint32_t at=0;
public:
  float update(float angle,uint32_t now,bool moving) {
    if(!std::isfinite(angle)) return displayed;
    if(!count || uint32_t(now-at)>1500) {count=0;index=0;smoothed=displayed=angle;}
    samples[index]=angle;index=(index+1)%5;if(count<5) ++count;
    float ordered[5];std::copy(samples,samples+count,ordered);
    // Для пяти элементов достаточно сортировки вставками с явными границами.
    for(uint8_t i=1;i<count;++i) {float v=ordered[i];uint8_t j=i;while(j && ordered[j-1]>v) {ordered[j]=ordered[j-1];--j;}ordered[j]=v;}
    const float median=ordered[count/2];
    const float dt=std::min(1.0f,float(uint32_t(now-at))/1000.0f);
    const float alpha=dt/(dt+(moving?0.12f:0.65f));
    smoothed+=alpha*(median-smoothed);
    if(moving || fabsf(smoothed-displayed)>=0.15f) displayed=smoothed;
    at=now;return displayed;
  }
};
