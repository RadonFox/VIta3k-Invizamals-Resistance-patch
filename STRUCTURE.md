# SmartAR Internal Structure (from disassembly)

Disassembled the game's AR code (eboot module, Thumb mode) using a capstone-based
scanner built into `smartar_recon.cpp`. This file documents the structures,
call flow, and gates discovered. Full raw disassembly is in `disasm_full.txt`.

---

## Function architecture

There are **two separate functions** in the game's code that handle AR:

### Function A — Pose Extractor (`0x814FEA92`–`0x814FEB64`)
Small function that calls `PropagateResult` and extracts the pose.
- Stack frame: `sub sp, #0x84` (0x84 = 132 bytes)
- `r4` = pointer to the game's AR object/state struct
- `r5` = pointer to a sub-struct (`r8 + 0x7ef4`)
- Calls `PropagateResult(r0 = r5+8, ...)` at `0x814FEB12`
- **State check:** `ldr r0, [r5, #0x10]; cmp r0, #2; bne skip` — checks
  `[r5+0x10] == 2` (STATE_NORMAL / tracking locked)
- **Pose copy:** if state==2, copies 7 floats from stack (`sp+0x18`..`sp+0x30`)
  to the game struct at `r4+0x50`..`r4+0x6c`
- Returns immediately after the pose copy (stack canary check + pop)

### Function B — Main AR Update Loop (`0x814FEE48`–`0x814FF0C2`)
The per-frame AR update function. This is where all the SmartAR
per-frame calls happen.
- Stack frame: `sub sp, #0x2c`
- `r8` = AR system base pointer (function arg 0)
- `fp = r8 + 0x7e00`
- `sl = r8 + 0x1020` (state machine pointer)
- **Init phase:** sets `[r8+9] = 0` (clear "done" flag), loads libsmart
  (`sceSysmoduleLoadModule(0x39)`), calls `sceSmartInit`, `sceSmartSceneMappingStart(5)`
- **Main loop** (`0x814FEF02`): each iteration:
  1. Check `[r8+9]` — if non-zero, jump to cleanup (minigame done)
  2. Time calculation (floats s16, s17)
  3. Call game function `0x814fc004` (update something with r5)
  4. Compute per-camera struct base: `(1-[fp+0xf0]) * 0x2f58 + r8`
  5. **State gate:** `ldr r7, [sb+0x1020]; cmp r7, #5` — if state != 5,
     skip the SmartAR calls (branch to `0x814FF00E`)
  6. If state == 5: call `DispatchAndQuery` → `GetInitializationPointInfo` →
     `GetLandmarkInfo` → merge → `RunCore` gate → loop back

---

## Per-camera struct (`0x2F58` bytes each)

The game maintains an array of per-camera structs, each `0x2F58` (12,120) bytes,
indexed by a camera number derived from `[fp+0xf0]`. Base computed as:
```
struct_base = (1 - [fp+0xf0]) * 0x2F58 + r8
```

Known offsets (all relative to `struct_base`):

| Offset | What | Source |
|--------|------|--------|
| `+0x2090` | init-point count (from `GetInitializationPointInfo` retval) | `str r0, [r2, 0x2090]` at `0x814FEFE0` |
| `+0x2094` | `GetInitializationPointInfo` output buffer (64 bytes) | `adds r0, r0, 0x2094` at `0x814FEFBC` |
| `+0x2798` | `GetLandmarkInfo` output buffer (512 bytes) | `adds r0, r2, 0x2798` at `0x814FEFE6` |
| `+0x2048` | SceneMapping context handle | `add r0, 0x2048` at `0x814FEF86` |

### Offsets relative to `struct_base + 0x2000` (the "+0x2000" sub-struct)

The game also uses a second sub-struct at `struct_base + 0x2000`. This is
where the tracking state and landmark count are stored/read:

| Offset from `struct_base` | Offset from `struct_base + 0x2000` | What | Source |
|--------|--------|------|--------|
| `+0x0054` | `+0x2004` | ?? (not yet identified) | |
| `+0x0040` | `+0x2040` | float `s16` (time-derived value, stored each frame) | `vstr s16, [r0, 0x40]` at `0x814FF02E` |
| `+0x0054` | `+0x2054` | **tracking state — THE GATE** (bit 0 tested) | `ldr.w sb, [r0, 0x54]` at `0x814FF032` |
| `+0x0090` | `+0x2090` | zeroed each frame (in state!=5 path) | `str.w r4, [r0, 0x90]` at `0x814FF02A` |
| `+0x0794` | `+0x2794` | **landmark count** (from `GetLandmarkInfo` retval) | `str.w r0, [r1, 0x794]` at `0x814FEFFC` |

