@echo off
setlocal
cd /d "%~dp0.."

where cmake >nul 2>nul
if errorlevel 1 (
    echo [EdgeGuard] CMake was not found. Install CMake 3.16+ and a C++17 compiler first.
    exit /b 1
)

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1

cmake --build build --config Release --parallel
if errorlevel 1 exit /b 1

if exist "build\Release\edgeguard.exe" (
    "build\Release\edgeguard.exe" run --config "configs\edgeguard.conf" --with-simulator
) else (
    "build\edgeguard.exe" run --config "configs\edgeguard.conf" --with-simulator
)
