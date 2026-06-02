@echo off
setlocal

rem libultrahdr (Ultra HDR export) vendors libjpeg-turbo 3.0.1, and libavif's
rem LOCAL aom (HDR AVIF) is older still - both have a cmake_minimum_required
rem that predates the floor enforced by CMake 4.x. This lets those nested
rem ExternalProject configures (which run during the build step) accept the
rem older minimum. Harmless for the rest of the build.
set CMAKE_POLICY_VERSION_MINIMUM=3.5

rem The first configure on a clean tree fetches libultrahdr / libavif; their
rem bundled dependencies (libjpeg-turbo, aom) aren't detected until the source
rem tree has settled, so the very first configure can fail. A second pass
rem (sources now present) succeeds, so retry once before giving up.
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
