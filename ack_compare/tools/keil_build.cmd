@echo off
setlocal enableextensions
set "UV4=D:\keil32\UV4\UV4.exe"
set "PROJ=%~dp0..\MDK-ARM\LDC1614_ACK_Compare.uvprojx"
if not exist "%UV4%" exit /b 1
set "FLAG=-b"
if /i "%~1"=="rebuild" set "FLAG=-r"
if not "%~1"=="" if /i not "%~1"=="rebuild" (
    echo [ERROR] Build only. Download manually in Keil. Argument: rebuild
    exit /b 1
)
"%UV4%" -j0 %FLAG% "%PROJ%" -t "LDC1614_ACK_B_OD3" -o "%~dp0..\MDK-ARM\build_B.log"
set "RC=%ERRORLEVEL%"
if exist "%~dp0..\MDK-ARM\build_B.log" type "%~dp0..\MDK-ARM\build_B.log"
if not "%RC%"=="0" exit /b %RC%
"%UV4%" -j0 %FLAG% "%PROJ%" -t "LDC1614_ACK_A_LEGACY" -o "%~dp0..\MDK-ARM\build_A.log"
set "RC=%ERRORLEVEL%"
if exist "%~dp0..\MDK-ARM\build_A.log" type "%~dp0..\MDK-ARM\build_A.log"
exit /b %RC%
