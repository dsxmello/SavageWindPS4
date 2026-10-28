#!/usr/bin/env bash
# Build the PS4 eboot (docs/ps4.md): the dependencies first when missing (SDL3, FFmpeg, volk), then
# ps4/CMakeLists.txt. With the game code: SW_GENERATED=<the generated/ of make gen> ps4/build.sh
#   build.sh [clean]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/env.sh"
mkdir -p "$SW_PS4_BUILD"/{src,build,downloads,out}

[ -f "$SW_PS4_BUILD/build/sdl3/libSDL3.a" ] || "$HERE/build-sdl3.sh"
"$HERE/build-mesa.sh"  # (ninja: only what changed)
[ -f "$SW_PS4_BUILD/ffmpeg/lib/libavcodec.a" ] || "$HERE/build-ffmpeg.sh"
[ -d "$SW_PS4_BUILD/src/volk" ] ||
    git clone -q --depth 1 --branch vulkan-sdk-1.4.335.0 https://github.com/zeux/volk "$SW_PS4_BUILD/src/volk"

# PGO=generate builds the instrumented eboot in its own tree, PGO=use (PGO_PROFILE=<merged .profdata>, default
# $SW_PS4_BUILD/pgo/savagewind.profdata: ps4/pgo-merge.sh) the optimized one. Every build gives the game code clang -O2
# without the vectorizers (SW_GEN_OPT=<flags> for others, e.g. the Makefile's -O1), so a plain build differs from a
# PGO one by the profile alone. package.sh takes the same PGO=.
PGO="${PGO:-}"
build="$SW_PS4_BUILD/build/savagewind${PGO:+-pgo-$PGO}"
pgo_args=()
case "$PGO" in
    "") ;;
    generate) pgo_args=(-DSW_PGO=generate -DSW_PGO_PROFILE=) ;;
    use) pgo_args=(-DSW_PGO=use -DSW_PGO_PROFILE="${PGO_PROFILE:-$SW_PS4_BUILD/pgo/savagewind.profdata}") ;;
    *) echo "PGO must be generate, use or empty" >&2; exit 1 ;;
esac
pgo_args+=(-DSW_GEN_OPT="${SW_GEN_OPT:--O2 -fno-vectorize -fno-slp-vectorize}")
[ "${1:-}" = clean ] && rm -rf "$build"
if [ ! -f "$build/CMakeCache.txt" ]; then
    env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS \
        cmake -S "$HERE" -B "$build" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$PS4_TOOLCHAIN_FILE" -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DPS4_MALLOC_REPLACE=ON -DPS4_PAID=0x3800000000000035 -DPS4_MESA_BUILD="$PS4_MESA_BUILD" \
        -DSW_GENERATED="${SW_GENERATED:-}" "${pgo_args[@]}" >/dev/null
else  # (a configured tree keeps its SW_GENERATED unless given; the Mesa build follows env.sh)
    cmake -S "$HERE" -B "$build" -DPS4_MESA_BUILD="$PS4_MESA_BUILD" ${SW_GENERATED+-DSW_GENERATED="$SW_GENERATED"} \
        "${pgo_args[@]}" >/dev/null
fi
ninja -C "$build" -k 0
ls -la "$build/savagewind_eboot/eboot.bin"
