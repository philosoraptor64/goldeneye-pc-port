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
**Native converter (G3, 2026-11-12):** `port/convert/ge_convert.c` is a C port of
d43/d69/d88(+propdefs), byte-identical output (`tools_pc/check_native_convert.sh`),
0.3 s on the Mac. The file tables come from the repo at build time
(`tools_pc/gen_convert_tables.py` -> build/port/convert/ge_convert_tables.h, one region
per build). The game runs it on first boot when the sidecars are missing (romconvert.c,
before the bundled Python converter; the only path on iOS), so on the phone only the ROM
needs to go into Documents. Tested: 14 Pro clean install + ROM only -> converted, boots.

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
- CMakeLists.txt: device builds (GE_IOS and a sysroot not matching [Ss]imulator; the sysroot is the full
  .../iPhoneOS27.2.sdk path and MATCHES is case-sensitive) default to PORT_ADDR_BASE=0x300000000; the Simulator keeps
  0x100000000000. Reconfigure an old build folder with `-UPORT_ADDR_BASE` (cached value).
- **Device build compiles + links with that base** (2026-11-12): configure as the Simulator row of the table with
  `-DCMAKE_OSX_SYSROOT=iphoneos` and the device SDL (~/Developer/N64/deps-ios/sdl2 on the MacBook) ->
  build-ios/ge007.aarch64.app. Not installed: the 14 Pro is at the free-profile limit of 3 apps (i64, Perfect Dark,
  Starship); the user must pick one to remove. Then: stub profile for its bundle id, codesign, devicectl install, copy
  the ROM + pcmodels-ntsc-final/ + pccg-ntsc-final/ (Mac converters) into Documents.
- **Boots on the iPhone 14 Pro (2026-11-12, MacBook).** Fix needed: CMake preprocessed `dram_syms.S` without
  `-DPORT_ADDR_BASE`, so portaddr.h fell back to the macOS 16 TiB base and portAddrInit aborted with
  "dram_syms base mismatch (cfb_16=0x100070000000, expected 0x370000000)". Now passed explicitly.
  Result: the four regions reserve at 0x3_0000_0000 (224 MB), the game renders and ran 30 s / 1500+ frames
  with no crash (no screen check yet: the user was away). Signed like PD's M7b (stub app `io.github.philosoraptor.goldeneye`,
  existing cert); data copied to Documents: ge007.ntsc-final.z64, pcmodels-ntsc-final/, pccg-ntsc-final/.
  Log: Documents/ge007.log. Standalone PD and Starship were removed from the 14 Pro by the user's choice
  (validated; later phone testing goes through i64 modules).
Caveats: the probe ran inside PD after PD's own allocations (16 MB heap + 32 MB ROM), so GoldenEye's heap may sit
differently: reserve the window first thing in main (port_addr.c) and log vm_region on failure. Re-probe on the Duo
(iPhone19,4, more RAM) when it's back. Next: G1 device build with this base, sign (stub profile, existing cert),
copy ROM + pcmodels/pccg sidecars to Documents, boot.

## Re-entry (R1, MacBook, 2026-11-12): start/stop several times in one process
For the i64 module (start/stop while the app keeps running). `GE_SOAK=<n> GE_SOAK_FRAMES=<f>` runs
start -> f frames -> shutdown -> restore n times in one process; `GE_SOAK_HASHLOG=<base>` (+ `GE_NET_DETERM=1
GE_RSEED=<hex>`) writes each cycle's state hashes to `<base>.<i>`.
- Threads (libultra.c `portShutdownThreads`): `g_portQuit` makes every port thread (N64 threads, tick thread,
  render worker; marked thread-local) leave at its next blocking point (osRecvMesg/osSendMesg waits, osYieldThread,
  the render worker's idle wait, the scheduler's wait for the previous frame); the shutdown keeps waking every queue
  until all are gone, then joins them (tick thread and render worker are no longer detached). Non-port threads
  (SDL audio) only see a failed call.
- Then audio/input/video (gfx_destroy + window/Metal view), ROM, genet are released; `portAddrResetWindow`
  replaces the window's 4 regions with fresh PROT_NONE reservations (VM_FLAGS_OVERWRITE, the window stays ours);
  `port/hostlib/reentry.c` restores __DATA (__data/__bss/__common) **and the __GESNAP segment** (the decomp's
  globals) to their bytes at the top of main. Survivors: GE_KEEP (__DATA,__gekeep).
- No ge007.ini save in soak mode (it would write the restored defaults).
- Result (Mac, GL): 3 x 300 frames, threads stop in ~17 ms, 7 threads after each cycle, **per-cycle hash logs
  identical** (300 distinct hashes). Footprint grows ~17 MB/cycle (renderer caches/GL objects not freed yet).

## i64 module (MacBook, 2026-11-12)
`-DGE_I64_MODULE=ON` builds `libge007_i64.dylib` exporting only `n64_game_*` (port/i64/exports.txt;
glue in port/hostlib/ge_i64_module.c, id `ge007-us`, ROM SHA-1 abe01e4a...). Mac:
`cmake -S . -B build-mac-module -G Ninja -DGE_I64_MODULE=ON -DROMID=ntsc-final -DCMAKE_C_COMPILER=clang
-DCMAKE_CXX_COMPILER=clang++ -DCMAKE_OSX_SYSROOT=$(xcrun --show-sdk-path)`, then i64's
`scripts/install-module.sh build-mac-module/libge007_i64.dylib`.
- The game keeps its own threads (N64 threads, tick, render worker) and renders by itself
  (N64_GAME_INFO_OWN_THREAD); `n64_game_frame()` only pumps SDL events on i64's main thread.
