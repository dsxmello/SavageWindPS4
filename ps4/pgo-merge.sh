#!/usr/bin/env bash
# PGO, step 2 of 3 (docs/ps4.md): fetch the instrumented eboot's profile from the console (GoldHEN's FTP server) and
# merge it into $SW_PS4_BUILD/pgo/savagewind.profdata, which PGO=use ps4/build.sh builds from.
#   PS4_FTP=ftp://<console ip>:2121 pgo-merge.sh [local .profraw ...]
# With files given, those are merged instead (several sessions: one .profdata).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/env.sh"
out="$SW_PS4_BUILD/pgo"
mkdir -p "$out"
if [ $# -eq 0 ]; then
    [ -n "${PS4_FTP:-}" ] || { echo "PS4_FTP=ftp://<console ip>:2121 needed (or local .profraw files)" >&2; exit 1; }
    raw="$out/savagewind-$(date +%Y%m%d-%H%M).profraw"
    curl -sS --max-time 900 -o "$raw" "$PS4_FTP/data/savagewind/pgo/savagewind.profraw"
    set -- "$raw"
fi
ls -la "$@"
llvm-profdata merge -o "$out/savagewind.profdata" "$@"
llvm-profdata show "$out/savagewind.profdata" | tail -4
echo "$out/savagewind.profdata"
