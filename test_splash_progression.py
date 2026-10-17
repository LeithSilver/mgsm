import os
import subprocess
import sys

# Usage: python test_splash_progression.py [steps=6] [interval_s=2] [key_every=1]
# Runs mgs_mobile.exe with MGS_AUTOTEST: the exe itself presses the keys and saves splash_N.png (the presented
# frame) every interval, then closes (autotest.cpp). Same on every platform; nothing here is Windows-specific.
#   MGS_EXE_ARGS       extra exe arguments, space separated
#   MGS_KEY_SCRIPT     "step:vk[/hold_s],..." hex Windows VK codes, replacing that step's Enter (e.g. "10:58,13:0d")
#   MGS_KEY_HOLD       default hold time (0.1 s)
#   MGS_RESIZE_SCRIPT  "step:WxH,..." window client size before a step's screenshot
#   MGS_EXE            the executable (default: mgs_mobile.exe on Windows, ./mgs_mobile elsewhere)
STEPS = int(sys.argv[1]) if len(sys.argv) > 1 else 6
INTERVAL = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0
KEY_EVERY = int(sys.argv[3]) if len(sys.argv) > 3 else 1
EXE = os.environ.get('MGS_EXE', 'mgs_mobile.exe' if os.name == 'nt' else './mgs_mobile')
EXE_ARGS = os.environ.get('MGS_EXE_ARGS', '').split()

env = dict(os.environ, MGS_AUTOTEST=f'{STEPS},{INTERVAL},{KEY_EVERY}')
timeout = 2 + STEPS * (INTERVAL + 1) + 30
with open('test_stdout.log', 'w') as out_f, open('test_stderr.log', 'w') as err_f:
    p = subprocess.Popen([EXE] + EXE_ARGS, stdout=out_f, stderr=err_f, env=env)
    print(f"PID: {p.pid}")
    try:
        p.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        print("Timed out, killing the game")
        p.kill()
        p.wait()

with open('test_stdout.log', errors='replace') as f:
    for line in f:
        if line.startswith('[AutoTest] Saved: ') or line.startswith('[AutoTest] Failed'):
            print(line[len('[AutoTest] '):].rstrip())
print(f"Exit code: {p.returncode}")
print("Completed splash progression test.")
