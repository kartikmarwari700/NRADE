@echo off
rem Windows launcher (needs MSYS2 with its "gcc" and "make" packages).
rem   run.bat          open the menu
rem   run.bat demo     step by step teacher demo
rem   run.bat test     run all test cases
setlocal
set "MSYSBASH=C:\msys64\usr\bin\bash.exe"
if not exist "%MSYSBASH%" (
    echo MSYS2 not found at C:\msys64. See README.md.
    exit /b 1
)
set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=run"
cd /d "%~dp0"
set "CHERE_INVOKING=1"
set "MSYSTEM=MSYS"
"%MSYSBASH%" -lc "make -s %TARGET%"
