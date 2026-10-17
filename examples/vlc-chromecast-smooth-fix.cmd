@echo off
rem Example launcher: keep the toolchain and the VLC SDK in a shared SDK folder
rem and the build data outside the repository. Copy this file to a folder in
rem your PATH and change the three paths below.
rem
rem   vlc-chromecast-smooth-fix.cmd          build if necessary, then install
rem   vlc-chromecast-smooth-fix.cmd remove   delete the plugin from VLC
setlocal
set "VLC_CHROMECAST_SMOOTH_FIX_REPO=C:\src\vlc-chromecast-smooth-fix"
set "VLC_CHROMECAST_SMOOTH_FIX_SDK=C:\sdk"
set "VLC_CHROMECAST_SMOOTH_FIX_WORK=C:\sdk\vlc-chromecast-smooth-fix"
call "%VLC_CHROMECAST_SMOOTH_FIX_REPO%\install.cmd" %*
