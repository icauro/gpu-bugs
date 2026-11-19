@echo off
rem Builds every case from a plain shell: finds Visual Studio with vswhere, sets up its x64
rem environment, then configures and builds with CMake and Ninja (MSVC cl; any C17 compiler works).
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (echo Visual Studio with the C++ tools not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist .builds\default\build.ninja cmake --preset default || exit /b 1
cmake --build .builds\default
