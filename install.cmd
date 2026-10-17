@echo off
rem Fixes for Chromecast output of VLC 3.0: NVENC encoding, dropped frames,
rem 5.1 audio after a seek, aspect ratio. Builds libchromecast_smooth_fix_plugin.dll
rem for the installed VLC version if it is missing, then installs it as
rem administrator.
rem
rem   install.cmd          build if necessary, then install
rem   install.cmd remove   delete the plugin from VLC
rem
rem Needs Git for Windows. Optional environment variables:
rem   GIT_BASH                 bash.exe of Git for Windows
rem   VLC_CHROMECAST_SMOOTH_FIX_WORK  headers and DLLs (default: .work in this folder)
rem   VLC_CHROMECAST_SMOOTH_FIX_SDK   toolchain and VLC SDK (default: the work folder)
rem   VLC_DIR                  VLC install folder (default: from the registry)
setlocal
set "SCRIPT=%~dp0scripts\vlc-chromecast-smooth-fix.sh"

if /i "%~1"=="--elevated" goto elevated

if not defined GIT_BASH set "GIT_BASH=%ProgramFiles%\Git\bin\bash.exe"
if not defined VLC_CHROMECAST_SMOOTH_FIX_WORK set "VLC_CHROMECAST_SMOOTH_FIX_WORK=%~dp0.work"
if not defined VLC_CHROMECAST_SMOOTH_FIX_SDK set "VLC_CHROMECAST_SMOOTH_FIX_SDK=%VLC_CHROMECAST_SMOOTH_FIX_WORK%"
if not exist "%GIT_BASH%" (
    echo ERROR: Git Bash not found: %GIT_BASH%
    echo Install Git for Windows or set GIT_BASH.
    exit /b 1
)

set "CMD_ARG=install"
if /i "%~1"=="remove" set "CMD_ARG=remove"
if "%CMD_ARG%"=="remove" goto elevate

"%GIT_BASH%" "%SCRIPT%" build
if errorlevel 1 (
    pause
    exit /b 1
)

:elevate
rem An elevated process does not get this environment: give the values as arguments.
rem Start cmd.exe /s /c ""script" args": a .cmd file started directly with quoted
rem arguments loses its first and last quote, and the elevated window fails at once.
set "SELF=%~f0"
powershell -NoProfile -Command "$q = [char]34; $args2 = @($env:VLC_CHROMECAST_SMOOTH_FIX_WORK, $env:VLC_CHROMECAST_SMOOTH_FIX_SDK, $env:GIT_BASH, $env:VLC_DIR | ForEach-Object { $q + $_ + $q }) -join ' '; $line = '/s /c ' + $q + $q + $env:SELF + $q + ' --elevated ' + $env:CMD_ARG + ' ' + $args2 + $q; Start-Process -FilePath cmd.exe -ArgumentList $line -Verb RunAs -Wait"
if errorlevel 1 echo The administrator step was cancelled. Nothing was changed.
exit /b

:elevated
set "VLC_CHROMECAST_SMOOTH_FIX_WORK=%~3"
set "VLC_CHROMECAST_SMOOTH_FIX_SDK=%~4"
set "GIT_BASH=%~5"
set "VLC_DIR=%~6"
rem keep the output: this window closes, and the log helps when the step fails
if not exist "%VLC_CHROMECAST_SMOOTH_FIX_WORK%" mkdir "%VLC_CHROMECAST_SMOOTH_FIX_WORK%"
set "LOG=%VLC_CHROMECAST_SMOOTH_FIX_WORK%\%~2.log"
echo %DATE% %TIME% %* > "%LOG%"
"%GIT_BASH%" "%SCRIPT%" %2 >> "%LOG%" 2>&1
type "%LOG%"
echo.
echo Log: %LOG%
pause