- Window: i64's SDL Metal window (`g_gfxExternalWindow`): the game adds/removes only its own Metal view and never
  resizes, moves, retitles or destroys it; Metal is forced. Audio: i64's audio_write/audio_buffered (22050 Hz).
  Data: `$S` = n64_host.data_dir (`g_sysDataDir`). No crash handler, no exit() on quit (`g_videoNoExit`).
- The address window is reserved by a constructor when i64 loads the module (at launch, before i64's heap can take
  it: matters on the iPhone, 0x3_0000_0000); image-relative pointers use the module's own header (dladdr).
- ROM import: the ROM (any byte order) is saved big-endian as `ge007.<region>.z64` in data_dir and converted
  natively; stamp `ge007.convert` = GE_CONVERT_STAMP (bump when the converter's output changes).
- Stop = the R1 shutdown + globals restore. Tested on the Mac: `I64_DATA_DIR=<d> I64_ROM_DIR=<ROMs>
  I64_TEST_BACKGROUND=1 I64_TEST_GAME=ge007-us I64_TEST_GAME_FRAMES=300 I64_TEST_GAME_RUNS=2 I64_TEST_GAME_QUIT=1
  i64.app/Contents/MacOS/i64`: import from the ROM, run, stop, run again, quit, all clean.
- Memory: `gfx_metal_release_all()` (shutdown) releases every Metal object (waits for in-flight frames, restores
  the frame semaphore's count first). Before: ~37 MB/run leaked; now flat (soak 4 cycles ~50 MB; in i64 3 runs
  60 -> 67 -> 60 MB).
- **iPhone (14 Pro, 2026-11-12): runs inside i64.** iOS module: `cmake -S . -B build-ios-module -G Ninja
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64
  -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 -DCMAKE_BUILD_TYPE=RelWithDebInfo -DROMID=ntsc-final -DGE_I64_MODULE=ON
  -DSDL2_INCLUDE_DIR=<i64>/deps-out/ios/include/SDL2 -DSDL2_LIBRARY=<i64>/deps-out/ios/lib/libSDL2-2.0.0.dylib
  -DSDL2MAIN_LIBRARY=<any libSDL2main.a (unused)>`; copy `libge007_i64.dylib` to `i64/modules/ios/`, build i64-iOS,
  install. ROM in i64's Documents (Files > i64). Test: `devicectl ... process launch --environment-variables
  '{"I64_TEST_GAME":"ge007-us","I64_TEST_GAME_FRAMES":"600","I64_TEST_GAME_RUNS":"2","I64_TEST_GAME_QUIT":"1"}'
  io.github.philosoraptor.i64`: import from the ROM on the phone, run, stop (threads 16 ms), run again, quit;
  window reserved at load (0x3_0000_0000), Metal on the A16, audio through i64, footprint 24 -> 30 MB per run.
  Not yet seen by eye or played with a controller.
- **Online through i64 (Mac pair test, 2026-11-12):** `genetInitExternal` on i64's game channel; i64 doesn't deliver
  session blobs yet, so without one genet runs its own handshake over the channel (`channelHandshake`: HELLO/WELCOME
  build check + the host's save in chunks). `n64_game_frame` forwards i64's session events (PEER_AWAY/BACK/LEFT).
  `SEQ="ge007-us:2400" EXTRA_ENV="GE_NET_BOT=1" i64/scripts/pair-test.sh` (data folders with the module + game
  data): connected as players 1/2 with i64's seed, **60/60 sync checks matched both sides**, rtt 8 ms. Host stalled
  7.8 s over 30 s (guest 0): to look at (probably the host waiting while the guest boots).
- **Mac <-> iPhone 14 Pro online through i64 (2026-11-12): IN SYNC** after a fix. Before: desyncs at varying ticks
  (661, 690, 1350...). Found with `GE_NET_HASHLOG=hash.txt` (relative = the game's data folder, also on iOS) +
  `GE_NET_HASHLOG_VALUES=1` (raw RNG/timer/stage/menu per line) + `GE_NET_RNGTRACE=<from>:<to>` (every RNG draw
  with its image-relative caller and thread): the Mac's shot made an explosion (explosionCreate) where the phone's
  hit an object (objHit), both via `bullet_path_from_screen_center`: the **window aspect and FOV settings feed the
  aim ray** (PD had the same bug). Fix (video.c): online the game sees a fixed 16:9 view at the default FOV
  settings (`simAspect/simNativeWS/simWSAuto/simFovScale`, AIWideView off, room pool without the draw-distance
  terms) and the picture is letterboxed to 16:9; Game.SkipIntro is off for the session (restored at shutdown).
  After: 2817 ticks of per-frame hashes compared, 0 mismatches; 60/60 sync checks both sides (bots shooting).
  Mac<->phone test script: session scratchpad `phonepair.sh` (local room server 7790, Mac i64 hosts, the 14 Pro
  joins via devicectl env). The module saves ge007.ini at stop (after genet has put the player's own
  settings back, before the globals reset); checked: a changed FovScale survives a run.
- **Pause:** `n64_game_set_paused` -> `g_portHostPaused`. Offline the game thread waits at its per-frame
  gfxFrameMsgQ receive (port layer); online genet's away handling (awayNow) holds and tells the other player. Both
  give way to the re-entry shutdown. Tested: `I64_TEST_PAUSE=300:3` offline, resume, stop.
