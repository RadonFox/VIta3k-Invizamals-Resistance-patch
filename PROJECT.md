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

---

## Next Action

**Step 1 — Capture LLE SmartAR's actual output (the key unknown):**
We need to see what the LLE `sceSmartSceneMappingDispatchAndQuery` writes into its result struct when the color bar fills (but tracking hasn't locked). This tells us which fields change and what the "not yet locked" tracking state value is. Approach:
1. Keep LLE libsmart (reverted to original — already done)
2. Add a host-side hook that, after `sceSmartSceneMappingDispatchAndQuery` returns, dumps the result struct at r1
3. This requires either:
   - A guest-level return hook (taiHEN-style "after" hook), OR
   - An HLE wrapper that calls the original LLE function then dumps/modify the output
4. Compare the struct contents across frames to identify the tracking state field

**Step 2 — Override the tracking state to "locked":**
Once we know which field is the tracking state and what "locked" looks like:
1. Use `register_hle_override()` (taiHEN.cpp line 1299) to redirect `sceSmartSceneMappingDispatchAndQuery` (NID 0xF10FDDD1) to an HLE wrapper after libsmart loads
2. The wrapper calls the original LLE function (for feature detection / color bar), then overwrites the tracking state field with "locked"
3. May also need to fill pose fields (position + orientation) with a stable identity transform

**Step 3 — Test and iterate:**
Run the Xtractor minigame, see if objects spawn. Adjust fake data as needed.

**Key files for this work:**
- `vita3k/modules/taiHEN/taiHEN.cpp` — `register_hle_override()` at line 1299
- `vita3k/modules/SceSmart/SceSmart.cpp` — HLE stubs (currently original UNIMPLEMENTED)
- `vita3k/module/src/load_module.cpp` — module loading, auto_lle_modules list
- `vita3k/modules/module_parent.cpp` — HLE/LLE path check (line 204)
- `vita3k/kernel/src/load_self.cpp` — export_nids registration (for understanding the hook mechanism)
