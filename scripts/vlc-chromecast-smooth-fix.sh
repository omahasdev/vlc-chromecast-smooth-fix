#!/bin/sh
# Build and install libchromecast_smooth_fix_plugin.dll: fixes for Chromecast
# output of VLC 3.0 (NVENC encoding, video DTS, Vorbis after seek, aspect).
#
# Usage (Git Bash):
#   vlc-chromecast-smooth-fix.sh prepare [VLC_VERSION]  download toolchain, VLC SDK, headers
#   vlc-chromecast-smooth-fix.sh build   [VLC_VERSION]  build the DLL if it is missing or old
#   vlc-chromecast-smooth-fix.sh install [VLC_VERSION]  copy the DLL into VLC (admin)
#   vlc-chromecast-smooth-fix.sh remove  [VLC_VERSION]  delete the DLL from VLC (admin)
# Without VLC_VERSION, the version of the installed vlc.exe is used.
#
# Environment:
#   VLC_CHROMECAST_SMOOTH_FIX_WORK  folder for the headers and the DLLs
#                                   (default: .work in the repository)
#   VLC_CHROMECAST_SMOOTH_FIX_SDK   folder for the toolchain and the VLC SDK
#                                   (default: the work folder). It can be
#                                   the same folder as for vlc-mdns-windows-fix.
#   VLC_DIR                         VLC install folder (default: from the registry)
#
# Each download is verified before use:
#   toolchain        pinned SHA-256 (TOOLCHAIN_SHA256)
#   VLC 7z (SDK)     SHA-256 file from the VideoLAN main server
#   nv-codec-headers pinned SHA-256 (NVH_SHA256)
# The NVIDIA driver supplies nvEncodeAPI64.dll and nvcuda.dll at run time;
# nothing from NVIDIA is linked.
#
# The build is reproducible: source paths in the DLL are relative and the PE
# header has no time stamp, so the same commit gives the same SHA-256.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(dirname "$HERE")
WORK=$(cygpath -u "${VLC_CHROMECAST_SMOOTH_FIX_WORK:-$REPO/.work}")
SDK_ROOT=$(cygpath -u "${VLC_CHROMECAST_SMOOTH_FIX_SDK:-$WORK}")
TOOLCHAIN_TAG=20260922
TOOLCHAIN_NAME=llvm-mingw-$TOOLCHAIN_TAG-msvcrt-x86_64   # msvcrt, the C runtime of VLC
TOOLCHAIN_SHA256=1e936a4a694fc27f9625e3311f5ec5d6d99abfeaa514fd41c52a4f8347145d1a
TOOLCHAIN=$SDK_ROOT/$TOOLCHAIN_NAME
CC=x86_64-w64-mingw32-clang
WIN_TARGET=0x0601   # Windows 7 API level, as VLC 3.0
WIN_TAR=/c/Windows/System32/tar.exe   # bsdtar: reads .zip, .7z and .tar.gz
VLC_DOWNLOAD=https://download.videolan.org/pub/videolan/vlc   # main server, not a mirror
NVH_VER=12.1.14.0   # NVENC API 12.1: NVIDIA driver 531 or newer
NVH_SHA256=62b30ab37e4e9be0d0c5b37b8fee4b094e38e570984d56e1135a6b6c2c164c9f
NVH_DIR=$WORK/nv-codec-headers-$NVH_VER
MODULE=chromecast_smooth_fix
PLUGIN=lib${MODULE}_plugin.dll
LEGACY_PLUGIN=libnvenc_plugin.dll   # the first name of this plugin

die() { echo "ERROR: $*" >&2; exit 1; }
say() { echo "== $*"; }

export PATH="$TOOLCHAIN/bin:$PATH"

# ---------------------------------------------------------------- VLC install
vlc_dir() {
    if [ -n "$VLC_DIR" ]; then cygpath -u "$VLC_DIR"; return; fi
    d=$(powershell -NoProfile -Command \
        "(Get-ItemProperty 'HKLM:\SOFTWARE\VideoLAN\VLC' -ErrorAction SilentlyContinue).InstallDir" | tr -d '\r')
    [ -n "$d" ] || d="$PROGRAMFILES\\VideoLAN\\VLC"
    cygpath -u "$d"
}

vlc_version() {
    [ -f "$(vlc_dir)/vlc.exe" ] || die "vlc.exe not found in $(vlc_dir)"
    exe=$(cygpath -w "$(vlc_dir)/vlc.exe")
    powershell -NoProfile -Command "(Get-Item '$exe').VersionInfo.FileVersion" | tr -d '\r'
}

