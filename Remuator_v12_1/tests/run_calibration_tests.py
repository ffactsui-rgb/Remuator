"""Test ADXL345 calibration sequence, interpolation and redundant persistence with mock I2C."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="remuator-cal-") as d:
    out=Path(d)/"test"
    subprocess.run(["g++","-std=c++17","-O1","-Wall","-Wextra","-fsanitize=address,undefined","-fno-omit-frame-pointer","-I",str(root),str(root/"tests/calibration_cases.cpp"),"-o",str(out)],check=True)
    env=os.environ.copy();env["ASAN_OPTIONS"]="detect_leaks=0"
    subprocess.run([str(out)],env=env,check=True)
