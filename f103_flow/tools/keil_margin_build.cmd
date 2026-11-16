@echo off
setlocal enableextensions
set "UV4=D:\keil32\UV4\UV4.exe"
set "PROJ=%~dp0..\MDK-ARM\LDC1614_F103_Margin_STLink.uvprojx"
if not exist "%UV4%" exit /b 1
if not "%~1"=="" (
    echo [ERROR] Build only. Download manually in Keil.
    exit /b 1
)
"%UV4%" -j0 -r "%PROJ%" -t "LDC1614_F103_MARGIN_SW" -o "%~dp0..\MDK-ARM\margin_build.log"
set "RC=%ERRORLEVEL%"
if exist "%~dp0..\MDK-ARM\margin_build.log" type "%~dp0..\MDK-ARM\margin_build.log"
exit /b %RC%
