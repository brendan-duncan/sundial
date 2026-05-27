@echo off
setlocal
call "%~dp0build.bat" || exit /b 1
start "" .\dist\sundial.exe
