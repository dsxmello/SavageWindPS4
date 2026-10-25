#!/usr/bin/env bash
# Savage Wind's own build of Mesa's RADV for the PS4 (docs/ps4.md): TriAevum PS4's Mesa 26.0.8 tree and DolphinPS4's
# GNM winsys (port/mesa/ac_ps4_drm.c), copied to $SW_PS4_BUILD/src/mesa, plus ps4/mesa-savagewind.patch (user memory
# buffers: the guest heap imported for shader global memory). The meson setup is DolphinPS4's scripts/build-mesa.sh's,
# with this tree and build directory; TriAevum's own tree and build stay as they are.
#   build-mesa.sh [setup]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/env.sh"
source "$DOLPHINPS4/scripts/ps4-env.sh"
ROOT="$PSB"
SYSROOT="$PS4_SYSROOT"
MESA="$PS4_MESA_SRC"
build="$PS4_MESA_BUILD"

if [ ! -d "$MESA" ]; then
    mkdir -p "$(dirname "$MESA")"
    cp -a "$ROOT/src/mesa" "$MESA"
fi
# the files the patch changes, as TriAevum's tree has them (uncommitted PS4 changes there), then the patch; files left
# the same keep their timestamps, so ninja rebuilds only when the patch changed
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/src/amd/common" "$tmp/src/amd/vulkan/winsys/amdgpu"
tr -d '\015' < "$DOLPHINPS4/port/mesa/ac_ps4_drm.c" > "$tmp/src/amd/common/ac_ps4_drm.c"
cp "$ROOT/src/mesa/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_bo.c" "$tmp/src/amd/vulkan/winsys/amdgpu/"
patch -d "$tmp" -p1 -s -f --no-backup-if-mismatch < "$HERE/mesa-savagewind.patch"
for f in src/amd/common/ac_ps4_drm.c src/amd/vulkan/winsys/amdgpu/radv_amdgpu_bo.c; do
    cmp -s "$tmp/$f" "$MESA/$f" || cp "$tmp/$f" "$MESA/$f"
done

export PATH="$ROOT/mesa-venv/bin:$ROOT/host-tools/bin:$PATH"
pc="$ROOT/mesa-pkgconfig"  # (libdrm headers only: written by TriAevum's build, read here)
flags="'-target', 'x86_64-pc-freebsd12-elf', '-D_GNU_SOURCE', '-D__PS4__', '-D__OPENORBIS__',
  '-D__ORBIS__', '-DPS4', '-D__BSD_VISIBLE', '-D_BSD_SOURCE', '-fPIC', '-funwind-tables',
  '-march=btver2', '-isysroot', '$OPENORBIS', '-isystem', '$SYSROOT/libc-overlay',
  '-isystem', '$DOLPHINPS4/port/mesa-overlay', '-I$ROOT/src/libdrm', '-I$ROOT/src/libdrm/include/drm',
  '-isystem', '$OPENORBIS/include', '-I$OPENORBIS/usr/include'"
link="'-fuse-ld=$OPENORBIS/bin/ld.lld', '-nostdlib', '-Wl,-m,elf_x86_64', '-Wl,-pie',
  '-Wl,--eh-frame-hdr', '-Wl,--script,$OPENORBIS/link.x', '-L$OPENORBIS/lib',
  '$OPENORBIS/lib/crt1.o', '-Wl,--sysroot=$OPENORBIS', '-lkernel', '-lc', '-lclang_rt.builtins-x86_64',
  '-lSceLibcInternal'"
cross="$build-cross.ini"
mkdir -p "$(dirname "$build")"
cat > "$cross" <<EOF
[binaries]
c = 'clang-21'
cpp = 'clang++-21'
ar = 'llvm-ar-21'
strip = 'llvm-strip-21'
pkg-config = 'pkg-config'

[properties]
needs_exe_wrapper = true
pkg_config_libdir = '$pc'

[built-in options]
c_args = [$flags]
cpp_args = ['-nostdinc++', '-isystem', '$SYSROOT/include/c++/v1', $flags]
c_link_args = [$link]
cpp_link_args = [$link, '$SYSROOT/lib/libc++.a', '$SYSROOT/lib/libc++abi.a',
  '$OPENORBIS/lib/libunwind.a']

[host_machine]
system = 'freebsd'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF

if [ "${1:-}" = setup ] || [ ! -f "$build/build.ninja" ]; then
    rm -rf "$build"
    PKG_CONFIG_LIBDIR="$pc" meson setup "$build" "$MESA" --cross-file "$cross" \
        -Dbuildtype=release -Db_ndebug=true -Ddefault_library=static \
        -Dvulkan-drivers=amd -Dgallium-drivers= -Dplatforms= -Dllvm=disabled \
        -Damd-use-llvm=false -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dglx=disabled \
        -Degl=disabled -Dgbm=disabled -Dzstd=disabled -Dexpat=disabled -Dxmlconfig=disabled \
        -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled -Dbuild-tests=false \
        -Dshader-cache=disabled -Dvulkan-layers= -Dtools= -Dvideo-codecs= -Dspirv-tools=disabled \
        -Dperfetto=false -Dselinux=false
fi
libs=(src/amd/vulkan/libvulkan_radeon.a src/amd/addrlib/libaddrlib.a
    src/amd/common/libamd_common.a src/amd/compiler/libaco.a src/c11/impl/libmesa_util_c11.a
    src/compiler/libcompiler.a src/compiler/nir/libnir.a src/compiler/spirv/libvtn.a
    src/util/blake3/libblake3.a src/util/libmesa_util.a src/util/libmesa_util_clflush.a
    src/util/libmesa_util_clflushopt.a src/util/libmesa_util_simd.a src/util/libparson.a
    src/util/libxmlconfig.a src/vulkan/runtime/libvulkan_instance.a
    src/vulkan/runtime/libvulkan_runtime.a src/vulkan/util/libvulkan_util.a
    src/vulkan/wsi/libvulkan_wsi.a)
meson configure "$build" -Dradv-build-id="$(git -C "$MESA" rev-parse HEAD)-savagewind" >/dev/null
ninja -C "$build" "${libs[@]}"
