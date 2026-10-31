"""Проверка последовательности калибровки ADXL345, интерполяции и резервного хранения с имитацией I2C."""
from pathlib import Path
import os, subprocess, tempfile, re
root=Path(__file__).resolve().parent.parent
source=next(root.glob('*.ino')).read_text()
start=source.index('void checkEndstops() {');end=source.index('\n}\n',start)+2
cases=(root/'tests/calibration_cases.cpp').read_text().replace('/* REAL_ENDSTOPS: настоящий обработчик концевиков для тестовой среды. */',source[start:end])
with tempfile.TemporaryDirectory(prefix="remuator-cal-") as d:
    out=Path(d)/"test"
    cpp=Path(d)/"test.cpp";cpp.write_text(cases)
    subprocess.run(["g++","-std=c++17","-O1","-Wall","-Wextra","-fsanitize=address,undefined","-fno-omit-frame-pointer","-I",str(root),str(cpp),"-o",str(out)],check=True)
    env=os.environ.copy();env["ASAN_OPTIONS"]="detect_leaks=0"
    subprocess.run([str(out)],env=env,check=True)