# ---------------------------------------------------------------- helpers
extract() {   # extract ARCHIVE DEST [tar options...]
    archive=$1; dest=$2; shift 2
    [ -x "$WIN_TAR" ] || die "Windows tar.exe not found: $WIN_TAR"
    mkdir -p "$dest"
    "$WIN_TAR" -xf "$(cygpath -w "$archive")" -C "$(cygpath -w "$dest")" "$@"
}

download() {   # download URL FILE (HTTPS only, also for redirects)
    say "download $1"
    curl -fsSL --proto '=https' --proto-redir '=https' --retry 3 -o "$2" "$1" || \
        { rm -f "$2"; die "download failed: $1"; }
}

verify() {   # verify FILE sha256 EXPECTED_HEX
    expected=$(echo "$3" | tr 'A-F' 'a-f')
    [ -n "$expected" ] || die "no $2 hash for $(basename "$1")"
    actual=$("${2}sum" "$1" | cut -d' ' -f1)
    if [ "$actual" != "$expected" ]; then
        rm -f "$1"
        die "$2 mismatch for $(basename "$1"): got $actual, expected $expected"
    fi
    say "$2 ok: $(basename "$1")"
}

entry_points() {   # the vlc_entry__* ABI names exported by a plugin DLL
    llvm-objdump -p "$1" | grep -o 'vlc_entry[a-z_]*__[0-9a-z_]*' | sort -u
}

# ---------------------------------------------------------------- inputs
ensure_toolchain() {
    [ -x "$TOOLCHAIN/bin/$CC.exe" ] && return
    zip="$SDK_ROOT/$TOOLCHAIN_NAME.zip"
    mkdir -p "$SDK_ROOT"
    download "https://github.com/mstorsjo/llvm-mingw/releases/download/$TOOLCHAIN_TAG/$TOOLCHAIN_NAME.zip" "$zip"
    verify "$zip" sha256 "$TOOLCHAIN_SHA256"
    extract "$zip" "$SDK_ROOT"
    rm -f "$zip"
    [ -x "$TOOLCHAIN/bin/$CC.exe" ] || die "toolchain not usable: $TOOLCHAIN"
}

ensure_sdk() {   # VLC plugin headers and libvlccore import library
    SDK=$SDK_ROOT/vlc-$VER/sdk
    [ -f "$SDK/lib/libvlccore.lib" ] && return
    name=vlc-$VER-win64.7z
    pkg="$SDK_ROOT/vlc-$VER/$name"
    mkdir -p "$SDK_ROOT/vlc-$VER"
    download "$VLC_DOWNLOAD/$VER/win64/$name.sha256" "$pkg.sha256"
    download "$VLC_DOWNLOAD/$VER/win64/$name" "$pkg"
    verify "$pkg" sha256 "$(grep "$name" "$pkg.sha256" | cut -d' ' -f1)"
    if ! extract "$pkg" "$SDK_ROOT/vlc-$VER" --strip-components=1 "vlc-$VER/sdk/*"; then
        # older Windows tar.exe cannot read .7z: use 7-Zip if it is installed
        command -v 7z >/dev/null 2>&1 || die "cannot extract $name: install 7-Zip"
        tmp="$SDK_ROOT/vlc-$VER/7z-tmp"
        7z x -y -bd -o"$(cygpath -w "$tmp")" "$(cygpath -w "$pkg")" "vlc-$VER/sdk/*" >/dev/null
        mv "$tmp/vlc-$VER/sdk" "$SDK_ROOT/vlc-$VER/sdk"
        rm -rf "$tmp"
    fi
    rm -f "$pkg" "$pkg.sha256"
    [ -f "$SDK/lib/libvlccore.lib" ] || die "no sdk/lib/libvlccore.lib in $name"
}

ensure_headers() {   # NVENC API headers (FFmpeg nv-codec-headers, MIT)
    [ -f "$NVH_DIR/include/ffnvcodec/nvEncodeAPI.h" ] && return
    mkdir -p "$WORK"
    tgz=$WORK/nv-codec-headers-$NVH_VER.tar.gz
    download "https://github.com/FFmpeg/nv-codec-headers/releases/download/n$NVH_VER/nv-codec-headers-$NVH_VER.tar.gz" "$tgz"
    verify "$tgz" sha256 "$NVH_SHA256"
    extract "$tgz" "$WORK"
    rm -f "$tgz"
}

