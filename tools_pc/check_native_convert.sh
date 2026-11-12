#!/bin/bash
# G3: the native converter (port/convert) must produce byte-identical sidecars
# to the Python tools. usage: tools_pc/check_native_convert.sh [build-dir]
# Needs data/ge007.<region>.z64; runs the Python tools into a temp copy of the
# repo's data/ layout, the native tool into another, and compares.
set -euo pipefail
B=${1:-build-mac}
R=${ROMID:-ntsc-final}
cmake --build "$B" --target ge007-convert-native >/dev/null
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/py/data" "$T/py/tools_pc"
ln -s "$PWD/assets" "$PWD/scripts" "$T/py/"
cp tools_pc/d43_emit.py tools_pc/d69_emit.py tools_pc/d88_emit.py tools_pc/d88_propdefs.py "$T/py/tools_pc/"
cp "data/ge007.$R.z64" "$T/py/data/"
(cd "$T/py" && python3 tools_pc/d43_emit.py "$R" >/dev/null && python3 tools_pc/d69_emit.py "$R" >/dev/null \
   && python3 tools_pc/d88_emit.py "$R" --regen >/dev/null)
"$B/port/convert/ge007-convert-native" "data/ge007.$R.z64" "$T/c" >/dev/null
fail=0
for f in "pcmodels-$R/pcmodels.bin" "pcmodels-$R/manifest.csv" "pccg-$R/pccg.bin" "pccg-$R/manifest.csv"; do
  if cmp -s "$T/py/data/$f" "$T/c/$f"; then echo "SAME $f"; else echo "DIFF $f"; fail=1; fi
done
exit $fail
