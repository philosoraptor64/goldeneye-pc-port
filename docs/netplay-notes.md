# Netplay and iPhone notes (philosoraptor64 fork, branch `netplay`)

Online play for the GoldenEye 007 PC port, built on
[n64net](https://github.com/philosoraptor64/n64net) (the same library the Smash,
Mario Kart, Star Fox and Perfect Dark ports use), plus an iOS build.
Base: upstream `release/v0.6.0` (native macOS arm64).

## Building

| Build | Command |
|---|---|
| Mac, GCC (upstream default; OpenGL) | `cmake -S . -B build-pc -G Ninja -DROMID=ntsc-final -DCMAKE_OSX_SYSROOT=$(xcrun --show-sdk-path)` |
| Mac, clang (OpenGL or Metal) | add `-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++`, e.g. `-B build-clang` |
| iOS Simulator | `cmake -S . -B build-iossim -G Ninja -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_ARCHITECTURES=arm64 -DROMID=ntsc-final -DSDL2_INCLUDE_DIR=<sdl>/include/SDL2 -DSDL2_LIBRARY=<sdl>/lib/libSDL2.a -DSDL2MAIN_LIBRARY=<sdl>/lib/libSDL2main.a` |

`<sdl>` = a static SDL2 with the UIScene patch (iOS 27 SDK), e.g.
`~/Development/N64/deps-iossim/sdl2` (simulator) / `deps-ios/sdl2` (device).
Then `cmake --build <dir>`.

All builds use `-ffp-contract=off`: GCC and clang builds simulate
bit-identically (checked on a 5400-frame 4-player match).

Game data (once, Mac): ROM -> `data/ge007.ntsc-final.z64`, then
`python3 tools_pc/d43_emit.py ntsc-final && python3 tools_pc/d69_emit.py ntsc-final && python3 tools_pc/d88_emit.py ntsc-final --regen`.
On iOS the same files go into the app's Documents folder (`$S` = Documents):
the ROM, `pcmodels-ntsc-final/`, `pccg-ntsc-final/` (the sim test copies them).
TODO: convert on the device (the Python converters would need a C port; upstream
spawns a bundled Python binary, impossible on iOS).

## Determinism (port/src/genet.c)

Upstream's simulation steps by "however many 1/60 s of wall-clock passed"
(findings D117), so two machines diverge. `GE_NET_DETERM=1` (always on online):
- a fixed `GE_NET_STEP` (1) tick per frame on an absolute 60 Hz schedule;
- input only through the RAMROM demo player's playback seam
  (`joySetPlaybackFunc` + contdata 1): one sample of all four pads per frame;
- no attract demo (it would replace the seam);
- `GE_RSEED` / the session seed for the boot RNG.
`GE_NET_HASHLOG=<file>` writes a per-frame state hash (RNG seeds, timers, menu
state, players, chrs). Same-seed runs are identical, also with the CPU loaded,
with GCC vs clang, and with Metal vs OpenGL.

Test aids: `GE_NET_BOT=<pad mask>` (scripted bots), `GE_NET_BOT_FROM=<tick>`,
`GE_NET_SCRIPT="tick:TOK,TOK;..."` (frame-exact menu scripts: A B Z START L R
UP DOWN LEFT RIGHT CU CD CL CR, sticks SUP SDOWN SLEFT SRIGHT SNONE),
`GE_NET_MENULOG=1` (menu/stage changes per tick), `GE_NET_FRAMES=<n>` (quit),
`GE_NET_CONTROLLERS=<n>` (offline controller count), upstream's
`GE_STARTMP=<players>,<mpstage>` + `GE_MPVIRT=<n-1>` (start an MP match at boot;
mpstage 1 = Temple, 0 = random).

## Online play

Lockstep through `n64net_input` (input delay 2, raised automatically up to 6).
The whole game is shared from boot like one console: host = player 1,
guest = player 2; the guest uses the host's EEPROM (in memory, its own
`ge007.eep` untouched) and seed; build fingerprint checked at the handshake.

