@echo off
rem ---------------------------------------------------------------------------
rem Keil uVision (UV4) command-line build / flash helper for LDC1614_FW.
rem
rem   keil_build.cmd            same as build
rem   keil_build.cmd build      incremental build
rem   keil_build.cmd rebuild    full rebuild
rem   keil_build.cmd clean      clean outputs
rem   keil_build.cmd flash      build, then download with ST-Link
rem
rem UV4 return code: 0=no errors/warnings, 1=warnings, 2=errors, >=3=other.
rem ---------------------------------------------------------------------------
setlocal enableextensions

set "UV4=D:\keil32\UV4\UV4.exe"
if not exist "%UV4%" (
    echo [ERROR] Keil UV4 not found: %UV4%
    echo         Edit UV4 in tools\keil_build.cmd.
    exit /b 1
)

set "PROJ=%~dp0..\MDK-ARM\LDC1614.uvprojx"
set "LOG=%~dp0..\MDK-ARM\build.log"

if not exist "%PROJ%" (
    echo [ERROR] Keil project not found: %PROJ%
    exit /b 1
)

set "MODE=%~1"
if "%MODE%"=="" set "MODE=build"

if /i "%MODE%"=="flash"   goto :flash
if /i "%MODE%"=="rebuild" goto :rebuild
if /i "%MODE%"=="clean"   goto :clean

:build
set "FLAG=-b"
goto :run

:rebuild
set "FLAG=-r"
goto :run

:clean
set "FLAG=-c"

:run
echo [Keil] UV4 %FLAG% LDC1614.uvprojx
"%UV4%" -j0 %FLAG% "%PROJ%" -o "%LOG%"
set "RC=%ERRORLEVEL%"
if exist "%LOG%" type "%LOG%"
if "%RC%"=="0" goto :build_ok
if "%RC%"=="1" goto :build_warn
echo [FAIL] Build failed. Exit code=%RC%
exit /b %RC%

:build_ok
echo [OK] Build passed. 0 errors, 0 warnings
exit /b 0

:build_warn
echo [WARN] Build completed with warnings. Exit code=%RC%
exit /b 1

:flash
echo [Keil] Building before ST-Link download...
"%UV4%" -j0 -b "%PROJ%" -o "%LOG%"
set "RC=%ERRORLEVEL%"
if exist "%LOG%" type "%LOG%"
if not "%RC%"=="0" (
    echo [FAIL] Build failed; download skipped. Exit code=%RC%
    exit /b %RC%
)
"%UV4%" -j0 -f "%PROJ%" -o "%LOG%"
set "RC=%ERRORLEVEL%"
if exist "%LOG%" type "%LOG%"
if "%RC%"=="0" (
    echo [OK] ST-Link download complete
) else (
    echo [FAIL] Download failed. Exit code=%RC% - check ST-Link wiring and Debug settings
)
exit /b %RC%
