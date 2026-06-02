@echo off
setlocal

rem libultrahdr (Ultra HDR export) vendors libjpeg-turbo 3.0.1, whose
rem cmake_minimum_required predates the floor enforced by CMake 4.x. This lets
rem that nested ExternalProject configure (which runs during the build step)
rem accept the older minimum. Harmless for the rest of the build.
set CMAKE_POLICY_VERSION_MINIMUM=3.5

rem The first configure on a clean tree fetches libultrahdr; its libjpeg-turbo
rem dependency isn't detected until the source tree has settled, so the very
rem first configure can fail. A second pass (sources now present) succeeds, so
rem retry once before giving up.
if not exist build (
    cmake -B build -G "Visual Studio 18 2026" -A x64 || ^
    cmake -B build -G "Visual Studio 18 2026" -A x64 || exit /b 1
)

cmake --build build --config Release || exit /b 1

if not exist dist mkdir dist
copy /Y build\Release\sundial.exe dist\sundial.exe >nul || exit /b 1
rem Bundle any runtime DLLs (e.g. the Velopack updater library) next to the exe.
copy /Y build\Release\*.dll dist\ >nul 2>nul

echo.
echo Built: dist\sundial.exe