| Setting | |
|---|---|
| `GE_NET_HOST=1 [GE_NET_PORT=7501]` | host |
| `GE_NET_PEER=a.b.c.d[:7501] [GE_NET_PORT=7502]` | guest |
| `GE_NET_SERVER=host:port GE_NET_ROOM=ABCD [GE_NET_PASSWORD, GE_NET_RELAY=1]` | room code (both) |
| `GE_NET_DELAY`, `GE_NET_WAIT_SECS` (60), `GE_NET_DROP_SECS` (20) | |
| `GE_NET_SIM_LATENCY/_JITTER` (ms), `GE_NET_SIM_LOSS` (%) | simulated bad link |
| `GE_NET_TEST_DESYNC=<tick>` | perturbs the RNG (desync detection test) |
| `GE_NET_VIEW=split` / `GE_NET_FULLVIEW=<player>` | full view off / forced |

iOS has no environment: the same lines go into `Documents/netplay.cfg`.

- Controls online: `inputNetPure` forces the port's Original scheme (a strict
  N64 pad) and turns off mouse look and the dedicated use/reload/crouch/gadget
  actions, which write game state directly (porting notes F2).
- Port settings that change the simulation: a guest with FOV 150, draw/LOD
  distance 400, no native widescreen and an 800x600 window stays identical
  (upstream's gameplay-visibility layer, D466). F10 edits during a match are
  not synced (avoid them online).
- Full view: in an online 2-4 player match each machine shows only its own
  player's view, scaled uniformly to the window (gfx_pc.cpp canvas offset +
  scissor clip; the game still draws all views).
- A silent peer for 20 s or a BYE = play on offline (inputs it already sent are
  still used). An iPhone switching apps is different: the game holds at a
  frame boundary and tells the other player it is away (n64net AWAY); the
  other side waits as long as it takes, then both resume in sync
  (`GE_NET_TEST_AWAY=<tick>:<secs>` simulates it; sim-test `AWAY=<at>:<secs>`
  switches the Simulator to Settings and back).

Tests (Mac): `tools/net/pair-test.sh` (two copies; `MENUS=1` = boot, title,
host walks Multiplayer -> Start by script; `LATENCY/JITTER/LOSS`, `SERVER=local
[RELAY=1]`, `DESYNC_TEST=<tick>`, `KILL_GUEST=<s>`, `GUEST_SET="Video.FovScale=150 ..."`).
`tools/net/sim-test.sh` = the Mac hosts, the booted iOS Simulator joins
(`MENUS=1`, `FRAMES`). All pass (see the commit messages for numbers).

## Rollback (Apple clang builds: Mac `build-clang`, iOS)

In a running multiplayer match the game predicts the other player's input
and corrects mispredictions by rewinding and resimulating (n64net
`n64net_rollback_frame`, window 8 frames, input delay 3); menus, the first
second of a match and everything after the game-over screen are lockstep.
GCC builds have no snapshots and stay lockstep (they still play with
rollback builds). `GE_NET_ROLLBACK=0` forces lockstep.

- Snapshot (`port/src/gesnap.c`): the game's C globals (the `__GESNAP`
  segment: game files force-include `port/include/gesnap.h`) + the DRAM
  arena minus the audio thread's music heap + the playback controller
  buffer = 8.7 MB, saved/loaded in ~0.13 ms; 16 slots.
- Resimulation (`resimFrame` in genet.c) = src/boss.c's frame body without
  the GPU task: GE ticks props, AI and auto-aim inside lvlRender, so the
  display list is built and thrown away. Before restoring memory the game
  waits until every submitted display list was drawn.
