# Invizimals: The Resistance — Vita3K AR Emulation Project

**Game:** Invizimals: The Resistance (PCSF00510)
**Goal:** Get the game playable past AR minigames (face-confirm, wall/floor detection, Xtractor shooting)
**Work directory:** `E:\Vita3K-src` (separate from main install at `E:\Vita`)
**Game files:** `E:\Vita game` (do not modify)
**Base commit:** `900b32a` (Nov 16, 2026)

---

## How to use this file

**Start of every new session:** Read this file. It is the single source of truth for project state. Do not re-derive context from conversation history.

**End of every session:** Update `## Progress Log` and `## Next Action` with what was done and what to do next.

---

## Current Blocker

The **"Xtractor shooting" minigame** will not spawn game objects after the color-lock + button press. The game sits in a silent loop playing "sparks" audio. Root cause: **SmartAR 6DOF visual-inertial tracking cannot work in the emulator.**

### Why SmartAR can't track
- The game loads `libsmart.suprx` (Sony SmartAR) as **LLE** — the real binary runs
- SmartAR does **SceneMapping (SLAM)** — markerless 6DOF tracking combining camera image + IMU
- In the emulator the **webcam and controller/keyboard are separate physical devices** — visual motion and inertial motion don't correlate, so SmartAR can't solve the device pose
- **Camera calibration data** (lens intrinsics) is not provided by Vita3K
- **Motion calibration data** — all `SceMotionDev` functions are `UNIMPLEMENTED()` stubs (but SmartAR reads motion via `sceMotionGetState`, not SceMotionDev directly)
- Symptom: "if I move the camera everything moves with it" — AR renders in camera-relative space, not world-anchored

