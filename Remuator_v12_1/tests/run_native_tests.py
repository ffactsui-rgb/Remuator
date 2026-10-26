from pathlib import Path
root=Path(__file__).resolve().parent.parent
s=(next(root.glob('*.ino'))).read_text()
names=['programZTargetsSafe','angleToStepsNaklon','stepsToAngleNaklon','scheduledTimeForStep','rebuildRemainingSchedule','beginStepInterval','saveExecution','loadExecution','resetExecution','startProgram','stopProgram','beginNaklonAction','beginPovorotAction','pauseProgram','resumeProgram','executeProgram','finishProgram','logRecoveryPower','resumeRecoveredProgram','serviceRecovery','restoreExecutionAfterBoot','checkEndstops','beginHoming','beginZHome','serviceHoming','canMoveY']
import re
def function(name):
    m=re.search(r'^(?:void|bool|int32_t|float|time_t) '+name+r'\([^\n]*\) \{',s,re.M)
    assert m,name
    start=m.start(); pos=m.end()-1; depth=0; quote=None; escape=False
    while pos<len(s):
        c=s[pos]
        if quote:
            if escape:escape=False
            elif c=='\\':escape=True
            elif c==quote:quote=None
        elif s[pos:pos+2]=='//':
            pos=s.index('\n',pos)
            continue
        elif c in "\"'":quote=c
        elif c=='{':depth+=1
        elif c=='}':
            depth-=1
            if depth==0:return s[start:pos+1]
        pos+=1
    raise AssertionError(name)
funcs=[function(n) for n in names]
types=s[s.index('enum ProgramState {'):s.index('size_t selectedProgram =')]
constants='\n'.join(re.search(r'^(?:const|constexpr) [^\n]* '+name+r' = [^;]+;',s,re.M)[0] for name in ['Y_END_PIN','Y_UPPER_END_PIN','Y_HOME_TRAVEL_STEPS','Y_ENDSTOP_SEARCH_STEPS','Y_LIMIT_SEARCH_TIMEOUT_SEC','RECOVERY_DELAY_US'])
output=(root/'tests/native_prefix.txt').read_text().replace('/* HARDWARE_CONSTANTS */',constants)+'\n'+types+'\nProgramState programState=IDLE;\n'
output+='\n'.join(f[:f.index('{')].strip()+';' for f in funcs)+'\n#include "run_history.h"\n'
output+='\n\n'.join(funcs)+'\n'+(root/'tests/native_cases.txt').read_text()
import subprocess, tempfile, os
with tempfile.TemporaryDirectory(prefix='remuator-tests-') as work:
    cpp=Path(work)/'controller_test.cpp'; binary=Path(work)/'controller_test'
    cpp.write_text(output)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-fsanitize=address,undefined','-g','-I',str(root),str(cpp),'-o',str(binary)],check=True)
    env=os.environ.copy();env['ASAN_OPTIONS']='detect_leaks=0'
    subprocess.run([str(binary)],env=env,check=True)