**Critical:** `+0x2054` bit 0 is the gate that controls whether
`sceSmartSceneMappingRunCore` is called:
```
0x814FF03A ands sb, sb, #1          ; sb = [struct+0x2000+0x54] & 1
0x814FF068 cmp.w sb, #0
0x814FF06C beq #0x814ff072         ; if bit0 == 0, skip
0x814FF06E blx sceSmartSceneMappingRunCore ; ← called ONLY if bit0 set
```

---

## DispatchAndQuery result struct (64 bytes, on stack)

Passed as `r1` to `sceSmartSceneMappingDispatchAndQuery`. Pre-filled with
`0xDEADBEEF` sentinel by the game. Layout from recon:

| Offset | Type | Contents (recon) |
|--------|------|--------------------|
| `+0x00` | `uint32` | `0x40` (64) — struct size |
| `+0x04` | `Ptr` | p04 — pose output buffer (all zeros, never populated) |
| `+0x08` | `uint32` | timestamp (monotonic, increments ~16666 = 1/60s) |
| `+0x0C` | `uint32` | `0` |
| `+0x10` | `Ptr` | p10 — camera image/feature buffer |
| `+0x14` | `uint32` | `0xCCCCCCCC` (uninit) |
| `+0x18` | `Ptr` | `0x824050B0` (constant) |
| `+0x1C` | `uint32` | `0x0DEADBEE` (NOT the 0xDEADBEEF sentinel) |
| `+0x20` | `uint32` | `0xCCCCCCCC` (uninit) |
| `+0x24` | `uint32` | `0` |
| `+0x28` | `uint32` | `0` |
| `+0x2C`–`+0x48` | — | `0xDEADBEEF` sentinel fill |
| `+0x4C` | `Ptr` | `0x814FC0B1` (constant) |
| `+0x50` | `uint32` | `0xDEADBEEF` |
| `+0x54` | `uint32` | `0x80001001` (constant) |
| `+0x58` | `Ptr` | `0x82404090` (constant) |
| `+0x5C` | `uint32` | `0xCCCCCCCC` (uninit) |

**Double-buffering:** `+0x04` and `+0x10` alternate between two
addresses each frame (A↔B), in lockstep — classic double-buffering
(LLE writes buffer A while game reads B, then swaps).

**WARNING:** The LLE `DispatchAndQuery` reads back `+0x04` (p04) and
`+0x10` (p10) pointers on subsequent calls. Overwriting these fields
with pose floats **crashes** the game (Experiment 2d). These pointer
fields cannot be overwritten.

---

## PropagateResult prop buffer

`PropagateResult` is called with `r0 = r5 + 8` (prop buffer at
`r8 + 0x7ef4 + 8 = r8 + 0x7efc`). Layout (from recon):

| Offset | Type | Contents (recon) |
|--------|------|--------------------|
| `+0x00` | `uint32` | timestamp (increasing) |
| `+0x04` | `uint32` | `0` |
| `+0x08` | `uint32` | **state enum** (1=INITIALIZING, 2=NORMAL/locked) — Experiment 1 forces this to 2 |
| `+0x0C` | `uint32` | `1` |
| `+0x10` | — | (this is `[r5+0x10]` checked by Function A) |
| `+0x14` | — | |
| `+0x18` | — | |
| `+0x1C` | `float` | `1.0f` (`0x3F800000`) |
| `+0x20` | `uint32` | `0x80000000` (bit 31 = "pose invalid" flag) — Experiment 1 clears to 0 |

### SceSmartSceneMappingState enum (hypothesis)
```
0 = NOT_INITIALIZED
1 = INITIALIZING    ← LLE writes this (SLAM never solves)
2 = NORMAL          ← "tracking locked" — Experiment 1 forces this
3 = LOST
```

---

## Import stub table

