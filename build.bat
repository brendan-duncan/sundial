@echo off
setlocal

if not exist build (
    cmake -B build -G "Visual Studio 18 2026" -A x64 || exit /b 1
)

cmake --build build --config Release || exit /b 1

if not exist dist mkdir dist
copy /Y build\Release\sundial.exe dist\sundial.exe >nul || exit /b 1
rem Bundle any runtime DLLs (e.g. the Velopack updater library) next to the exe.
copy /Y build\Release\*.dll dist\ >nul 2>nul

echo.
echo Built: dist\sundial.exe