### Key findings from Phase 0 experiments
- **LLE SmartAR**: 2D color/surface detection ✓ (color bar fills), 6DOF tracking ✗ → stuck at sparks
- **HLE SmartAR** (all stubs return 0): 2D detection ✗ (color bar doesn't fill) — SmartAR does the detection itself
- **Full HLE is off the table** — would need to re-implement 2D feature detection, not just fake tracking
- The **hybrid approach** (LLE for detection + HLE override for tracking state) is the viable path
- Game uses **SceneMapping (SLAM)**, NOT TargetTracking. No sceSmartTargetTracking* calls at all.

### Game's SmartAR API usage (from HLE test log)
Functions the game calls (NIDs from nids.inc):
1. `sceSmartInit` (0x58979C84) — init library
2. `sceSmartSceneMappingStart` (0xA3A82B99) — start SLAM tracking
3. `sceSmartSceneMappingDispatchAndQuery` (0xF10FDDD1) — per-frame query (tracking state)
4. `sceSmartSceneMappingGetInitializationPointInfo` (0x9A578A29) — init point info (color bar)
5. `sceSmartSceneMappingGetLandmarkInfo` (0xB17057EA) — landmark info
6. `sceSmartSceneMappingPropagateResult` (0x5A9EC64D) — propagate result
7. `sceSmartSceneMappingReset` (0xC1AD43BC) — reset tracking

This minigame is **required to progress the game**, so we cannot skip it.

---

## Option A — HLE SmartAR with fake tracking (the plan)

Replace the LLE `libsmart.suprx` with an HLE implementation that stubs/forces the tracking functions to report "target found" with a stable pose, so the game spawns objects.

### Phase 0 — Cheap experiment (DONE)
- [x] Removed `SCE_SYSMODULE_SMART` from `auto_lle_modules` in `vita3k/module/src/load_module.cpp`
- [x] **GOTCHA:** Also had to extend the HLE/LLE path check in `vita3k/modules/module_parent.cpp` (line 204) to cover `app0:sce_module/` — originally only checked `vs0:sys/external/`.
- [x] Result: HLE stubs break 2D color detection (color bar doesn't fill). Full HLE is NOT viable.
- [x] Key finding: game uses SceneMapping (SLAM), not TargetTracking. 7 functions identified.

### Phase 0.5 — API reconnaissance (IN PROGRESS)
- [x] Rewrote `vita3k/modules/SceSmart/SceSmart.cpp` with parameter-logging stubs for all 7 game-called functions
- [x] Each stub logs r0-r3 and dumps 64 bytes of memory at pointer args
- [x] DispatchAndQuery logging limited to first 5 + every 60th call (avoids log spam)
- [ ] Build and run to capture function signatures
- [ ] Infer struct layouts from logged args
- [ ] Document findings in `## SmartAR API Notes`
- [x] **Recon trampoline installed (Step 1):** wraps LLE `sceSmartSceneMappingDispatchAndQuery` to dump the 96-byte result struct post-LLE. See `## Recon Trampoline (Step 1)` below.
- [ ] Run Xtractor minigame, capture log, identify tracking-state field + "locked" value

### Phase 1 — Implement hybrid HLE overrides
Vita3K has `register_hle_override()` in `vita3k/modules/taiHEN/taiHEN.cpp` (line 1299) that creates SVC stubs to redirect LLE exports to HLE functions. This is the mechanism for the hybrid approach:
- Keep LLE libsmart for 2D feature detection (color bar)
- Override specific NIDs (DispatchAndQuery, PropagateResult) with HLE that reports "tracking locked"
- May need to let LLE run first then modify output, OR fake all data from HLE

| Function | Required behavior |
|----------|-------------------|
| `sceSmartInit` | Return 0 (success) |
| `sceSmartRelease` | Return 0 |
| `sceSmartCreateInstantImageTarget` | Return fake target ID |
| `sceSmartTargetTrackingRegisterTarget` | Register fake target, return 0 |
| `sceSmartTargetTrackingStart` | Return 0 |
| `sceSmartTargetTrackingRun` / `Run2` / `RunWorker` | Return 0 (per-frame update) |
### Phase 2 — Implement fake tracking results
- [ ] Based on Phase 0.5 recon, implement HLE overrides for key functions
- [ ] Test with hybrid approach (LLE detection + HLE tracking state)
- [ ] Iterate until objects spawn

### Phase 3 — Port to main install
- [ ] Once working: copy patched `Vita3K.exe` + DLLs from `E:\Vita3K-src\build\windows-vs2022\bin\Release\` to `E:\Vita\`

---

## SmartAR API Notes

*(Fill in as structures are discovered during Phase 1)*

### Known function list (from `vita3k/modules/SceSmart/SceSmart.cpp`)
All currently `UNIMPLEMENTED()` stubs returning 0. Full list:
sceSmartCreateInstantImageTarget, sceSmartCreateLearnedImageTarget, sceSmartDestroyTarget, sceSmartGetTargetInfo, sceSmartInit, sceSmartRelease, sceSmartSceneMappingDispatchAndQuery, sceSmartSceneMappingDispatchAndQueryWithMask, sceSmartSceneMappingEnableMask, sceSmartSceneMappingFixMap, sceSmartSceneMappingForceLocalize, sceSmartSceneMappingGetInitializationPointInfo, sceSmartSceneMappingGetLandmarkInfo, sceSmartSceneMappingGetNodePointInfo, sceSmartSceneMappingGetNumInitializationPoints, sceSmartSceneMappingGetNumLandmarks, sceSmartSceneMappingGetNumNodePoints, sceSmartSceneMappingLoadMap, sceSmartSceneMappingPropagateResult, sceSmartSceneMappingRegisterTarget, sceSmartSceneMappingRemoveLandmark, sceSmartSceneMappingReset, sceSmartSceneMappingRun, sceSmartSceneMappingRunCore, sceSmartSceneMappingSaveMap, sceSmartSceneMappingSetCustomListener, sceSmartSceneMappingSetDenseMapMode, sceSmartSceneMappingSetDispatchMode, sceSmartSceneMappingStart, sceSmartSceneMappingStop, sceSmartSceneMappingUnregisterTarget, sceSmartTargetTrackingDispatchAndQuery, sceSmartTargetTrackingDispatchAndQuery2, sceSmartTargetTrackingDispatchAndQueryWithMask, sceSmartTargetTrackingGetResults, sceSmartTargetTrackingQuery, sceSmartTargetTrackingQuery2, sceSmartTargetTrackingRegisterTarget, sceSmartTargetTrackingRun, sceSmartTargetTrackingRun2, sceSmartTargetTrackingRunWorker, sceSmartTargetTrackingStart, sceSmartTargetTrackingStop, sceSmartTargetTrackingUnregisterTarget

### Structures
*(From Phase 0.5 recon — function signatures and buffer layouts inferred from logged r0-r3 + memory dumps)*

```c
// sceSmartInit(Ptr<SmartInitConfig> config, int mode, ...)
// r0 = pointer to 64-byte config struct, r1 = 1 (mode)
// Config struct at r0 contains: pointers to callbacks, float values, sizes
SmartInitConfig (64 bytes):
  0x00: uint32_t (0)
  0x04: Ptr     (callback?)
  0x08: Ptr     (callback?)
  0x0C: uint32_t (1)
  0x14: float   (0.02)
  0x20: uint32_t (1)
  0x24: Ptr     (callback?)
  0x28: uint32_t (32)
  0x2C: float   (2.0)
  0x34-0x3C: Ptr (3 pointers)

// sceSmartSceneMappingStart(int flags=5, int enable=1, ...)
// No pointers, just two int args

// sceSmartSceneMappingDispatchAndQuery(Ptr context, Ptr result, int, int)
// r0 = 0x82409030 (scene mapping context handle)
// r1 = Ptr to OUTPUT result struct (64+ bytes, pre-filled with 0xDEADBEEF)
// r2 = 0, r3 = 0
// THIS IS THE KEY FUNCTION — result struct contains tracking state + pose
SceneMappingResult (64+ bytes, pre-filled 0xDEADBEEF):
  0x00: uint32_t (0x40 = 64, likely struct size)
  0x04: Ptr     (0x824040B0, data pointer)
  0x08: uint32_t (0)
  0x0C: uint32_t (0)
  0x10: uint32_t (0x8E0002B0, pointer or special value)
  0x14: 0xCCCCCCCC (uninitialized fill)
  0x18: Ptr     (0x824050B0)
  0x1C: uint32_t (0x0DEADBEEF)
  0x20: 0xCCCCCCCC (uninitialized)
  0x24: uint32_t (0)
  0x28: uint32_t (0)
  0x2C-0x3C: 0xDEADBEEF (sentinel fill, more fields exist beyond 64 bytes)

// sceSmartSceneMappingGetInitializationPointInfo(Ptr outBuf, int bufSize, int index, ...)
// r0 = output buffer ptr, r1 = 0x40 (64 bytes), r2 = index, r3 = 0

// sceSmartSceneMappingGetLandmarkInfo(Ptr outBuf, int bufSize, Ptr context, int index)
// r0 = output ptr, r1 = 0x200 (512 bytes), r2 = context ptr, r3 = index
```

### Post-LLE capture (Session 3, Step 1) — ACTUAL struct contents

Captured 22,200 frames (~6 min) with the recon trampoline while the game sat in
the Xtractor "sparks" state (color locked, 6DOF tracking not achieved). The
64-byte result struct at r1 (fixed addr `0x8099DF98`) is **essentially static**;
only 3 fields change:

| Offset | Field | Behavior |
|--------|-------|----------|
| +0x00 | size | `0x00000040` (64) constant |
| **+0x04** | **data ptr A/B** | Alternates `0x824040B0` ↔ `0x824050C8` — double-buffered pointer to per-frame data |
| **+0x08** | **timestamp (us)** | Monotonic 0 → 0x0977A554, increments ~16666 (1/60s) |
| +0x0C | ? | `0` constant |
| **+0x10** | **state/tracking ptr A/B** | Alternates `0x8E398FA0` ↔ `0x8E3E3FB0`, in lockstep with +0x04 |
| +0x14 | (uninit) | `0xCCCCCCCC` constant |
| +0x18 | ptr | `0x824050B0` constant |
| +0x1C | ? | `0x0DEADBEE` constant (NOT the 0xDEADBEEF sentinel — LLE wrote this) |
| +0x20 | (uninit) | `0xCCCCCCCC` constant |
| +0x24, +0x28 | ? | `0` constant |
| +0x2C–0x48 | sentinels | `0xDEADBEEF` constant |
| +0x4C | ptr | `0x814FC0B1` constant |
| +0x50 | sentinel | `0xDEADBEEF` constant |
| +0x54 | ?/flags | `0x80001001` constant |
| +0x58 | ptr | `0x82404090` constant |
| +0x5C | (uninit) | `0xCCCCCCCC` constant |

**Key finding: the tracking state + pose are NOT in this 64-byte wrapper struct.**
They live in the sub-structures pointed to by **+0x04** and **+0x10** (which
alternate in lockstep — classic double-buffering: LLE writes buffer A while the
game reads buffer B, then swaps). The wrapper is just a header (size, two
double-buffered data pointers, timestamp).

**Session 3b capture (with pointer-following):** p04 (the +0x04 sub-struct) is
**all zeros across all phases** — it's the pose OUTPUT buffer, empty because
tracking never locks. p10 (the +0x10 sub-struct) is the camera image/feature
buffer (zeros when idle → uniform `0x03` during color-bar fill → real webcam
bytes `0x0F-0xFE` in sparks). p18 (+0x18) is static config (`00000005...` = the
`flags=5` from SceneMappingStart). The main struct (0-95 bytes) is fully static.

**Session 3c capture (wrapped all 7 game-called functions):**
- `GetNumInitializationPoints` (0xDF900B29) and `GetNumLandmarks` (0x2260E8BB)
  are **never called by the game** (`patched=0` — the game doesn't even import
  them). The game does NOT query counts via these.
- `GetInitializationPointInfo` (0x9A578A29) is called **10,632 times** — this is
  the game's main per-frame info query. Its **retval IS the init-point count**
  (transitions 0 → 0x1C(28) → 0x3(3) → 0x2A(42) → 0x39(57) as features are
  detected/lost). Its 64-byte buffer contains **floats = 2D pixel coordinates**
  of detected init points (e.g. 475.5, 300.5, 112.5, 282.5...), packed as
  repeated (id, x, y, 0) tuples. The color bar fills based on this count.
- `GetLandmarkInfo` (0xB17057EA) and `PropagateResult` (0x5A9EC64D) — wrapped in
  Session 3d (pending capture). Landmarks appear only when SLAM tracking locks;
  `PropagateResult` likely carries the tracking result/state to the game.

**Working hypothesis:** "tracking locked" = landmarks exist (GetLandmarkInfo
returns non-empty data). Since SLAM can't solve in the emulator, landmarks never
appear → game stays in sparks. The hybrid HLE override (Step 2) likely needs to
fake `GetLandmarkInfo` to return a valid landmark + pose, possibly also filling
the p04 pose buffer via `DispatchAndQuery`'s result.

**Session 3d capture (wrapped GetLandmarkInfo + PropagateResult):**
- `GetLandmarkInfo` (0xB17057EA): called 14,323 times. **retval is ALWAYS 0x0
  with an all-zero 512-byte buffer.** SLAM never produces landmarks. This is the
  "not locked" gate — the game polls this every frame and always gets nothing.
- `PropagateResult` (0x5A9EC64D): called 2,329 times. retval=0 (success) most
  frames, occasionally 0x808C0005. arg1=0x1E (30) constant. state@ (r0) is a
  double-buffered pointer (0x82409030 / 0x824060D8). The 64-byte `prop` buffer is
  **static across all phases** (only +0x00 timestamp changes):
  ```
  +0x00: timestamp (increasing)        +0x04: 0
  +0x08: 1  <-- candidate state enum    +0x0C: 1
  +0x10: 0  +0x14: 0  +0x18: 0
  +0x1C: 3F800000 (1.0f)               +0x20: 80000000 (candidate "invalid" flag)
  +0x24-0x3C: 0
  ```

### Step 2 — hybrid HLE override (IN PROGRESS)

**Hypothesis:** `prop` buffer +0x08 is `SceSmartSceneMappingState`
(0=NOT_INITIALIZED, 1=INITIALIZING, **2=NORMAL/tracking-locked**, 3=LOST).
Currently LLE writes 1 (INITIALIZING) because SLAM never solves. +0x20=0x80000000
is a "pose invalid" flag (bit 31). Forcing +0x08=2 and +0x20=0 should make the
game see "tracking locked" and spawn objects.

**Experiment 1 (DONE — DID NOT WORK):** In the `PropagateResult` trampoline's host
handler (`dump_propagate_result`), after LLE has already run, overwrite the prop
buffer:
- `+0x08`: 1 → 2 (force STATE_NORMAL / tracking locked)
- `+0x20`: 0x80000000 → 0 (clear "invalid" flag)

Keep all recon logging so we can observe whether:
- objects spawn (user reports)
- `GetLandmarkInfo` starts returning non-zero (would indicate the game accepted
  the fake state and tried to read landmarks)
- the prop buffer's +0x08 stays at 2 across frames or gets reset by LLE/game

**Experiment 1 result (2026-11-18):** Override applied on all 2,391
PropagateResult calls. `GetLandmarkInfo` retval stayed **0x0** on every call (no
behavior change). `GetInitializationPointInfo` count ranged 0-64 (color bar
unchanged). User confirmed: let it sit to count-of-20, **nothing spawned, no
visible change.**

**Conclusion:** `prop` buffer `+0x08` is **NOT** the field the game checks for
"tracking locked" (or +0x08 is not the state enum). The gate is elsewhere — most
likely the game checks `GetLandmarkInfo` directly for non-empty landmark data,
not a state enum in PropagateResult's buffer.

If Experiment 1 doesn't spawn objects, next steps:
- Experiment 2: also fake `GetLandmarkInfo` (retval=1 + fill 512-byte buffer with
  a minimal fake landmark — needs landmark struct layout, unknown; try position
  floats at origin)
- Experiment 3: modify the `DispatchAndQuery` result struct instead (e.g.
  +0x1C=0x0DEADBEE → a "tracking" value, or fill the p04 pose buffer)
- Experiment 4: disassemble the game binary around the GetLandmarkInfo /
  DispatchAndQuery call sites to find the exact field/branch the game checks for
  "tracking locked" (decisive but most work)

### Step 2 — Experiment 4: disassembly findings (2026-11-19)

Disassembled the game's SmartAR call sites using a capstone-based scanner
(`disassemble_smartar_callsites` in `smartar_recon.cpp`). Found 8 call sites
in Thumb mode, all in one function at `0x814FEE48` (the AR update function):

| Address | Function | Key finding |
|---------|----------|-------------|
| 0x814FEE82 | sceSmartInit | init, called once |
| 0x814FEE88 | SceneMappingStart | right after init |
| 0x814FEB12 | PropagateResult | **game checks prop[+0x08]==2 after return** |
| 0x814FEFA8 | DispatchAndQuery | retval IGNORED (game loads new r0 immediately) |
| 0x814FEFC8 | GetInitializationPointInfo | retval (count) stored at struct+0x2090 |
| 0x814FEFE8 | GetLandmarkInfo | retval stored at struct+0x794; **struct+0x54 bit 0 tested** |
| 0x814FF05C | SceneMappingStart | state transition (state!=5 path) |
| 0x814FF062 | SceneMappingReset | state transition |

**The real "tracking locked" gate** (after GetLandmarkInfo at 0x814FEFE8):
```
0x814FEFFC str.w r0, [r1, #0x794]    ; store GetLandmarkInfo retval at struct+0x794
0x814FF004 ldr.w sb, [r1, #0x54]     ; load struct+0x54
0x814FF00C b   #0x814ff03a
0x814FF03A ands sb, sb, #1           ; test bit 0 of struct+0x54
0x814FF068 cmp.w sb, #0
0x814FF06C beq #0x814ff072           ; if bit 0 == 0, SKIP critical call
0x814FF06E blx #0x8185df64           ; called ONLY if struct+0x54 bit 0 is set
```

The output buffer for GetLandmarkInfo is at `struct_base + 0x2798`, so
`struct_base = buf - 0x2798`. This lets us modify `struct+0x54` directly from
the trampoline's dump handler.

**Why Experiments 1 & 2 failed:**
- Experiment 1 (prop+0x08=2): the pose-copy path WAS entered, but the pose
  floats on the stack were empty (SLAM never produced a pose). The game copied
  an all-zero/invalid pose. AND struct+0x54 bit 0 was never set.
- Experiment 2 (fake GetLandmarkInfo retval+buffer): the retval was stored at
  struct+0x794, but struct+0x54 bit 0 was never set, so the critical call at
  0x8185df64 was always skipped.

**Experiment 2c (current):** fake retval + buffer AND set struct+0x54 bit 0
(via `struct_base = buf - 0x2798`). Also set struct+0x794=1. Combined with
Experiment 1 (prop+0x08=2) still active. This should make BOTH gates pass:
the pose-copy path (from prop+0x08=2) AND the critical function call (from
struct+0x54 bit 0).

**Hypothesis:** The game polls `GetLandmarkInfo` every frame and always gets
retval=0 / empty buffer → never sees "tracking locked". If we fake a non-zero
retval + a filled landmark buffer, the game should think SLAM has landmarks and
proceed to spawn objects.

**Implementation:**
1. Extended the generic recon trampoline to also preserve entry r2→r7 and
   r3→r8 (previously only r0/r1 were saved; LLE clobbers r0-r3). The dump
   handler now receives r0=entry r0, r1=entry r1, r2=retval, r3=entry r3
   (the landmark index for `GetLandmarkInfo`). Trampoline is now 80 bytes
   (18 instrs); push/pop changed to `{r4-r8, lr}` / `{r4-r8, pc}`. The
   "already wrapped" check was updated (`0xE92D4070` → `0xE92D41F0`).
2. **Return-value override mechanism:** the trampoline's final `mov r0, r6`
   runs *after* the SVC returns. `write_call_result` only overwrites r0, not
   r6 — so a host handler calling `cpu.set_reg(6, newval)` changes the value
   the game sees as the function's return value.
3. `dump_landmark_info` now, when LLE returned retval=0 AND index==0 AND buf
   is valid:
   - Zeros the 512-byte buffer.
   - Writes a minimal fake landmark: `+0x00` pos x=0.0f, `+0x04` pos y=0.0f,
     `+0x08` pos z=1.0f (1m ahead), `+0x0C` quality=1.0f, `+0x10` valid/id=1,
     `+0x18` secondary quality=1.0f. Rest zeros. (Exact
     SceSmartLandmarkInfo layout unknown — best-guess fill.)
   - Calls `cpu.set_reg(6, 1)` so the game sees retval=1 (one landmark).
   - For index > 0: leaves retval=0 (LLE's "no more landmarks") so the game
     stops iterating if it walks indices.
4. The PropagateResult Experiment 1 override (`prop+0x08=2`, `prop+0x20=0`)
   is still active — it was harmless (no effect) and keeping it avoids
   confounding if removing it matters. If Experiment 2 shows partial progress,
   we can toggle it.

**Unknowns / risks:**
- The landmark struct layout is guessed. If the game reads specific fields
  (e.g. a descriptor, a view direction) at offsets we left as zero, it may
  ignore the fake landmark or crash.
- Whether retval is a count (like `GetInitializationPointInfo`) or an
  error code. If it's an error code, retval=1 might mean "error" not
  "1 landmark" — in which case the game would still see failure. (Recon
  showed LLE returns 0 = "no landmarks"; treating non-zero as "landmarks
  exist" is the best inference.)
- Whether the game iterates landmarks by index (needs per-index handling)
  or reads them all from one call. We handle both: index 0 → fake, index >0
  → empty.

**Test status:** AWAITING user run — load the Xtractor minigame, let it
color-lock, and observe whether objects spawn. Check `vita3k.log` for
`EXPERIMENT2: faked 1 landmark` lines.

**Test result (2026-11-19):** NOTHING SPAWNED. Root cause: the `index == 0`
gate never fired — the game passes r3=8336 (0x2090) on EVERY GetLandmarkInfo
call (constant, not a per-landmark index). The game does NOT iterate landmark
indices; it calls GetLandmarkInfo with fixed args every frame. 0 EXPERIMENT2
lines in the log. Color-lock worked (init-point count progressed 0→64).
**Fix applied:** removed the `index == 0` condition — now always fakes when
LLE returns retval=0 with a valid buffer. Rebuilt; awaiting re-test.

**Open question:** which sub-struct (+0x04 or +0x10) holds the tracking state,
and at what offset? Step 1b extends the dump to follow these pointers (128 bytes
each) and captures the **idle → color-bar-filling → locked transition** (the
Session 3 capture was steady-state only, so no state field visibly transitioned).

### What we still don't know
- Which field in SceneMappingResult is the tracking state enum (not tracking / initializing / tracking / lost)
- Which fields are the pose (position + orientation matrix)
- What values represent "tracking locked"
- The layout of InitializationPointInfo and LandmarkInfo structs
- Whether the game validates pose consistency across frames

### HLE/LLE selection mechanism
- `vita3k/module/src/load_module.cpp` line 142-162: `auto_lle_modules` list
- Line 159: `SCE_SYSMODULE_SMART` — remove this to force HLE
- `is_lle_module()` (line 164): in AUTOMATIC mode, returns true if module in `auto_lle_modules`
- Line 87: `p[SCE_SYSMODULE_SMART] = { "libsmart" };` — maps sysmodule to HLE module name

---

## Completed Work (DO NOT revert)

### Atrac9 audio hang fix (v5) — KEEP
**File:** `vita3k/ngs/src/modules/atrac9.cpp`
On Atrac9 decode error, advances `current_byte_position_in_buffer = bufparam.bytes_count` so the NGS voice reaches end-of-data and the game advances. This is the ONLY audio patch kept. All libatrac9 decoder modifications were reverted (caused crashes).

### Motion/orientation system — KEEP
- Keyboard numpad fake motion + DualShock real motion
- "Motion Input Source" dropdown in Controls settings (Auto/Controller/Keyboard)
- `motion_input_mode` stored on `MotionState`, synced from config at init + on GUI change

**Files modified:**
- `vita3k/motion/include/motion/state.h` — `fake_acceleration` (Util::Vec3f) + `motion_input_mode` (int)
- `vita3k/motion/include/motion/functions.h` — `set_fake_orientation()` decl
- `vita3k/motion/src/motion.cpp` — fake orientation impl, feeds fake accel when keyboard mode active (16667us delta ~60Hz)
- `vita3k/modules/SceDriverUser/SceMotion.cpp` — local `is_motion_active()` helper
- `vita3k/config/include/config/config.h` — `motion-input-mode` field (0=Auto, 1=Controller, 2=Keyboard)
- `vita3k/gui-qt/src/controls_dialog.cpp` — Motion Input Source QComboBox
- `vita3k/gui-qt/src/ctrl_keyboard_filter.cpp` — numpad handling (Qt frontend)
- `vita3k/sdl-frontend/src/session.cpp` — numpad handling (SDL frontend)
- `vita3k/app/src/app_init.cpp` — syncs `motion.motion_input_mode` from config
- `vita3k/display/src/display.cpp` — `refresh_motion(emuenv.motion, emuenv.ctrl)` call

### What works (verified by user)
- Past camera face-confirm ✓
- Past wall detection ✓
- Into Xtractor minigame, color-lock works ✓
- Audio plays (sparks) ✓

### Numpad controls (NumLock ON)
5=flat, 8=upright(wall), 2=face-down, 4/6=landscape, 7/9=upright-tilted, 1/3=flat-tilted

---

## Build & Test

### Build commands
```
# MUST stop Vita3K.exe first (LNK1104 if locked)
Stop-Process -Name Vita3K -Force

# Incremental build (background):
cmd /c C:\Users\rianp\AppData\Local\Temp\opencode\build_wrapper.bat
# Output: build_log.txt
```
- CMake preset: `windows-vs2022`
- Qt6_ROOT: `E:\Qt\6.11.3\msvc2022_64`
- MSVC toolset: 14.44.35207
- Windows SDK: 10.0.26100.0
- vcvarsall: `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat x64`

### Build output
`E:\Vita3K-src\build\windows-vs2022\bin\Release\Vita3K.exe`

### Test log
`E:\Vita3K-src\build\windows-vs2022\bin\Release\vita3k.log` (NOT `E:\Vita\vita3k.log`)

### Environment
- VS 2022 Community + Qt 6.11.3
- User controller: DualShock (motion-capable), connected
- Webcam working for face/wall/color detection
- Atrac9 decode errors (0x82100007 etc.) appear throughout but handled silently by v5 patch

---

## Progress Log

### Session 1-? (prior to this file)
- Atrac9 v5 fix implemented and tested — past face-confirm
- Motion system (keyboard + DualShock + GUI) implemented and built — past wall detection
- Camera color-detection lock works in Xtractor minigame
- Diagnosed SmartAR 6DOF tracking as the blocker for object spawning
- User confirmed: keeping webcam+controller still, and moving them together, both don't work
- User confirmed: AR objects follow camera (not world-anchored) — confirms no 6DOF pose

### Session 2 (Phase 0 + 0.5 + git setup)
- Phase 0: forced HLE for SmartAR via auto_lle_modules removal + app0 path check fix
- **Result: HLE stubs break color detection. Full HLE not viable.**
- Key finding: game uses SceneMapping (SLAM), not TargetTracking. 7 functions identified with NIDs.
- Phase 0.5: recon build with parameter-logging SceSmart stubs
- **Recon results (captured function signatures):**
  - sceSmartInit(Ptr config, int mode) — config is 64-byte struct with callbacks/params
  - sceSmartSceneMappingStart(int flags=5, int enable=1)
  - sceSmartSceneMappingDispatchAndQuery(Ptr ctx, Ptr result, int, int) — result is 64+ byte struct pre-filled with 0xDEADBEEF. KEY FUNCTION.
  - sceSmartSceneMappingGetInitializationPointInfo(Ptr out, int size=64, int index)
  - sceSmartSceneMappingGetLandmarkInfo(Ptr out, int size=512, Ptr ctx, int index)
- Reverted all experimental changes (load_module.cpp, module_parent.cpp, SceSmart.cpp back to original)
- Only working patches remain: Atrac9 fix + motion system (11 files)
- Git repo created: https://github.com/RadonFox/VIta3k-Invizamals-Resistance-patch (private)
- Pushed 3 branches: `master` (baseline), `invizimals-ar-patches` (working patches + docs), `hybrid-hle-override` (next step, empty)
- Added SUMMARY.md (executive summary) and confirmed GPLv2 license is present (COPYING.txt)
- **Current branch: `hybrid-hle-override`** — ready for the hybrid approach implementation

### Session 3 (Recon trampoline → Step 2 Experiment 1)
- Implemented guest-ARM trampoline that wraps LLE `sceSmartSceneMappingDispatchAndQuery` (NID 0xF10FDDD1) to capture the post-LLE `SceneMappingResult` struct contents
- **Bug fixed:** game loads `libsmart.suprx` from `app0:sce_module/` via `load_module` — NOT via `sceSysmoduleLoadModule(SCE_SYSMODULE_SMART)`. Moved the hook trigger from SceSysmodule into `load_module` (gated on path containing "libsmart"). Added idempotency guard.
- Iteratively extended recon across 4 capture cycles to wrap all 7 game-called SmartAR functions and follow sub-struct pointers
- **Complete recon picture (see `## SmartAR API Notes` → "Post-LLE capture"):**
  - `DispatchAndQuery` result struct = static header (size, double-buffered data ptrs +0x04/+0x10, timestamp +0x08). p04 = pose output buffer (all zeros, never populated). p10 = camera image buffer. p18 = static config (flags=5).
  - `GetInitializationPointInfo` retval = init-point count (0→28→3→42→57); buffer = float 2D pixel coords. Drives the color bar.
  - `GetNumInitializationPoints` / `GetNumLandmarks` — **never called by the game** (not imported).
  - `GetLandmarkInfo` — always retval=0, empty 512-byte buffer. **The "not locked" gate.**
  - `PropagateResult` — retval=0 mostly; `prop` buffer static identity-ish pose (+0x08=1, +0x1C=1.0f, +0x20=0x80000000).
- **Step 2 Experiment 1 (DID NOT WORK):** forced `prop+0x08=2` (STATE_NORMAL) + `prop+0x20=0` in the PropagateResult trampoline after LLE ran. Override applied on all 2,391 calls, but `GetLandmarkInfo` stayed 0 and nothing spawned. `prop+0x08` is not the gate the game checks.
- **Files added:** `vita3k/modules/SceSmart/smartar_recon.{h,cpp}`
- **Files modified:** `vita3k/modules/CMakeLists.txt`, `vita3k/modules/SceSysmodule/SceSysmodule.cpp`, `vita3k/modules/module_parent.cpp`
- Git: 5 commits on `hybrid-hle-override` (recon trampoline + 3 recon extensions + Step 2 exp1), ready to push
- **Status: PAUSED for testing.** Next is Step 2 Experiment 2 (fake `GetLandmarkInfo` retval + buffer) or Experiment 4 (disassemble game binary to find the exact "tracking locked" check).

### Session 4 (Step 2 Experiment 2 implementation)
- Extended the generic recon trampoline to preserve entry r2→r7, r3→r8 (LLE
  clobbers r0-r3); dump handlers now receive r3=entry r3 (landmark index).
  Trampoline grew from 64→80 bytes; push/pop encodings updated.
- Confirmed the return-value override mechanism: trampoline's final `mov r0, r6`
  runs after the SVC returns, and `write_call_result` only overwrites r0 — so
  `cpu.set_reg(6, newval)` in a host handler changes the game's visible retval.
- Implemented Experiment 2 in `dump_landmark_info`: when LLE returns retval=0
  with index==0, zero the 512-byte buffer, write a minimal fake landmark
  (pos (0,0,1), quality 1.0, valid flag), and set retval→1 via `set_reg(6,1)`.
  index>0 left as retval=0 (no more landmarks).
- PropagateResult Experiment 1 override kept active (harmless).
- Build: clean (exit 0), `Vita3K.exe` rebuilt 2026-11-19.
- **Status: AWAITING TEST.** Next is a user run of the Xtractor minigame to see
  if objects spawn.

### Session 5 (Experiment 2c/2d + full disassembly)
- Experiment 2 (fake GetLandmarkInfo retval+buffer): first run had a bug —
  `index == 0` gate never fired (game passes r3=8336=0x2090 every call, a
  struct offset, NOT a landmark index). Fixed: removed the index condition.
  Re-tested: override fired (10,448 EXP2c lines) but nothing spawned.
- Experiment 2c: set `[struct+0x2000+0x54]` bit0 via `struct_base = buf - 0x2798`
  (corrected from the wrong `+0x54` offset — off by 0x2000). Re-tested:
  override fired at correct address (6,069 EXP2c lines, `+0x2054: 0x1->0x1`).
  `sceSmartSceneMappingRunCore` IS called now, but it's LLE SLAM processing
  — runs but can't solve tracking in the emulator. Nothing spawned.
- Experiment 2d: filled DispatchAndQuery result struct `+0x10..+x28` with an
  identity pose (7 floats). **CRASHED** — LLE DispatchAndQuery reads back the
  `+0x10` (p10 pointer) on the next call; overwriting with a float
  broke it. Reverted.
- Built a capstone-based disassembly scanner (`disassemble_smartar_callsites`)
  that finds all SmartAR call sites in the game's code segment and
  dumps the full annotated disassembly. Found 8 call sites in Thumb
  mode, all in one function cluster (`0x814FEB12`–`0x814FF062`).
- Identified the critical function `0x8185df64` =
  `sceSmartSceneMappingRunCore` (NID 0xCB30BD4D) — the core SLAM
  processing, called only when `[struct+0x2000+0x54]` bit0 is set.
- Mapped the complete per-frame call flow, per-camera struct offsets,
  gates, and the two-function architecture (Function A = pose extractor,
  Function B = main AR loop). See `STRUCTURE.md` for the full reference.
- Created `STRUCTURE.md` (structure reference) and `disasm_full.txt`
  (raw disassembly).
- **Files modified:** `vita3k/modules/SceSmart/smartar_recon.cpp`
- **Files added:** `STRUCTURE.md`, `disasm_full.txt`
- **Status: STALLED.** All faking approaches hit the fundamental limitation
  that LLE SLAM can't solve in the emulator. Next options documented in
  `STRUCTURE.md` and `## Next Action`.
- Extended the generic recon trampoline to preserve entry r2→r7, r3→r8 (LLE
  clobbers r0-r3); dump handlers now receive r3=entry r3 (landmark index).
---

## Next Action

**Step 2 — Hybrid HLE override: STALLED (fundamental limitation hit)**

All faking approaches tried (Experiments 1, 2,2c,2d) failed. See
`STRUCTURE.md` for the complete disassembly findings and the remaining
options. The fundamental problem: the LLE SLAM algorithm (`RunCore` +
`DispatchAndQuery`) cannot produce a valid 6DOF pose in the emulator, and
faking individual outputs doesn't work because the LLE code needs its
internal data structures (pointers) to be consistent.

**Remaining options (see STRUCTURE.md "Possible next approaches"):**
1. HLE-override `RunCore` (NID 0xCB30BD4D) — skip SLAM entirely, return
   success. Risk: `RunCore` might be what sets `[r8+9]` (done flag).
2. Fill the p04 sub-buffer (pose output, pointed to by result+0x04)
   with an identity pose, without touching the result struct pointer fields.
3. Provide camera calibration + sync IMU with camera motion so SLAM can
   actually solve. Much bigger task.
4. Disassemble Function A fully (starts before our current dump
   range) to understand where the 7 "pose floats" on its stack come from when
   tracking IS locked.

---

## Recon Trampoline (Step 1)

**Goal:** See what LLE libsmart writes into the `SceneMappingResult` struct each frame, so we can identify the tracking-state field and its "locked" value.

**Why a trampoline (not HLE override):** `register_hle_override` replaces the export with an SVC stub that runs HLE instead of LLE — LLE never runs, so 2D detection breaks. The trampoline calls the original LLE first (2D detection still works), then traps to the host to dump/modify the output.

**How it works:**
1. `sceSysmoduleLoadModule(SCE_SYSMODULE_SMART)` loads LLE libsmart. `load_func_exports` (load_self.cpp:278) rewrites the game's import stubs for NID 0xF10FDDD1 from SVC form to `MOVW/MOVT/BX r12 → LLE`.
2. `install_smartar_recon_hook` (called right after) allocates:
   - A 12-byte dump SVC stub: `svc #0; mov pc, lr; 0xDEAD0001`
   - A 64-byte ARM trampoline (11 instructions + NOPs)
3. It re-patches every game import stub that currently targets the LLE address to target the trampoline instead.
4. Trampoline flow (AAPCS-correct, r4–r6 saved):
   ```
   push {r4-r6, lr}
   mov  r4, r1          ; r4 = result ptr
   movw/movt r12, lle   ; call original LLE
   blx  r12
   mov  r6, r0          ; save LLE return value
   mov  r1, r4          ; r1 = result ptr for dump
   movw/movt r12, dump  ; trap to host dump handler
   blx  r12
   mov  r0, r6          ; restore LLE return value
   pop  {r4-r6, pc}     ; return to game
   ```
5. `call_import` (module_parent.cpp) special-cases NID 0xDEAD0001 → `smartar_recon_dump_result`, which logs 96 bytes of the struct at r1 (first 5 frames + every 60th).

**Log output format:**
```
SmartAR recon: installed trampoline at ... wrapping LLE DispatchAndQuery at ... (dump SVC at ...); patched N import stub(s)
SmartAR recon [frame 1] SceneMappingResult @ 0x... (r0=0x...)
  +0x00: ...
  +0x10: ...
  +0x20: ...
  +0x30: ...
  +0x40: ...
  +0x50: ...
```