# ---------------------------------------------------------------- build
build_id() {   # hash of everything in this repository that changes the DLL
    cat "$REPO"/src/*.c "$REPO"/src/*.h "$REPO/build/vlc_module_prelude.h" "$0" | sha256sum | cut -d' ' -f1
}

prepare() {
    ensure_toolchain
    ensure_sdk
    ensure_headers
    say "toolchain: $TOOLCHAIN"
    say "VLC SDK:   $SDK"
    say "headers:   $NVH_DIR"
}

build() {
    out="$WORK/out/$VER/$PLUGIN"
    stamp="$WORK/out/$VER/build.id"
    id=$(build_id)
    if [ -f "$out" ] && [ "$(cat "$stamp" 2>/dev/null)" = "$id" ]; then
        say "DLL for VLC $VER is up to date: $out"
        return
    fi
    prepare

    mkdir -p "$WORK/out/$VER"
    say "build $PLUGIN for VLC $VER"
    # relative paths in the DLL: no user folders, the same DLL on each PC
    maps="-ffile-prefix-map=$(cygpath -m "$REPO")=repo -ffile-prefix-map=$(cygpath -m "$SDK")=vlc-sdk"
    maps="$maps -ffile-prefix-map=$(cygpath -m "$NVH_DIR")=nv-codec-headers"
    maps="$maps -ffile-prefix-map=$(cygpath -m "$TOOLCHAIN")=llvm-mingw"
    (cd "$REPO" && $CC -O2 -Wall -Wextra -Wno-unused-parameter $maps -shared \
        -o "$out" src/*.c \
        -include build/vlc_module_prelude.h \
        -D__PLUGIN__ -DMODULE_NAME=$MODULE -DMODULE_STRING=\"$MODULE\" -D_FILE_OFFSET_BITS=64 \
        -D_WIN32_WINNT=$WIN_TARGET -DWINVER=$WIN_TARGET -DUNICODE -D_UNICODE \
        -I"$SDK/include" -I"$SDK/include/vlc/plugins" -I"$NVH_DIR/include" \
        "$SDK/lib/libvlccore.lib" \
        -s -Wl,--no-insert-timestamp)
    entry_points "$out" | grep -q "__3_0_0" || die "the DLL has no VLC 3.0 entry point"
    echo "$id" > "$stamp"
    say "built: $out"
    say "sha256: $(sha256sum "$out" | cut -d' ' -f1)"
}

# ---------------------------------------------------------------- install
require_admin() {
    net session >/dev/null 2>&1 || die "run as administrator"
    if MSYS_NO_PATHCONV=1 tasklist.exe /NH /FI "IMAGENAME eq vlc.exe" | grep -qi vlc.exe; then
        die "close all VLC windows first"
    fi
}

refresh_cache() {
    VLC=$(vlc_dir)
    rm -f "$VLC/plugins/plugins.dat"
    "$VLC/vlc-cache-gen.exe" "$(cygpath -w "$VLC/plugins")" || die "vlc-cache-gen failed"
    say "plugin cache refreshed"
}

install() {
    require_admin
    VLC=$(vlc_dir)
    target="$VLC/plugins/codec/$PLUGIN"
    ours="$WORK/out/$VER/$PLUGIN"
    [ -f "$ours" ] || die "no DLL for VLC $VER, run the build first"
    [ -x "$TOOLCHAIN/bin/llvm-objdump.exe" ] || die "toolchain missing, run the build first"
    [ "$(entry_points "$ours")" = "$(entry_points "$VLC/plugins/codec/libx264_plugin.dll")" ] || \
        die "plugin ABI differs from the installed VLC, delete $ours and build again"

    legacy="$VLC/plugins/codec/$LEGACY_PLUGIN"
    if [ -f "$legacy" ]; then
        # it registers the same modules: VLC must not load two copies
        rm -f "$legacy"
        say "removed the old plugin: $legacy"
    elif cmp -s "$ours" "$target"; then
        say "this DLL is already installed"
        return
    fi
    cp "$ours" "$target"
    say "installed: $target"
    say "sha256:    $(sha256sum "$target" | cut -d' ' -f1)"
    refresh_cache
}

remove() {
    require_admin
    found=
    for name in "$PLUGIN" "$LEGACY_PLUGIN"; do
        target="$(vlc_dir)/plugins/codec/$name"
        [ -f "$target" ] || continue
        rm -f "$target"
        say "removed: $target"
        found=1
    done
    if [ -z "$found" ]; then say "not installed"; return; fi
    refresh_cache
}

# ---------------------------------------------------------------- main
cmd=${1:-build}
VER=${2:-$(vlc_version)}
case "$VER" in
    3.0.*) ;;
    *) die "VLC $VER is not supported: this plugin is for VLC 3.0.x" ;;
esac
SDK=$SDK_ROOT/vlc-$VER/sdk
say "VLC $VER"

case "$cmd" in
    prepare) prepare ;;
    build) build ;;
    install) install ;;
    remove) remove ;;
    *) die "unknown command: $cmd (prepare, build, install, remove)" ;;
esac
