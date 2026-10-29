"""Static guards against axis mapping, Serial blocking, and TMC preset regressions."""
from pathlib import Path
import re
root = Path(__file__).resolve().parent.parent
ino = next(root.glob('*.ino')).read_text(encoding='utf-8')
cfg = (root / 'tmc_config.h').read_text(encoding='utf-8')
html = (root / 'data/uart.html').read_text(encoding='utf-8')
js = (root / 'data/uart.js').read_text(encoding='utf-8')

for name, pin in {'Y_DIR':4, 'Y_STEP':5, 'Y_ENABLE':6,
                  'Z_DIR':42, 'Z_STEP':41, 'Z_ENABLE':40,
                  'Y_END':14, 'Y_UPPER_END':13,
                  'TMC_UART_RX':47, 'TMC_UART_TX':21,
                  'I2C_SDA':2, 'I2C_SCL':1}.items():
    assert re.search(rf'constexpr uint8_t {name}_PIN\s*=\s*{pin}\s*;', ino) if name not in ('TMC_UART_RX','TMC_UART_TX') else re.search(rf'constexpr uint8_t {name}\s*=\s*{pin}\s*;', ino), name
assert 'GStepper<STEPPER2WIRE> motor_Naklon(200, Y_STEP_PIN, Y_DIR_PIN);' in ino
assert 'GStepper<STEPPER2WIRE> motor_Povorot(200, Z_STEP_PIN, Z_DIR_PIN);' in ino
assert 'constexpr bool HARDWARE_BUTTONS_ENABLED = false;' in ino
assert 'motor_Naklon.getCurrent() != motor_Naklon.getTarget()' in ino
assert 'motor_Povorot.getCurrent() != motor_Povorot.getTarget()' in ino
assert '#define DBG(x) do { if (!stepMotionPending()) { captureStartup(x); if (serialOutputEnabled) Serial.print(x); } } while (0)' in ino
assert re.search(r'#define DBGLN\(x\) do \{ if \(!stepMotionPending\(\)\) \{ captureStartup\(x\);.*if \(serialOutputEnabled\) Serial.println\(x\);', ino)
assert 'if (serialOutputEnabled && !zCalStarted && !stepMotionPending())\n      handleSerial();' in ino
assert 'motor_Povorot.getCurrent()==motor_Povorot.getTarget() &&' in ino
assert '!motor_Naklon.getState() && !motor_Povorot.getState()) delay(1);' in ino
assert 'path != "/api/calibration/y"' in ino
assert 'if (!stepMotionPending()) {\n      pollTimeSync();\n      serviceRtcClock();' in ino
assert 'if (!stepMotionPending()) {\n      setConnectionLED();\n      oledShowStatus();' in ino
for part in ('bool stealthChop = false', 'uint16_t rmsCurrent = 900',
             'uint8_t toff = 5', 'int8_t hysteresisEnd = 0',
             'uint16_t microsteps = 16', 'bool intpol = true'):
    assert part in cfg, part
assert 'TmcUartSettings uartNaklon{TmcModel::Tmc2208, 0};' in ino
assert 'TmcUartSettings uartPovorot{TmcModel::Tmc2209, 1};' in ino
assert html.count('data-torque-profile') == 2
assert 'const torqueProfile =' in js
print('PASS: swapped axis GPIO and safety pins, nonblocking Serial, motion fast-path, TMC torque preset and UI')
