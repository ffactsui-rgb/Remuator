"""Compile actual UART handlers with in-memory serial/NVS/HTTP doubles; no hardware."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
root = Path(__file__).resolve().parent.parent
source = (next(root.glob('*.ino'))).read_text()
names = ['normalizeTmcBusSettings', 'reportTmcConnection', 'configureTmcDrivers', 'initTMC', 'applyTmcSettingsVerified',
         'loadTmcSettings', 'saveTmcSettings', 'applyTmcSettings', 'handleUartSettingsSave']

def function(name):
    match = re.search(r'^(?:void|bool) ' + name + r'\([^\n]*\) \{', source, re.M)
    assert match, name
    pos = match.end() - 1
    depth = 0
    quote = None
    escape = False
    while pos < len(source):
        c = source[pos]
        if quote:
            if escape:
                escape = False
            elif c == '\\':
                escape = True
            elif c == quote:
                quote = None
        elif source[pos:pos + 2] == '//':
            pos = source.index('\n', pos)
            continue
        elif c in "\"'":
            quote = c
        elif c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return source[match.start():pos + 1]
        pos += 1
    raise AssertionError(name)

functions = [function(n) for n in names]
code = (root / 'tests/uart_prefix.txt').read_text() + '\n'
code += '\n'.join(f[:f.index('{')].strip() + ';' for f in functions) + '\n'
code += '\n'.join(functions) + '\n' + (root / 'tests/uart_cases.txt').read_text()
with tempfile.TemporaryDirectory(prefix='remuator-uart-tests-') as tmp:
    tmp = Path(tmp)
    (tmp / 'TMCStepper.h').write_text((root / 'tests/uart_mock.txt').read_text())
    (tmp / 'uart_test.cpp').write_text(code)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Wno-unused-parameter',
                    '-fsanitize=address,undefined', '-g', '-I', str(tmp), '-I', str(root),
                    str(tmp / 'uart_test.cpp'), '-o', str(tmp / 'uart_test')], check=True)
    env = os.environ.copy()
    env['ASAN_OPTIONS'] = 'detect_leaks=0'
    subprocess.run([str(tmp / 'uart_test')], env=env, check=True)
