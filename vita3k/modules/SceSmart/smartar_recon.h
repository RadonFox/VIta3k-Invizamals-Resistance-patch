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

// NID 0xF10FDDD1 = sceSmartSceneMappingDispatchAndQuery (the per-frame SLAM
// query whose result struct holds the tracking state + pose we want to inspect).
constexpr uint32_t SMARTAR_DISPATCH_NID = 0xF10FDDD1u;

// Synthetic NID used by the recon trampoline's dump SVC stub. call_import()
// special-cases this NID to dump the SceneMappingResult struct to the log.
// Chosen to never collide with a real NID.
constexpr uint32_t SMARTAR_RECON_DUMP_NID = 0xDEAD0001u;

// Install a guest-ARM trampoline that wraps the LLE
// sceSmartSceneMappingDispatchAndQuery export: it calls the original LLE
// function (so 2D feature detection / color bar still work), then traps to
// the host via a dump SVC stub so we can log the contents of the result
// struct at r1. Must be called once after libsmart.suprx has loaded and
// registered its exports. Safe to call when libsmart is HLE (no-op).
void install_smartar_recon_hook(EmuEnvState &emuenv);

// Dump the SceneMappingResult struct (host side, invoked via the dump SVC
// stub). Reads r1 for the result pointer. Defined in smartar_recon.cpp and
// called from call_import() when nid == SMARTAR_RECON_DUMP_NID.
void smartar_recon_dump_result(EmuEnvState &emuenv, CPUState &cpu);
