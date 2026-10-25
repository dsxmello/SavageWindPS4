#!/usr/bin/env bash
# Cross-build SDL3 for the PS4 as a static library: the PS5 port's SDL3 (dusklight-ps5 tag deps/sdl3-ps5) with
# dusklight-ps4's patches/sdl3-ps4.patch, the options of dusklight-ps4's tools/build-sdl3.sh. No window system or GPU
# API (Vulkan goes straight to RADV), SceAudioOut audio, scePad through virtual gamepads (runtime/hle/input.c).
#   build-sdl3.sh [setup]   (setup: reconfigure from scratch)
set -euo pipefail
source "$(dirname "$0")/env.sh"
SRC="$SW_PS4_BUILD/src/sdl3"
BUILD="$SW_PS4_BUILD/build/sdl3"
PATCH="${DUSKLIGHT_PS4:-$HOME/Projetos/triaevum/refs/dusklight-ps4-main}/patches/sdl3-ps4.patch"

if [ ! -d "$SRC/.git" ]; then
    git clone -q --depth 1 --branch deps/sdl3-ps5 https://github.com/mshivam019/dusklight-ps5 "$SRC"
    git -C "$SRC" apply "$PATCH"
fi
if [ "${1:-}" = setup ] || [ ! -f "$BUILD/build.ninja" ]; then
    rm -rf "$BUILD"
    env -u CFLAGS -u CXXFLAGS -u LDFLAGS cmake -S "$SRC" -B "$BUILD" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$SW_ROOT/ps4/toolchain-freebsd.cmake" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG -march=btver2" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF \
        -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF \
        -DSDL_AUDIO=ON -DSDL_JOYSTICK=ON -DSDL_HAPTIC=ON -DSDL_SENSOR=ON -DSDL_HIDAPI=OFF \
        -DSDL_DUMMYAUDIO=OFF -DSDL_DISKAUDIO=OFF -DSDL_CAMERA=OFF -DSDL_DIALOG=OFF -DSDL_TRAY=OFF \
        -DSDL_VULKAN=OFF -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF \
        -DLIBC_HAS_WCSNLEN=OFF -DLIBC_HAS_WCSLCPY=OFF -DLIBC_HAS_WCSLCAT=OFF \
        -DSDL_DLOPEN_NOTES=OFF -DSDL_PTHREADS=ON -DHAVE_LINUX_INPUT_H=OFF
fi
cmake --build "$BUILD" --target SDL3-static --parallel "${JOBS:-$(nproc)}"
