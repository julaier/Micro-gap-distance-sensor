@echo off
setlocal enableextensions
set "UV4=D:\keil32\UV4\UV4.exe"
set "PROJ=%~dp0..\MDK-ARM\LDC1614_ACK_Test.uvprojx"
set "LOG=%~dp0..\MDK-ARM\build.log"
if not exist "%UV4%" (
    echo [ERROR] Keil not found: %UV4%
    exit /b 1
)
set "FLAG=-b"
if /i "%~1"=="rebuild" set "FLAG=-r"
if /i "%~1"=="flash" (
    echo [ERROR] This helper only builds. Download explicitly in Keil.
    exit /b 1
)
"%UV4%" -j0 %FLAG% "%PROJ%" -o "%LOG%"
set "RC=%ERRORLEVEL%"
if exist "%LOG%" type "%LOG%"
exit /b %RC%
