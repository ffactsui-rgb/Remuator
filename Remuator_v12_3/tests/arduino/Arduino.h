#pragma once
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <cstdlib>
using std::abs;
inline uint32_t testMicros=0;
inline uint32_t micros(){return testMicros;}
inline uint32_t millis(){return testMicros/1000;}
constexpr int OUTPUT=1;
inline void pinMode(int,int){}
inline void digitalWrite(int,int){}
inline void delayMicroseconds(int n){testMicros+=n;}
inline long map(long x,long a,long b,long c,long d){return (x-a)*(d-c)/(b-a)+c;}
template<class T,class L,class H>T constrain(T x,L lo,H hi){return x<lo?T(lo):x>hi?T(hi):x;}
template<class A,class B>auto min(A a,B b){return a<b?a:b;}
template<class A,class B>auto max(A a,B b){return a>b?a:b;}
inline bool bitRead(unsigned n,int b){return (n>>b)&1;}
