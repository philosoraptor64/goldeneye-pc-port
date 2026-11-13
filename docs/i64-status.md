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
| 4 Correctness | partial | to verify (owner) |
| 5 Determinism | done | replays |
| 6 i64 module | done | compact layout inside the arena (acf578f5); starts on the Duo |
| 7 Online (lockstep) | done | in sync Mac <-> 14 Pro |
| 8 Rollback | done | rollback in matches (lockstep outside) |
| 9 Multiplayer features | partial | multiplayer settings; remove-player option pending |
| 10 iPhone | partial | Duo/14 Pro starts; full online match on phones to verify (owner) |

Status: done / partial / not started / n/a. Updated 2026-11-13 (Studio agent; owners please correct).

Known issues / open:
- crash-safe EEPROM (0c90be60)
