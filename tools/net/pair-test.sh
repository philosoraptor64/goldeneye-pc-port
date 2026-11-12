#!/bin/bash
# Netplay pair test: two copies of the game on this machine (host = player 1,
# guest = player 2), both driven by scripted bots, and a per-frame comparison
# of their state hashes.
#
# Usage: tools/net/pair-test.sh   (from the repo root, after building build-pc)
# Env:   FRAMES=3600 DELAY=2 MP=2,1 (GE_STARTMP: players,mpstage[,scenario]; empty = boot to the title)
#        LATENCY/JITTER (ms) LOSS (%) on both sides; SERVER=local [ROOM=GETS RELAY=1] (local room server);
#        DESYNC_TEST=<tick> (inject a desync on the guest: the run must report it);
#        GUEST_LATE=<secs> (start the guest later); KILL_GUEST=<secs> (kill it: the host must play on)
set -uo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"
bin=$(ls build-pc/ge007*.aarch64 build-pc/ge007*.x86_64 2>/dev/null | head -1)
[ -x "$bin" ] || { echo "build first (build-pc)"; exit 1; }
FRAMES=${FRAMES:-3600}
MP=${MP-2,1}
out=build-pc/net-test/$(date +%Y%m%d-%H%M%S); mkdir -p "$out"

common=(GE_NET_BOT=1 GE_NET_FRAMES=$FRAMES GE_NET_DELAY=${DELAY:-2}
        GE_NET_SIM_LATENCY=${LATENCY:-0} GE_NET_SIM_JITTER=${JITTER:-0} GE_NET_SIM_LOSS=${LOSS:-0})
[ -n "$MP" ] && common+=(GE_STARTMP=$MP GE_MPVIRT=1)

spid=
if [ "${SERVER:-}" = local ]; then
    python3 external/n64net/server/rendezvous.py --bind 127.0.0.1 --port 7790 > "$out/server.log" 2>&1 &
    spid=$!
    sleep 1
    hostnet=(GE_NET_HOST=1 GE_NET_SERVER=127.0.0.1:7790 GE_NET_ROOM=${ROOM:-GETS} ${RELAY:+GE_NET_RELAY=$RELAY})
    guestnet=(GE_NET_SERVER=127.0.0.1:7790 GE_NET_ROOM=${ROOM:-GETS} ${RELAY:+GE_NET_RELAY=$RELAY})
else
    hostnet=(GE_NET_HOST=1 GE_NET_PORT=7501)
    guestnet=(GE_NET_PEER=127.0.0.1:7501 GE_NET_PORT=7502)
fi

env "${common[@]}" "${hostnet[@]}" GE_NET_HASHLOG="$out/host.hash" "$bin" > "$out/host.log" 2>&1 &
hpid=$!
sleep "${GUEST_LATE:-1}"
env "${common[@]}" "${guestnet[@]}" GE_NET_HASHLOG="$out/guest.hash" ${DESYNC_TEST:+GE_NET_TEST_DESYNC=$DESYNC_TEST} \
    "$bin" > "$out/guest.log" 2>&1 &
gpid=$!
trap 'kill $hpid $gpid $spid 2>/dev/null' INT TERM
if [ -n "${KILL_GUEST:-}" ]; then
    sleep "$KILL_GUEST"; kill -9 $gpid 2>/dev/null; echo "(guest killed after $KILL_GUEST s)"
fi
wait $hpid; wait $gpid 2>/dev/null
[ -n "$spid" ] && kill $spid 2>/dev/null

for side in host guest; do
    echo "== $side"; grep -E 'genet:' "$out/$side.log" | grep -v 'fixed-step' | tail -5
done
for side in host guest; do
    grep -q 'connected as player' "$out/$side.log" || { echo "RESULT: FAILED ($side never connected)"; exit 1; }
done
if [ -n "${DESYNC_TEST:-}" ]; then
    grep -q 'DESYNC detected' "$out/host.log" "$out/guest.log" && { echo "RESULT: DESYNC DETECTED (as injected)"; exit 0; }
    echo "RESULT: FAILED (injected desync not detected)"; exit 1
fi
if grep -q 'DESYNC' "$out/host.log" "$out/guest.log"; then echo "RESULT: DESYNC"; exit 1; fi
n=$(wc -l < "$out/guest.hash"); h=$(wc -l < "$out/host.hash"); [ "$h" -lt "$n" ] && n=$h
if [ -n "${KILL_GUEST:-}" ]; then
    grep -q 'Playing on offline' "$out/host.log" && echo "RESULT: OK (host played on offline after the guest was killed)" \
        || echo "RESULT: FAILED (host did not go offline)"
    exit 0
fi
if cmp -s <(head -n "$n" "$out/host.hash") <(head -n "$n" "$out/guest.hash"); then
    echo "RESULT: IN SYNC ($n frames identical; logs in $out)"
else
    first=$(diff <(head -n "$n" "$out/host.hash") <(head -n "$n" "$out/guest.hash") | head -2 | tail -1)
    echo "RESULT: DIFFERENT HASHES (first: $first; logs in $out)"; exit 1
fi
