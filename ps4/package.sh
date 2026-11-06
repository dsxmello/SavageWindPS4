#!/usr/bin/env bash
# Package the PS4 eboot (ps4/build.sh) into a fake PKG for GoldHEN: $SW_PS4_BUILD/out/IV0000-SVWD00001_00-....pkg.
# The package holds the eboot, the menu fonts and the icon: no game files and, unless the eboot was built with
# SW_GENERATED, no game code. A package built with the game code is made from the player's own game files and is
# theirs alone: never share it.
#   [PGO=generate|use] package.sh [version XX.YY]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/env.sh"
VERSION="${1:-00.01}"
build="$SW_PS4_BUILD/build/savagewind${PGO:+-pgo-$PGO}"  # (PGO=generate|use: build.sh's PGO trees)
[ "${PGO:-}" = use ] && : "${THINLTO:=1}"
[ "${THINLTO:-0}" = 1 ] && build="$build-lto"  # (THINLTO=1, the default with PGO=use: build.sh's ThinLTO tree)
eboot="$build/savagewind_eboot/eboot.bin"
[ -f "$eboot" ] || { echo "no eboot: ps4/build.sh first" >&2; exit 1; }

stage="$build/stage"
rm -rf "$stage"
mkdir -p "$stage/sce_sys"
cp "$eboot" "$stage/"
cp "$SW_ROOT/third_party/fonts/FiraSans/FiraSans-SemiBold.ttf" "$SW_ROOT/third_party/fonts/FiraSans/FiraSans-Medium.ttf" \
   "$SW_ROOT/third_party/fonts/FiraSans/OFL.txt" "$stage/"
cp "$SW_ROOT/docs/icon/icon_1024.png" "$stage/icon.png"
cc -O2 -I"$SW_ROOT/third_party/stb" "$HERE/make_icon.c" -o "$build/make_icon" -lm
"$build/make_icon" "$SW_ROOT/docs/icon/icon_1024.png" "$stage/sce_sys/icon0.png"

# Plain identity (CATEGORY gd): the process gets its own VideoOut and 4.5 GiB of direct memory (RADV on GNM).
pkg="$(SFO_STYLE=plain NO_PIGLET_MODULES=1 "$PS4_VK_ROOT/tools/make-pkg.sh" "$stage" "${TITLE_ID:-SVWD00001}" \
    "Savage Wind" "$VERSION" SAVAGEWIND "$SW_PS4_BUILD/out" | tail -1)"
echo "$pkg"
sha256sum "$pkg"
