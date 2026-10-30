"""Native DS3231 register test: OSF, BCD, UTC, missing I2C, write/read-back."""
from pathlib import Path
import subprocess,tempfile,os
root=Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix='remuator-rtc-') as work:
    work=Path(work)
    (work/'Arduino.h').write_text('#pragma once\n#include <cstdint>\n#include <cstddef>\n#include <ctime>\n')
    (work/'Wire.h').write_text('''#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
class TwoWire {
public:
  uint8_t mem[256] = {};
  bool connected=true;
  size_t selected=0, writeCount=0, readIndex=0;
  std::vector<uint8_t> output;
  void beginTransmission(uint8_t address) { (void)address;output.clear(); }
  size_t write(uint8_t data) { output.push_back(data);return 1; }
  uint8_t endTransmission(bool stop=true) {
    (void)stop;
    if(!connected)return 4;
    if(output.empty())return 4;
    selected=output[0];
    if(output.size()>1)for(size_t i=1;i<output.size();++i)mem[selected+i-1]=output[i];
    if(output.size()>1)writeCount++;
    return 0;
  }
  uint8_t requestFrom(uint8_t address,uint8_t count) {
    (void)address;readIndex=selected;return connected?count:0;
  }
  int read(){return mem[readIndex++];}
};
''')
    (work/'rtc_test.cpp').write_text('''#include <cassert>
#include <ctime>
#include <cstdlib>
#include "ds3231_clock.h"
int main() {
  TwoWire wire;
  Ds3231Clock rtc(wire);
  time_t timestamp=0;
  assert(!rtc.readUtc(timestamp)); // OSF clear, but no valid date
  assert(rtc.present());
  wire.mem[0x0F]=0x88; // stopped oscillator, 32kHz ON
  assert(!rtc.readUtc(timestamp));
  assert(rtc.present());
  wire.mem[0x0F]=0x08;
  // 2026-10-22 06:02:31 UTC; BCD 26 10 22 06 02 31
  wire.mem[0x00]=0x31;wire.mem[0x01]=0x02;wire.mem[0x02]=0x06;
  wire.mem[0x04]=0x22;wire.mem[0x05]=0x10;wire.mem[0x06]=0x26;
  assert(rtc.readUtc(timestamp));
  struct tm utc={};gmtime_r(&timestamp,&utc);
  assert(utc.tm_year+1900==2026&&utc.tm_mon==9&&utc.tm_mday==22&&utc.tm_hour==6&&utc.tm_min==2&&utc.tm_sec==31);
  // TZ must not affect the UTC representation or the time epoch.
  setenv("TZ","MSK-3",1);tzset();
  time_t again;assert(rtc.readUtc(again)&&again==timestamp);
  wire.mem[0x0E]=0x9C; // EOSC=1, preserve unrelated control bits
  assert(rtc.writeUtc(timestamp+16));
  assert(wire.mem[0x0E]==0x1C);
  assert(wire.mem[0x00]==0x47 && wire.mem[0x0F]==0x08);
  assert(rtc.readUtc(again)&&again==timestamp+16);
  wire.mem[0x01]=0x6A;assert(!rtc.readUtc(again)); // illegal BCD
  wire.mem[0x01]=0x02;
  wire.connected=false;assert(!rtc.readUtc(again)&&!rtc.present());
  wire.connected=true;assert(rtc.readUtc(again)&&rtc.present());
  wire.mem[0x0F]=0x80;assert(!rtc.readUtc(again));
  assert(rtc.writeUtc(timestamp)); // clears OSF only after setting correct date
  assert(!(wire.mem[0x0F]&0x80));
}
''')
    cmd=['g++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(work),'-I',str(root),str(work/'rtc_test.cpp'),'-o',str(work/'rtc_test')]
    subprocess.run(cmd,check=True)
    subprocess.run([str(work/'rtc_test')],check=True)
    print('PASS: DS3231 I2C, OSF, BCD, UTC independent of TZ, write-readback, missing device')