- Outside the snapshot on purpose: audio (own thread), the scheduler and
  message queues, controller polling (joy.c), screen shake (retrace
  thread), performance graphs, the wall-clock copies, gfx task settings.
  Sounds of resimulated frames: genet lists every sound each tick starts
  (last 64 ticks); a resimulation pass doesn't start one that tick already
  started, and the game gets that sound's state back (src/snd.c wrapper,
  so the game state matches the first pass: SyncTest stays 0 bytes); a
  sound only the corrected frames make (the other player's shot) plays, a
  few frames late. Stats: "resim sounds skipped N new M".
- A stage end produced by a predicted frame is settled in a boss.c hook
  before the stage unloads (genetStageExit); normally the window is already
  closed by then (the game-over screen locks the controls).
- `GE_SYNCTEST=N` (offline): rewind N frames every frame and compare the
  whole snapshot byte for byte; `GE_SYNCTEST_CONTINUE=1` plays on from the
  resimulated state, `GE_SYNCTEST_DUMP=<file>` lists every differing range
  of the first mismatch (symbolize with `atos -o <binary> -l <image base>`),
  `GE_NET_HASHDETAIL=<tick>` prints the hash parts. Result: 2P and 4P,
  rewinding 8 frames every frame for 3600 frames, 0 differing bytes.
- Results (pair tests, simulated 40-50 ms + 20-30 ms jitter + 3-5% loss):
  in sync; ~200 rollbacks per side per 30 s (max depth 6-9); stalls ~0.2-1
  s per minute at delay 3, where lockstep needs delay 6 for the same stalls;
  Mac vs iOS Simulator in sync, also across a 15 s app switch.
  `GE_NET_TEST_MPTIME=<s>` shortens matches (match-end tests).

## iOS

- `GE_IOS` (CMake) selects the iOS code. Do not test `PLATFORM_IOS`:
  `<mach-o/loader.h>` defines it on macOS too.
- Renderer: Metal (`port/fast3d/gfx_metal.mm`, ported from the PD port).
  OpenGL ES drew into SDL's framebuffer but never reached the screen.
  `GE_RENDERER=metal` uses it on macOS too (clang builds).
- Address space: the port maps the N64's 32-bit space at a fixed host base
  (`PORT_ADDR_BASE`, 16 TiB on macOS/Simulator). Only the regions it maps are
  reserved (224 MB: cart, DRAM + mirror, thread stacks). A real iPhone has only
  ~64 GB of address space, so the device build needs a base below that whose
  regions are free; not known yet (needs a run on a phone).
- Not done: signing/installing on a phone (MacBook), touch controls (use a Bluetooth controller), on-device
  data conversion.

## G1: iPhone address window (MacBook, 2026-11-12, branch `ios-device`)
Probed on the iPhone 14 Pro (iPhone15,2, iOS 27) with perfect_dark's `PD_VMPROBE=1` (port/src/vmprobe.c there: tries
the four regions at every 4 GB-aligned base with vm_map VM_FLAGS_FIXED, non-clobbering; full output in perfect_dark
tools/vmprobe-iphone14pro.txt):
- 0x1_0000_0000 and 0x2_0000_0000: KERN_NO_SPACE (the app's own mappings: heap around 0x2_C000_0000-0x3_0000_0000).
- **0x3_0000_0000: all four regions fit** (the only one).
- 0x4_0000_0000 and up (incl. the macOS default 0x1000_0000_0000): KERN_INVALID_ADDRESS: an app's address space ends
  near 0x4_4000_0000 (~17 GB) without the extended-virtual-addressing entitlement (not available to free teams).
- CMakeLists.txt: device builds (GE_IOS + iphoneos sysroot) now default to PORT_ADDR_BASE=0x300000000; the
  Simulator keeps 0x100000000000.
Caveats: the probe ran inside PD after PD's own allocations (16 MB heap + 32 MB ROM), so GoldenEye's heap may sit
differently: reserve the window first thing in main (port_addr.c) and log vm_region on failure. Re-probe on the Duo
(iPhone19,4, more RAM) when it's back. Next: G1 device build with this base, sign (stub profile, existing cert),
copy ROM + pcmodels/pccg sidecars to Documents, boot.