All SmartAR import stubs are at `0x8185Dxxx`, spaced `0x10` (16) bytes apart:

| Stub address | NID | Function |
|-------------|------|----------|
| `0x8185DC94` | `0x?` | `sceSysmoduleUnloadModule` |
| `0x8185DCA4` | `0x?` | `sceSysmoduleLoadModule` |
| `0x8185DEE4` | `0x?` | `sceSmartRelease` |
| `0x8185DEF4` | `0x?` | `sceSmartSceneMappingStop` |
| `0x8185DF04` | `0x58979C84` | `sceSmartInit` |
| `0x8185DF14` | `0x5A9EC64D` | `sceSmartSceneMappingPropagateResult` |
| `0x8185DF24` | `0x9A578A29` | `sceSmartSceneMappingGetInitializationPointInfo` |
| `0x8185DF34` | `0xA3A82B99` | `sceSmartSceneMappingStart` |
| `0x8185DF44` | `0xB17057EA` | `sceSmartSceneMappingGetLandmarkInfo` |
| `0x8185DF54` | `0xC1AD43BC` | `sceSmartSceneMappingReset` |
| `0x8185DF64` | `0xCB30BD4D` | **sceSmartSceneMappingRunCore** |
| `0x8185DF74` | `0xF10FDDD1` | `sceSmartSceneMappingDispatchAndQuery` |
| `0x8185DE84` | (not SmartAR) | used elsewhere in AR code |
| `0x8185DEE4` | `0x?` | `sceSmartRelease` |
| `0x8185DFE4` | (not SmartAR) | used in init path |
| `0x8185E384` | (not SmartAR) | `memset` (called in state!=5 path) |
| `0x8185E984` | — | `__stack_chk_fail` |

---

## Per-frame call flow (Function B, state == 5 path)

```
                    ┌─────────────────────────────────────────┐
                    │ [r8+9] == 0?                          │
                    │   NO → cleanup (minigame done)            │
                    │   YES → continue                        │
                    └───────────────┬──────────────────────────┘
                                   │
                    ┌──────────────▼──────────┐
                    │ time calc (s16, s17)    │
                    │ call 0x814fc004 (update) │
                    └───────────────┬──────────┘
                                   │
                    ┌──────────────▼──────────────────────┐
                    │ state = [sb+0x1020]                │
                    │ state != 5?  YES → skip (state!=5 path) │
                    │ state == 5?  continue                 │
                    └───────────────┬───────────────────────┘
                                   │
                    ┌──────────────▼──────────────────────┐
                    │ DispatchAndQuery(r0=ctx,       │
                    │   r1=result@sp+8, r2, r3)     │
                    │   retval IGNORED                   │
                    └───────────────┬───────────────────────┘
                                   │
                    ┌──────────────▼──────────────────────┐
                    │ GetInitializationPointInfo        │
                    │   (r0=struct+0x2094, r1=64)   │
                    │   retval = init point count           │
                    │   → stored at [struct+0x2090]       │
                    └───────────────┬───────────────────────┘
                                   │
                    ┌──────────────▼──────────────────────┐
                    │ GetLandmarkInfo                     │
                    │   (r0=struct+0x2798, r1=512,  │
                    │    r2=struct, r3=0x2090)          │
                    │   retval = landmark count             │
                    │   → stored at [struct+0x2000+0x794]  │
                    │   → load sb = [struct+0x2000+0x54]   │
                    └───────────────┬───────────────────────┘
                                   │
                    ┌──────────────▼──────────────────────┐
                    │ MERGE POINT (both paths arrive)     │
                    │   state!=5: memset 72 bytes;        │
                    │            zero struct+0x2000+0x90;  │
                    │            store s16 at +0x2040;       │
                    │            RELOAD sb = [+0x2054];      │
                    │            zero [+0x2794]             │
                    │   ands sb, sb, #1  ← THE GATE        │
                    └───────────────┬───────────────────────┘
                                   │
                    ┌──────────────▼──────────────────────┐
                    │ call 0x814fc012 (game func)     │
                    │ state transition (Start/Stop/Reset) │
                    │ cmp.w sb, #0                          │
                    │   sb==0 → skip                       │
                    │   sb!=0 → call RunCore             │
                    └───────────────┬───────────────────────┘
                                   │
                    ┌──────────────▼──────────────────────┐
                    │ [r8+9] == 0?                      │
                    │   YES → loop back to top             │
                    │   NO  → cleanup (done)             │
                    └───────────────────────────────────────┘
```

