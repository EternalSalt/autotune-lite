@echo off
rem  AutoTune Lite - builds the plugin and wraps it in a Windows installer.
rem  Double-click this file (or run it from a command prompt) from the project folder.
rem  Needs: Visual Studio 2022 (Desktop development with C++), CMake, Git, Inno Setup 6.3+.
setlocal
cd /d "%~dp0"

echo.
echo === AutoTune Lite installer builder ===
echo.

where cmake >nul 2>nul
if errorlevel 1 goto :nocmake
where git >nul 2>nul
if errorlevel 1 goto :nogit

set "ISCC="
if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if not defined ISCC goto :noinno

echo [1/3] Configuring (downloads JUCE the first time, needs internet)...
cmake -B build -G "Visual Studio 17 2022" -A x64
if errorlevel 1 goto :fail

echo.
echo [2/3] Building the plugin (a few minutes)...
cmake --build build --config Release
if errorlevel 1 goto :fail

if not exist "build\AutoTuneLite_artefacts\Release\VST3\AutoTune Lite.vst3" goto :novst3

echo.
echo [3/3] Creating the installer...
"%ISCC%" installer\AutoTuneLite.iss
if errorlevel 1 goto :fail

echo.
echo ============================================================
echo  DONE.  Your installer is in:
echo    %~dp0installer\Output\
echo  Run AutoTuneLite-Setup-*.exe to install into FL Studio.
echo ============================================================
if "%CI%"=="" pause
exit /b 0

:nocmake
echo CMake was not found. Install it from https://cmake.org/download and choose "Add CMake to the system PATH".
goto :fail
:nogit
echo Git was not found. Install it from https://git-scm.com
goto :fail
:noinno
echo Inno Setup 6 was not found. Install it from https://jrsoftware.org/isdl.php
goto :fail
:novst3
echo The build finished but the plugin folder was not where expected:
echo   build\AutoTuneLite_artefacts\Release\VST3\AutoTune Lite.vst3
goto :fail

:fail
echo.
echo *** FAILED - scroll up for the error text and send it to me. ***
if "%CI%"=="" pause
exit /b 1
