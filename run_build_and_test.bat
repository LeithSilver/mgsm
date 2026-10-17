@echo off
cd /d "%~dp0"
echo === BUILD === > build_and_test.log
call mgs_native\build.bat >> build_and_test.log 2>&1
if not exist mgs_mobile.exe goto done
echo === TEST === >> build_and_test.log
python test_splash_progression.py >> build_and_test.log 2>&1
:done
echo === FINISHED === >> build_and_test.log
