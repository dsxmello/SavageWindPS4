# Settings shared by the PS4 build scripts (docs/ps4.md). Source this file.
#
# The toolchain is the one TriAevum PS4 (Vulkan) already built and ran on the console: its PacBrew OpenOrbis SDK,
# libc++ 21 / glslang sysroot, Mesa RADV build and DolphinPS4 runtime tree, all used read-only from PS4_VK_ROOT.
# Savage Wind's own sources and build trees go to SW_PS4_BUILD.
SW_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export SW_ROOT
export PS4_VK_ROOT="${PS4_VK_ROOT:-$HOME/Projetos/triaevum/triaevum-ps4-vk}"
[ -f "$PS4_VK_ROOT/tools/env.sh" ] || { echo "PS4_VK_ROOT: no toolchain at $PS4_VK_ROOT" >&2; return 1; }
source "$PS4_VK_ROOT/tools/env.sh"  # PSB, PS4_SYSROOT, OPENORBIS, DOLPHINPS4, PATH, LD_LIBRARY_PATH
export SW_PS4_BUILD="${SW_PS4_BUILD:-$HOME/savagewind-ps4-build}"
# Mesa: Savage Wind's own copy and build (ps4/build-mesa.sh: TriAevum's tree + ps4/mesa-savagewind.patch)
export PS4_MESA_SRC="$SW_PS4_BUILD/src/mesa"
export PS4_MESA_BUILD="$SW_PS4_BUILD/build/mesa"
export PS4_TOOLCHAIN_FILE="$DOLPHINPS4/toolchain/ps4-love-modern.cmake"
