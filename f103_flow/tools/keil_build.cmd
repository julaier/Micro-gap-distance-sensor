@echo off
setlocal enableextensions
set "UV4=D:\keil32\UV4\UV4.exe"
set "PROJ=%~dp0..\MDK-ARM\LDC1614_F103_Flow.uvprojx"
if not exist "%UV4%" exit /b 1
if not "%~1"=="" (
    echo [ERROR] Build only. No arguments accepted. Download manually in Keil.
    exit /b 1
)
"%UV4%" -j0 -r "%PROJ%" -t "LDC1614_F103_FLOW_SW" -o "%~dp0..\MDK-ARM\build.log"
set "RC=%ERRORLEVEL%"
if exist "%~dp0..\MDK-ARM\build.log" type "%~dp0..\MDK-ARM\build.log"
exit /b %RC%
