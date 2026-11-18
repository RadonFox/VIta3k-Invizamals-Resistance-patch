# Executive Summary

## Project: Invizimals: The Resistance on Vita3K

**Status:** Partially working — past face-confirm, wall detection, and camera color-lock. Blocked at the Xtractor shooting minigame (objects won't spawn). Recon of the SmartAR API is complete; first hybrid HLE override attempt did not work; next attempt planned.

---

## What works

- **Atrac9 audio hang fix** — game was hanging at audio-gated scenes (face-confirm). Fixed by skipping the audio buffer on decode error so the NGS voice reaches end-of-data and the game advances. Patched in `ngs/src/modules/atrac9.cpp`.
- **Motion/orientation system** — keyboard numpad fake motion + DualShock real motion, with a GUI dropdown to switch. Lets the user pass orientation-based AR checks (wall detection).
- **Camera** — webcam feed works for face detection, wall/floor detection, and surface color detection.

## What's blocked

The **Xtractor shooting minigame** requires Sony's **SmartAR** library (`libsmart.suprx`) to achieve 6DOF visual-inertial tracking (SLAM). This can't work in the emulator because:

- The **webcam and controller are separate physical devices** — camera motion and gyroscope motion don't correlate, so SmartAR can't compute the device's 3D pose.
- **No camera calibration data** (lens intrinsics) is provided by Vita3K.
- SmartAR detects 2D scene features fine (the color bar fills), but never achieves "tracking locked" — so the game refuses to spawn minigame objects.

This minigame is **required to progress the story**.

## The path forward: hybrid HLE override

Full HLE replacement of SmartAR doesn't work (it breaks the 2D feature detection that fills the color bar). Instead, the plan is a **hybrid approach**: keep SmartAR running as LLE (real binary) so 2D detection works, then override specific outputs to fake "tracking locked".

## Recon findings (Step 1 — COMPLETE)

A guest-ARM trampoline was built (`vita3k/modules/SceSmart/smartar_recon.cpp`) that wraps the LLE SmartAR functions: it calls the original LLE function (so detection still works), then traps to the host to log the function's outputs. Captures across the idle → color-bar-filling → sparks transition revealed:

| Function | What it does | Recon finding |
|----------|--------------|---------------|
| `sceSmartSceneMappingDispatchAndQuery` | per-frame SLAM query | Result struct = static header (size, double-buffered data ptrs, timestamp). Pose output buffer (p04) all zeros — never populated. Camera image buffer (p10) has the feed. |
| `sceSmartSceneMappingGetInitializationPointInfo` | init-point info | **retval = init-point count** (0→28→3→42→57). Buffer = float 2D pixel coords of features. Drives the color bar. |
| `sceSmartSceneMappingGetLandmarkInfo` | landmark info | **ALWAYS retval=0, empty 512-byte buffer.** SLAM never produces landmarks. ← the "not locked" gate |
| `sceSmartSceneMappingGetNumInitializationPoints` / `GetNumLandmarks` | count queries | **Never called by the game** (not imported). |
| `sceSmartSceneMappingPropagateResult` | apply result | retval=0 (success) mostly; `prop` buffer = static identity-ish pose (+0x08=1, +0x1C=1.0f, +0x20=0x80000000). |

The game polls `GetLandmarkInfo` every frame and always gets 0/empty → never sees "tracking locked" → sits in sparks.

## Step 2 — hybrid HLE override (IN PROGRESS)

**Experiment 1 (DONE — did not work):** Forced `prop+0x08 = 2` (hypothesis: `SceSmartSceneMappingState` NORMAL/tracking-locked) and `prop+0x20 = 0` (clear "invalid" flag) in the `PropagateResult` trampoline after LLE ran. Override applied on all 2,391 calls, but `GetLandmarkInfo` stayed 0 and nothing spawned. The `prop` buffer state enum is **not** the gate the game checks.

**Next (Experiment 2):** Fake `GetLandmarkInfo` — override retval to non-zero and fill the 512-byte buffer with a minimal fake landmark. Unknown: the landmark struct layout (SmartAR landmarks are 3D map points: position + descriptor + quality). Risk: wrong layout → crash or no effect.

**Decisive fallback (Experiment 4):** Disassemble the game binary around the `GetLandmarkInfo` / `DispatchAndQuery` call sites to find the exact field/branch the game checks for "tracking locked". Most work but removes all guesswork.

## Branch state

- **`hybrid-hle-override`** — recon trampoline + Step 2 Experiment 1 (in progress). 5 commits ahead of remote, ready to push.
- `master` — Vita3K baseline.
- `invizimals-ar-patches` — Atrac9 fix + motion system (the working patches).

See **PROJECT.md** for full technical details, build instructions, complete recon data, and the Step 2 experiment log.
