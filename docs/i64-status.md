# GoldenEye 007 in i64: status

ROM: GoldenEye 007 (USA)  
Route: A, goldeneye-pc-port fork  
Owner: Studio agent (MacBook did the i64 layout)  
Process and gate definitions: https://github.com/philosoraptor64/n64net/blob/main/docs/new-game-process.md

| Gate | Status | Evidence |
|---|---|---|
| 0 Intake | done |  |
| 1 Code map | n/a |  |
| 2 Builds | done |  |
| 3 Boots and plays | done |  |
| 4 Correctness | partial | user played it on the Duo: "launches and plays great" (2026-11-13); multiplayer by eye not checked |
| 5 Determinism | done | replays |
| 6 i64 module | done | compact layout inside the arena (acf578f5); starts on the Duo |
| 7 Online (lockstep) | done | in sync Mac <-> 14 Pro |
| 8 Rollback | done | rollback in matches (lockstep outside) |
| 9 Multiplayer features | partial | multiplayer settings; remove-player option pending |
| 10 iPhone | partial | Duo (iOS 27, arena layout) and 14 Pro start and play; Mac <-> 14 Pro online in sync (2026-11-12); a two-phone online match not yet |

Status: done / partial / not started / n/a. Updated 2026-11-13 (Studio agent; corrected by the MacBook agent, owner).

Known issues / open:
- crash-safe EEPROM (0c90be60)
- host stall at session start seen once Mac <-> 14 Pro (before session blobs); not reproducible on the Mac
- no home-screen picture yet (offer_title_art)
- the long bot test (deferred by the owner)
