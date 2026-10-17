@echo off
rem Download the build inputs for the installed VLC 3.0.x without a build:
rem the llvm-mingw toolchain, the VLC SDK and the NVENC API headers.
rem Needs no administrator rights.
rem
rem   prepare.cmd [VLC_VERSION]
rem
rem Uses the same environment variables as install.cmd (GIT_BASH,
rem VLC_CHROMECAST_SMOOTH_FIX_WORK, VLC_CHROMECAST_SMOOTH_FIX_SDK, VLC_DIR).
setlocal
if not defined GIT_BASH set "GIT_BASH=%ProgramFiles%\Git\bin\bash.exe"
if not defined VLC_CHROMECAST_SMOOTH_FIX_WORK set "VLC_CHROMECAST_SMOOTH_FIX_WORK=%~dp0.work"
if not exist "%GIT_BASH%" (
    echo ERROR: Git Bash not found: %GIT_BASH%
    echo Install Git for Windows or set GIT_BASH.
    exit /b 1
)
"%GIT_BASH%" "%~dp0scripts\vlc-chromecast-smooth-fix.sh" prepare %1
