"""Static guard for Arduino-generated prototype type visibility."""
from pathlib import Path
root=Path(__file__).resolve().parent.parent
s=next(root.glob('*.ino')).read_text(encoding='utf-8')
h=(root/'RemuatorCompileDeps.h').read_text(encoding='utf-8')
assert s.index('#include "RemuatorCompileDeps.h"') < s.index('SET_LOOP_TASK_STACK_SIZE')
assert s.index('#include "RemuatorCompileDeps.h"') < s.index('void onWiFiEvent(')
assert s.count('SET_LOOP_TASK_STACK_SIZE(16 * 1024)') == 1
assert s.count('String fmArg(AsyncWebServerRequest *r, const char *key, bool post = false);') == 1
assert s.count('String fmArg(AsyncWebServerRequest *r, const char *key, bool post) {') == 1
for header in ('Arduino.h','WiFi.h','TMCStepper.h','ESPAsyncWebServer.h','tmc_axis.h','tmc_config.h','program_model.h'):
    assert header in h, header
for signature in ('void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info);',
                  'void reportTmcConnection(TmcAxis &axis, const char *name, const TmcUartSettings &settings);',
                  'void loadTmcSettings(TmcUartSettings &c, const char *x);',
                  'void applyTmcSettings(TMC2208Stepper &d, const TmcUartSettings &c);',
                  'bool fmAuthenticated(AsyncWebServerRequest *r);'):
    assert signature in s, signature
print('PASS: deps first, types declared, explicit Arduino function prototypes, stack size')
