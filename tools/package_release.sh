#!/usr/bin/env bash
# Package an mctde-Link release zip, with checksum.
#
#   tools/package_release.sh 0.4.9
#
# The same d3d9.dll runs on Windows and on Linux (through Proton), so Link ships ONE zip for both.
#
# Inputs:
#   Release/d3d9.dll   the Release | x86 build (MSVC).            Override with DLL=...
#   mctde_input.dll    a 32-bit Valve steam_api.dll, renamed       Set INPUT_DLL=... (see docs/RELEASING.md)
#
# Output in dist/<version>/ -- attach both files to a GitHub release (a full release, not a
# pre-release, so it becomes /releases/latest):
#   mctde-Link.zip         d3d9.dll, mctde_input.dll, mctde-link.ini, README.md, CHANGELOG.md
#   mctde-Link.zip.sha256
set -euo pipefail
cd "$(dirname "$0")/.."

VER=${1:-}
[[ -n $VER ]] || { echo "usage: $0 <version>" >&2; exit 2; }
[[ $VER =~ ^[0-9]+(\.[0-9]+)*$ ]] || { echo "Use a plain dotted version (e.g. 0.4.9)." >&2; exit 1; }

# Both version stamps must say $VER: the updater compares CURRENT_MCTDE_LINK_VERSION, and the
# launchers read the dll's FILEVERSION.
SRC_VER=$(sed -n 's/^#define CURRENT_MCTDE_LINK_VERSION "\(.*\)"/\1/p' mctde-Link/VersionChecker.cpp)
RC_VER=$(sed -n 's/^ *FILEVERSION \([0-9]*\),\([0-9]*\),\([0-9]*\),.*/\1.\2.\3/p' mctde-Link/version.rc)
[[ $SRC_VER == "$VER" ]] || { echo "VersionChecker.cpp has CURRENT_MCTDE_LINK_VERSION \"$SRC_VER\", not $VER." >&2; exit 1; }
[[ $RC_VER == "$VER" ]] || { echo "version.rc has FILEVERSION $RC_VER, not $VER." >&2; exit 1; }
grep -q "^## v$VER" CHANGELOG.md || { echo "CHANGELOG.md has no '## v$VER' section (rename 'Unreleased')." >&2; exit 1; }

DLL=${DLL:-Release/d3d9.dll}
INPUT_DLL=${INPUT_DLL:-}
[[ -f $DLL ]] || { echo "Missing $DLL (build Release | x86)." >&2; exit 1; }
[[ $(head -c2 "$DLL") == MZ ]] || { echo "$DLL isn't a Windows dll." >&2; exit 1; }
if [[ -z $INPUT_DLL || ! -f $INPUT_DLL ]]; then
    echo "Set INPUT_DLL to the 32-bit steam_api.dll to ship as mctde_input.dll (docs/RELEASING.md)." >&2
    echo "It's required for [Controller] BindingNudge; 0.4.8 went out without it." >&2
    exit 1
fi
# Both dlls must be 32-bit (PE32, machine 0x14c), like PTDE.
pe_machine() { python3 -I -c 'import struct,sys; d=open(sys.argv[1],"rb").read(4096); o=struct.unpack_from("<I",d,0x3c)[0]; print(hex(struct.unpack_from("<H",d,o+4)[0]))' "$1"; }
for f in "$DLL" "$INPUT_DLL"; do
    [[ $(pe_machine "$f") == 0x14c ]] || { echo "$f isn't a 32-bit (x86) dll." >&2; exit 1; }
done

OUT=dist/$VER
rm -rf "$OUT"; mkdir -p "$OUT/stage"
cp "$DLL" "$OUT/stage/d3d9.dll"
cp "$INPUT_DLL" "$OUT/stage/mctde_input.dll"
cp samples/mctde-link.sample.ini "$OUT/stage/mctde-link.ini"   # never overwrites a player's ini
cp README.md CHANGELOG.md "$OUT/stage/"
python3 -I - "$OUT/mctde-Link.zip" "$OUT"/stage/{d3d9.dll,mctde_input.dll,mctde-link.ini,README.md,CHANGELOG.md} <<'PY'
import os, sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for f in sys.argv[2:]:
        z.write(f, os.path.basename(f))
PY
rm -rf "$OUT/stage"
(cd "$OUT" && sha256sum mctde-Link.zip > mctde-Link.zip.sha256)

echo "Release files in $OUT:"
(cd "$OUT" && ls -l)
python3 -I -c 'import sys,zipfile; [print("  "+n) for n in zipfile.ZipFile(sys.argv[1]).namelist()]' "$OUT/mctde-Link.zip"
cat <<EOF

Next:
  1. GitHub release in mctde-Link (a full release, not a pre-release) with mctde-Link.zip and
     mctde-Link.zip.sha256 attached.
  2. Then set mctde-link=$VER in latest.txt on main.
EOF