---

## Key findings summary

1. **`[r8+9]` is the "minigame done" flag.** Set to 0 at init. Checked
   each frame. If non-zero → cleanup (stop SmartAR, release, unload, return).
   What sets it is unknown (possibly RunCore when tracking locks, or a
   callback, or Function A).

2. **`[sb+0x1020]` is the state machine value.** Compared to 5.
   If != 5, the SmartAR per-frame calls are skipped (only memset + zeroing).

3. **`[struct+0x2000+0x54]` bit 0 is the RunCore gate.** If bit 0 is 0,
   `RunCore` is skipped. If bit 0 is 1, `RunCore` is called. We can set this
   (Experiment 2c) but RunCore is LLE SLAM processing — it runs but
   can't solve tracking in the emulator.

4. **`[struct+0x2000+0x794]` is the landmark count.** Stored from
   `GetLandmarkInfo` retval. We can set this (Experiment 2c).

5. **DispatchAndQuery retval is IGNORED.** The game loads a new r0
   immediately after the call.

6. **`GetInitializationPointInfo` retval = init-point count** (drives
   the color bar). Stored at `[struct+0x2090]`.

7. **`GetLandmarkInfo` r3 = 0x2090** (a struct offset, NOT a landmark
   index — it's constant 8336 every frame).

8. **PropagateResult `[r5+0x10]` = state enum.** Checked == 2 by
   Function A. Experiment 1 forces this to 2, which makes Function A copy
   the pose — but the "pose" it copies (7 floats from its own stack)
   are the raw DispatchAndQuery result struct fields (pointers/sentinels),
   not real pose data. Overwriting those pointer fields crashes the LLE
   DispatchAndQuery on the next call.

9. **`sceSmartSceneMappingRunCore` (NID 0xCB30BD4D)** is the core
   SLAM processing function. Called only when `+0x2054` bit0 is set.
   It runs the actual SLAM algorithm, which can't solve in the emulator
   (webcam and controller are separate devices; no camera calibration).

---

## What we've tried and why it failed

| Experiment | What | Result | Why |
|-----------|-------|--------|-----|
| Exp 1 | Force `prop+0x08 = 2` (state=NORMAL) | No effect | Function A copies "pose" from stack, but those are pointers/sentinels (garbage as floats), not real pose. Pose is invalid. |
| Exp 2 | Fake `GetLandmarkInfo` retval=1 + buffer | No effect | Retval stored at `+0x2794` but the gate is `+0x2054` bit0, which was never set |
| Exp 2c | Set `+0x2054` bit0 (RunCore gate) | No effect | RunCore IS called now, but it's LLE SLAM processing — runs but can't solve tracking |
| Exp 2d | Fill DispatchAndQuery result struct `+0x10..+x28` with identity pose | **CRASH** | LLE DispatchAndQuery reads back `+0x10` (p10 pointer) on next call; overwriting with float breaks it |

### The fundamental problem
The SLAM algorithm (`RunCore` + `DispatchAndQuery`) cannot produce a valid
6DOF pose in the emulator because:
- The webcam and controller are separate physical devices (visual and
  inertial motion don't correlate)
- No camera calibration data (lens intrinsics) is provided by Vita3K

Faking individual outputs (state, landmark count, gates) doesn't work because
the game's state machine is complex and the LLE code needs its internal
data structures (pointers) to be consistent.

### Possible next approaches
1. **HLE-override `RunCore` (NID 0xCB30BD4D)**: skip SLAM entirely,
   return success. Risk: `RunCore` might be what sets `[r8+9]` (done flag); if
   it doesn't run, the minigame might loop forever.
2. **Fill the p04 sub-buffer** (pose output, pointed to by result+0x04)
   with an identity pose, without touching the result struct pointer fields.
   The game might read from p04 somewhere we haven't found yet.
3. **Provide camera calibration + sync IMU with camera motion** so SLAM
   can actually solve. Much bigger task.
4. **Disassemble Function A** fully (it starts before our current dump
   range) to understand where the 7 "pose floats" on its stack come from
   when tracking IS locked.
