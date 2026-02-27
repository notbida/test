@echo off
:: build_windows.bat  –  Build script for Windows with Visual Studio 2022
::
:: Prerequisites:
::   - Visual Studio 2022 with "Desktop development with C++" workload
::   - CUDA 11.8 or 12.x  (NVIDIA installer, adds to Program Files)
::   - TensorRT 8.6+ or 9.x  (extract to C:\Program Files\NVIDIA GPU Computing Toolkit\TensorRT)
::   - OBS Studio (installed, or build from source at %OBS_ROOT%)
::   - OpenCV 4.x  (set %OpenCV_DIR% to the cmake config dir)
::   - CMake ≥ 3.22 on PATH
::
:: Usage:
::   build_windows.bat [Release|Debug] [CUDA_ARCH]
::
:: Example:
::   build_windows.bat Release "75;86;89"

setlocal enabledelayedexpansion

set BUILD_TYPE=%1
if "%BUILD_TYPE%"=="" set BUILD_TYPE=Release

set CUDA_ARCH=%2
if "%CUDA_ARCH%"=="" set CUDA_ARCH=75;86;89

set SCRIPT_DIR=%~dp0
set ROOT_DIR=%SCRIPT_DIR%..
set BUILD_DIR=%ROOT_DIR%\build

echo === obs-yolo-tensorrt Windows build ===
echo   Build type : %BUILD_TYPE%
echo   CUDA archs : %CUDA_ARCH%
echo   Build dir  : %BUILD_DIR%
echo.

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
cd /d "%BUILD_DIR%"

cmake "%ROOT_DIR%" ^
  -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
  -DCMAKE_CUDA_ARCHITECTURES="%CUDA_ARCH%"

if %ERRORLEVEL% neq 0 (
  echo CMake configure failed!
  exit /b 1
)

cmake --build . --config %BUILD_TYPE% --parallel

if %ERRORLEVEL% neq 0 (
  echo Build failed!
  exit /b 1
)

echo.
echo === Build succeeded ===
echo   Plugin:   %BUILD_DIR%\%BUILD_TYPE%\obs-yolo-tensorrt.dll
echo   colorbot: %BUILD_DIR%\%BUILD_TYPE%\colorbot.exe
echo.
echo Install plugin:
echo   cmake --install %BUILD_DIR% --config %BUILD_TYPE%
