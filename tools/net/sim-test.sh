#!/bin/bash
# Netplay between this Mac (host = player 1, build-pc) and the iOS build in the
# booted iOS Simulator (guest = player 2, build-iossim). Both driven by bots;
# per-frame state hashes compared. Exercises the iOS-only code (Documents,
# netplay.cfg, Metal, the UIScene app) without signing or a phone.
# Usage: tools/net/sim-test.sh   (env: FRAMES=2400 MENUS=1 (title -> Multiplayer
#        by host script) or MP=2,1 (default: start a 2P Temple match at boot);
#        LATENCY/JITTER/LOSS; SIM=<udid> (default: booted); BIN=build-pc)
# The simulator app needs the ROM and converted data in its Documents once
# (copied from ./data by this script).
set -uo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"
SIM=${SIM:-booted}; BUNDLE=io.github.philosoraptor.goldeneye
FRAMES=${FRAMES:-2400}; MP=${MP-2,1}
bin=${BIN:-build-pc}/ge007.aarch64
out=build-pc/net-test/sim-$(date +%Y%m%d-%H%M%S); mkdir -p "$out"

xcrun simctl install "$SIM" build-iossim/ge007.aarch64.app || { echo "RESULT: FAILED (install)"; exit 1; }
data=$(xcrun simctl get_app_container "$SIM" $BUNDLE data) || { echo "RESULT: FAILED (no container)"; exit 1; }
docs="$data/Documents"; mkdir -p "$docs"
[ -f "$docs/ge007.ntsc-final.z64" ] || cp data/ge007.ntsc-final.z64 "$docs/"
for d in pcmodels-ntsc-final pccg-ntsc-final; do rm -rf "$docs/$d"; cp -R "data/$d" "$docs/"; done
rm -f "$docs/ge007.log" "$docs/guest.hash"

common=(GE_NET_BOT=1 GE_NET_FRAMES=$FRAMES GE_NET_SIM_LATENCY=${LATENCY:-0}
        GE_NET_SIM_JITTER=${JITTER:-0} GE_NET_SIM_LOSS=${LOSS:-0})
hostonly=()
if [ "${MENUS:-0}" = 1 ]; then
    sc=""; for t in $(seq 60 60 540); do sc="$sc$t:START;"; done
    hostonly+=("GE_NET_SCRIPT=${sc}580:SDOWN;586:SNONE;600:A;700:START")
    common+=(GE_NET_BOT_FROM=800)
elif [ -n "$MP" ]; then
    common+=(GE_STARTMP=$MP GE_MPVIRT=1)
fi

# the guest's settings go through the iPhone path: Documents/netplay.cfg
{
    echo "# written by tools/net/sim-test.sh"
    for kv in "${common[@]}" GE_NET_PEER=127.0.0.1:7501 GE_NET_PORT=7502 "GE_NET_HASHLOG=$docs/guest.hash"; do echo "$kv"; done
} > "$docs/netplay.cfg"

env "${common[@]}" ${hostonly[@]+"${hostonly[@]}"} GE_NET_HOST=1 GE_NET_PORT=7501 GE_NET_HASHLOG="$out/host.hash" \
    "$bin" > "$out/host.log" 2>&1 &
hpid=$!
sleep 1
SIMCTL_CHILD_SDL_AUDIODRIVER=dummy xcrun simctl launch --terminate-running-process "$SIM" $BUNDLE > /dev/null
trap 'kill $hpid 2>/dev/null; xcrun simctl terminate "$SIM" $BUNDLE 2>/dev/null' INT TERM
wait $hpid
# the guest quits by itself after FRAMES too; give it a moment
for _ in $(seq 1 20); do
    grep -q 'frames done' "$docs/ge007.log" 2>/dev/null && break
    sleep 1
done
xcrun simctl terminate "$SIM" $BUNDLE 2>/dev/null
cp "$docs/ge007.log" "$out/guest.log" 2>/dev/null
cp "$docs/guest.hash" "$out/guest.hash" 2>/dev/null
echo "# netplay off (sim-test.sh)" > "$docs/netplay.cfg"

for side in host guest; do
    echo "== $side"; grep -E 'genet:' "$out/$side.log" | grep -v -E 'fixed-step|script:' | tail -4
done
for side in host guest; do
    grep -q 'connected as player' "$out/$side.log" 2>/dev/null || { echo "RESULT: FAILED ($side never connected)"; exit 1; }
done
grep -q DESYNC "$out/host.log" "$out/guest.log" && { echo "RESULT: DESYNC"; exit 1; }
n=$(wc -l < "$out/guest.hash"); h=$(wc -l < "$out/host.hash"); [ "$h" -lt "$n" ] && n=$h
if cmp -s <(head -n "$n" "$out/host.hash") <(head -n "$n" "$out/guest.hash"); then
    echo "RESULT: IN SYNC ($n frames identical, Mac vs iOS Simulator; logs in $out)"
else
    echo "RESULT: DIFFERENT HASHES (first: $(diff <(head -n "$n" "$out/host.hash") <(head -n "$n" "$out/guest.hash") | sed -n 2p); logs in $out)"
    exit 1
fi
