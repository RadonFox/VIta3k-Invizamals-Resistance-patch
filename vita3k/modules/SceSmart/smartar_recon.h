// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#pragma once

#include <util/types.h>

struct EmuEnvState;
struct CPUState;

// NIDs of the SmartAR SceneMapping functions the game calls (from nids.inc).
constexpr uint32_t SMARTAR_NID_DISPATCH_AND_QUERY = 0xF10FDDD1u; // sceSmartSceneMappingDispatchAndQuery
constexpr uint32_t SMARTAR_NID_GET_NUM_INIT_POINTS = 0xDF900B29u; // sceSmartSceneMappingGetNumInitializationPoints
constexpr uint32_t SMARTAR_NID_GET_NUM_LANDMARKS = 0x2260E8BBu; // sceSmartSceneMappingGetNumLandmarks
constexpr uint32_t SMARTAR_NID_GET_INIT_POINT_INFO = 0x9A578A29u; // sceSmartSceneMappingGetInitializationPointInfo

// Synthetic log NIDs used by the recon trampolines' SVC stubs. call_import()
// special-cases these to dump the wrapped function's inputs/outputs.
// Chosen to never collide with real NIDs.
constexpr uint32_t SMARTAR_RECON_DUMP_NID = 0xDEAD0001u; // DispatchAndQuery (result struct)
constexpr uint32_t SMARTAR_RECON_NUM_INIT_POINTS_NID = 0xDEAD0010u; // GetNumInitializationPoints (retval=count)
constexpr uint32_t SMARTAR_RECON_NUM_LANDMARKS_NID = 0xDEAD0011u; // GetNumLandmarks (retval=count)
constexpr uint32_t SMARTAR_RECON_INIT_POINT_INFO_NID = 0xDEAD0012u; // GetInitializationPointInfo (buf fill)

// Install guest-ARM trampolines wrapping the LLE SmartAR functions listed above.
// Each trampoline calls the original LLE function (so 2D detection / color bar
// still work), then traps to the host to log the function's outputs. Must be
// called once after libsmart.suprx has loaded and registered its exports.
// Safe to call when libsmart is HLE (per-function no-op).
void install_smartar_recon_hook(EmuEnvState &emuenv);

// Host-side dump dispatcher, invoked via the log SVC stubs. Called from
// call_import() when nid is one of the SMARTAR_RECON_*_NID values.
void smartar_recon_dump_result(EmuEnvState &emuenv, CPUState &cpu, uint32_t log_nid);
