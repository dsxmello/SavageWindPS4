#!/usr/bin/env bash
# glslang 15.4.0 as static PS4 libraries in $SW_PS4_BUILD/glslang, as TriAevum PS4's tools/build-glslang.sh builds it
# into its sysroot (front end + SPIR-V back end only), for a toolchain set up without it. ps4/CMakeLists.txt takes
# this one when it exists.
set -euo pipefail
source "$(dirname "$0")/env.sh"
VER=15.4.0
SHA=b16c78e7604b9be9f546ee35ad8b6db6f39bbbbfb19e8d038b6fe2ea5bba4ff4
tarball="$SW_PS4_BUILD/downloads/glslang-$VER.tar.gz"
src="$SW_PS4_BUILD/src/glslang-$VER"
build="$SW_PS4_BUILD/build/glslang"
prefix="$SW_PS4_BUILD/glslang"
if [ ! -d "$src" ]; then
    [ -f "$tarball" ] || curl -fsSL -o "$tarball" "https://github.com/KhronosGroup/glslang/archive/refs/tags/$VER.tar.gz"
    echo "$SHA  $tarball" | sha256sum -c --quiet
    tar -xzf "$tarball" -C "$SW_PS4_BUILD/src"
fi
rm -rf "$build"
env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS \
    cmake -S "$src" -B "$build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PS4_TOOLCHAIN_FILE" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG -march=btver2" \
    -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG -march=btver2" -DCMAKE_INSTALL_PREFIX="$prefix" -DBUILD_SHARED_LIBS=OFF \
    -DGLSLANG_TESTS=OFF -DGLSLANG_ENABLE_INSTALL=ON -DENABLE_GLSLANG_BINARIES=OFF -DENABLE_HLSL=OFF -DENABLE_OPT=OFF \
    -DENABLE_SPVREMAPPER=OFF -DENABLE_PCH=OFF -DENABLE_CTEST=OFF -DALLOW_EXTERNAL_SPIRV_TOOLS=OFF >/dev/null
ninja -C "$build"
rm -rf "$prefix"
ninja -C "$build" install >/dev/null
ls "$prefix/lib"
