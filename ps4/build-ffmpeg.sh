#!/usr/bin/env bash
# Cross-build FFmpeg for the PS4 (cutscene videos, runtime/hle/movie.c): static, LGPL, only what the game's videos
# need (as tools/build_deps_windows.sh: file protocol, mov / matroska, h264 / vp9 / vp8, aac / vorbis / opus, swscale),
# compiled with DolphinPS4's toolchain wrapper (toolchain/bin/ps4-cc) for the Jaguar. Installs to $SW_PS4_BUILD/ffmpeg.
set -euo pipefail
source "$(dirname "$0")/env.sh"
V=9.0.2
SHA=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
# (FFmpeg's configure refuses a path with spaces: then it builds through a temporary link without them)
if [[ "$SW_PS4_BUILD" == *" "* ]]; then
    LINK="$(mktemp -d)/b"
    ln -s "$SW_PS4_BUILD" "$LINK"
    trap 'rm -rf "$(dirname "$LINK")"' EXIT
    SW_PS4_BUILD="$LINK"
fi
DL="$SW_PS4_BUILD/downloads/ffmpeg-$V.tar.xz"
SRC="$SW_PS4_BUILD/src/ffmpeg-$V"
BUILD="$SW_PS4_BUILD/build/ffmpeg"
PREFIX="$SW_PS4_BUILD/ffmpeg"

[ -f "$DL" ] || curl -fsSL -o "$DL.part" "https://ffmpeg.org/releases/ffmpeg-$V.tar.xz" && { [ -f "$DL" ] || mv "$DL.part" "$DL"; }
echo "$SHA  $DL" | sha256sum -c --quiet
[ -d "$SRC" ] || tar -xf "$DL" -C "$SW_PS4_BUILD/src"
# DolphinPS4's ps4-cc takes its sysroot from $PSB/sysroot and links configure's test programs with its runtime
# library, which this toolchain doesn't build: a sysroot of links to TriAevum's (libc++, the libc overlay) with an
# empty runtime library stands in for it (FFmpeg's libraries are static: nothing from it is linked for real).
SYS="$SW_PS4_BUILD/sysroot"
mkdir -p "$SYS/lib"
for d in include libc-overlay; do ln -sfn "$PS4_SYSROOT/$d" "$SYS/$d"; done
for f in libc++.a libc++abi.a; do ln -sfn "$PS4_SYSROOT/lib/$f" "$SYS/lib/$f"; done
[ -f "$SYS/lib/libdolphinps4_rt.a" ] || llvm-ar rc "$SYS/lib/libdolphinps4_rt.a"
export PSB="$SW_PS4_BUILD"
rm -rf "$BUILD" && mkdir -p "$BUILD"
cd "$BUILD"
"$SRC/configure" --prefix="$PREFIX" --enable-cross-compile --arch=x86_64 --cpu=btver2 --target-os=none \
    --cc="$DOLPHINPS4/toolchain/bin/ps4-cc" --cxx="$DOLPHINPS4/toolchain/bin/ps4-c++" \
    --ar="llvm-ar" --ranlib="llvm-ranlib" --nm="llvm-nm" --strip=true \
    --enable-static --disable-shared --enable-pic --enable-pthreads $(command -v nasm >/dev/null || echo --disable-x86asm) \
    --disable-autodetect --disable-programs --disable-doc --disable-network --disable-avdevice --disable-avfilter \
    --disable-swresample --disable-everything --disable-debug \
    --enable-protocol=file --enable-demuxer=matroska,mov \
    --enable-decoder=vp9,vp8,vorbis,opus,h264,aac --enable-parser=vp9,vp8,vorbis,opus,h264,aac \
    --extra-cflags="-march=btver2 -O2" >configure.log 2>&1 || { tail -30 configure.log; exit 1; }
make -j"${JOBS:-$(nproc)}" >make.log 2>&1 || { grep -m20 -B2 -A5 "error" make.log; exit 1; }
make install >/dev/null
grep -q 'GPL version 3\|--enable-gpl' "$PREFIX/lib/pkgconfig/libavcodec.pc" && { echo "FFmpeg is not LGPL" >&2; exit 1; }
ls "$PREFIX/lib"
