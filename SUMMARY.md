# Executive Summary

## Project: Invizimals: The Resistance on Vita3K

**Status:** Partially working — past face-confirm, wall detection, and camera color-lock. Blocked at the Xtractor shooting minigame (objects won't spawn).

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

Full HLE replacement of SmartAR doesn't work (it breaks the 2D feature detection that fills the color bar). Instead, the plan is a **hybrid approach**:

1. Keep SmartAR running as LLE (real binary) so 2D feature detection works
2. After `libsmart.suprx` loads, use Vita3K's `register_hle_override()` (in `taiHEN.cpp`) to redirect specific SmartAR export functions to HLE stubs
3. Override `sceSmartSceneMappingDispatchAndQuery` to inject "tracking locked" into the result struct
4. The challenge: we need to know which field in the 64-byte result struct is the tracking state, and what value means "locked"

## What we know from recon

The game calls 7 SmartAR functions (all SceneMapping/SLAM, no TargetTracking):

| Function | Signature | Purpose |
|----------|-----------|---------|
| `sceSmartInit` | `(Ptr config, int mode)` | Init library |
| `sceSmartSceneMappingStart` | `(int flags=5, int enable=1)` | Start SLAM |
| `sceSmartSceneMappingDispatchAndQuery` | `(Ptr ctx, Ptr result, int, int)` | **Per-frame query — result struct at r1 (64+ bytes) holds tracking state** |
| `sceSmartSceneMappingGetInitializationPointInfo` | `(Ptr out, int size=64, int index)` | Init point info (color bar) |
| `sceSmartSceneMappingGetLandmarkInfo` | `(Ptr out, int size=512, Ptr ctx, int index)` | Landmark info |
| `sceSmartSceneMappingPropagateResult` | `(Ptr, int, int, int)` | Propagate results |
| `sceSmartSceneMappingReset` | `(int, int, int, int)` | Reset tracking |

The result struct passed to `DispatchAndQuery` is pre-filled with `0xDEADBEEF` sentinels. With LLE, SmartAR fills in real data. We need to determine the tracking state field and its "locked" value.

## Next step

**Branch: `hybrid-hle-override`**

Implement the hybrid approach:
1. Find a way to call the original LLE `sceSmartSceneMappingDispatchAndQuery`, then modify its output to set tracking state = "locked"
2. If that's not possible, study the result struct by comparing LLE output (when color bar fills) vs the 0xDEADBEEF pre-fill to identify the tracking state field
3. Use `register_hle_override()` to redirect the function after `libsmart.suprx` loads

See **PROJECT.md** for full technical details, build instructions, and complete recon data.
