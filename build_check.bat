@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cd /d "c:\Users\jinhs\Downloads\FBZZ_Engine\build\debug"
ninja -j4 > "c:\Users\jinhs\Downloads\FBZZ_Engine\build_log.txt" 2>&1
echo EXITCODE=%ERRORLEVEL%
