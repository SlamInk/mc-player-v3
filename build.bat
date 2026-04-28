@echo off
REM mc-player build helper.
REM Usage: build.bat [Debug|RelWithDebInfo|Release]

setlocal EnableDelayedExpansion
set CFG=%~1
if "%CFG%"=="" set CFG=RelWithDebInfo

REM put vswhere on PATH so vcvars64.bat can find it
set "VSWHEREDIR=C:\Program Files (x86)\Microsoft Visual Studio\Installer"
if exist "%VSWHEREDIR%\vswhere.exe" set "PATH=%VSWHEREDIR%;%PATH%"

set "VSROOT=C:\Program Files\Microsoft Visual Studio\18\Enterprise"
set "VCVARS=%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo [build.bat] vcvars64.bat not found at %VCVARS%
    exit /b 1
)
call "%VCVARS%"
if errorlevel 1 (
    echo [build.bat] vcvars64.bat failed
    exit /b 1
)

set "CMAKE_EXE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA_EXE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

if not exist "%CMAKE_EXE%" (
    echo [build.bat] cmake.exe not found at %CMAKE_EXE%
    exit /b 1
)

set "BUILD_DIR=%~dp0build\ninja-msvc-%CFG%"
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

set "PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"

"%CMAKE_EXE%" -S "%~dp0." -B "%BUILD_DIR%" -G Ninja "-DCMAKE_MAKE_PROGRAM=%NINJA_EXE%" "-DCMAKE_BUILD_TYPE=%CFG%" -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
if errorlevel 1 exit /b 1

"%CMAKE_EXE%" --build "%BUILD_DIR%" --target mc_player_demo
exit /b %errorlevel%