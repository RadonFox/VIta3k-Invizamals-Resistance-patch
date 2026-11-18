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

// SmartAR recon: capture what LLE libsmart writes / returns across the
// idle -> color-bar-filling -> locked transition, so we can identify where
// the game reads the tracking state from. See PROJECT.md "Next Action".

#include "smartar_recon.h"

#include <cpu/state.h>
#include <emuenv/state.h>
#include <kernel/state.h>
#include <mem/functions.h>
#include <mem/ptr.h>
#include <mem/util.h>
#include <util/arm.h>
#include <util/log.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>

// ---- ARM A32 instruction encoders (host side, for emitting guest code) ----

// MOV Rd, Rm  (0xE1A00000 | Rm | (Rd << 12))
static constexpr uint32_t arm_mov(uint8_t rd, uint8_t rm) {
    return 0xE1A00000u | (static_cast<uint32_t>(rm) << 0) | (static_cast<uint32_t>(rd) << 12);
}

// BLX Rm  (0xE12FFF30 | Rm) — branch with link (sets lr = return addr)
static constexpr uint32_t arm_blx_reg(uint8_t rm) {
    return 0xE12FFF30u | static_cast<uint32_t>(rm);
}

// A resolved import stub is: MOVW r12, lo(addr); MOVT r12, hi(addr); BX r12.
static bool stub_targets_address(const uint32_t *stub, Address addr) {
    return stub[0] == encode_arm_inst(INSTRUCTION_MOVW, static_cast<uint16_t>(addr), 12)
        && stub[1] == encode_arm_inst(INSTRUCTION_MOVT, static_cast<uint16_t>(addr >> 16), 12)
        && stub[2] == encode_arm_inst(INSTRUCTION_BRANCH, 0, 12);
}

static void write_branch_stub(uint32_t *dest, Address target) {
    dest[0] = encode_arm_inst(INSTRUCTION_MOVW, static_cast<uint16_t>(target), 12);
    dest[1] = encode_arm_inst(INSTRUCTION_MOVT, static_cast<uint16_t>(target >> 16), 12);
    dest[2] = encode_arm_inst(INSTRUCTION_BRANCH, 0, 12);
}

// Emit a generic trampoline that:
//   1. saves entry r0 -> r4, r1 -> r5 (callee-saved across the LLE call)
//   2. calls the original LLE function at lle_addr
//   3. saves retval (r0) -> r6
//   4. sets r0 = entry r0, r1 = entry r1, r2 = retval, then calls log_stub
//      (the host dump handler reads r0/r1/r2 to log whatever it needs)
//   5. restores r0 = retval and returns to the game
// r4-r6 + lr are saved/restored (AAPCS-correct).
//
//       push {r4-r6, lr}
//       mov  r4, r0
//       mov  r5, r1
//       movw/movt r12, lle ; blx r12
//       mov  r6, r0
//       mov  r0, r4
//       mov  r1, r5
//       mov  r2, r6
//       movw/movt r12, log ; blx r12
//       mov  r0, r6
//       pop  {r4-r6, pc}
static Address emit_generic_trampoline(MemState &mem, Address lle_addr, Address log_stub_addr) {
    Address tramp = alloc(mem, 64, "smartar_recon_trampoline");
    if (!tramp)
        return 0;
    uint32_t *tr = Ptr<uint32_t>(tramp).get(mem);
    const uint16_t lle_lo = static_cast<uint16_t>(lle_addr);
    const uint16_t lle_hi = static_cast<uint16_t>(lle_addr >> 16);
    const uint16_t log_lo = static_cast<uint16_t>(log_stub_addr);
    const uint16_t log_hi = static_cast<uint16_t>(log_stub_addr >> 16);
    int i = 0;
    tr[i++] = 0xE92D4070u; // push {r4-r6, lr}
    tr[i++] = arm_mov(4, 0); // mov r4, r0
    tr[i++] = arm_mov(5, 1); // mov r5, r1
    tr[i++] = encode_arm_inst(INSTRUCTION_MOVW, lle_lo, 12);
    tr[i++] = encode_arm_inst(INSTRUCTION_MOVT, lle_hi, 12);
    tr[i++] = arm_blx_reg(12); // blx r12 (call LLE)
    tr[i++] = arm_mov(6, 0); // mov r6, r0 (save retval)
    tr[i++] = arm_mov(0, 4); // mov r0, r4 (entry r0)
    tr[i++] = arm_mov(1, 5); // mov r1, r5 (entry r1)
    tr[i++] = arm_mov(2, 6); // mov r2, r6 (retval)
    tr[i++] = encode_arm_inst(INSTRUCTION_MOVW, log_lo, 12);
    tr[i++] = encode_arm_inst(INSTRUCTION_MOVT, log_hi, 12);
    tr[i++] = arm_blx_reg(12); // blx r12 (log SVC)
    tr[i++] = arm_mov(0, 6); // mov r0, r6 (restore retval)
    tr[i++] = 0xE8BD8070u; // pop {r4-r6, pc}
    for (; i < 16; ++i)
        tr[i] = 0xE1A00000u; // NOP
    return tramp;
}

// Allocate a 12-byte SVC stub: svc #0; mov pc, lr; log_nid
static Address alloc_log_svc_stub(MemState &mem, uint32_t log_nid) {
    Address stub_addr = alloc(mem, 12, "smartar_recon_log_svc");
    if (!stub_addr)
        return 0;
    uint32_t *stub = Ptr<uint32_t>(stub_addr).get(mem);
    stub[0] = 0xEF000000u;
    stub[1] = 0xE1A0F00Eu;
    stub[2] = log_nid;
    return stub_addr;
}

// Install a generic trampoline wrapping the LLE export for `func_nid`, routing
// the post-call log trap to `log_nid`. Idempotent per func_nid.
static void install_wrapper(EmuEnvState &emuenv, uint32_t func_nid, uint32_t log_nid, const char *name) {
    auto &kernel = emuenv.kernel;
    auto &mem = emuenv.mem;

    Address lle_addr = 0;
    {
        const std::lock_guard<std::mutex> guard(kernel.export_nids_mutex);
        const auto it = kernel.export_nids.find(func_nid);
        if (it == kernel.export_nids.end() || it->second == 0) {
            LOG_INFO("SmartAR recon: {} export not found (HLE or not loaded); skipping", name);
            return;
        }
        lle_addr = it->second;
        // If already wrapped (lle_addr is one of our trampolines), skip.
        const uint32_t *p = Ptr<uint32_t>(lle_addr).get(mem);
        if (p && p[0] == 0xE92D4070u) {
            LOG_DEBUG("SmartAR recon: {} already wrapped", name);
            return;
        }
    }

    // Skip if export is an HLE SVC stub (no LLE to wrap).
    {
        const uint32_t *p = Ptr<uint32_t>(lle_addr).get(mem);
        if (p && p[0] == 0xEF000000u && p[1] == 0xE1A0F00Eu) {
            LOG_INFO("SmartAR recon: {} export is HLE SVC stub; nothing to wrap", name);
            return;
        }
    }

    Address log_stub = alloc_log_svc_stub(mem, log_nid);
    if (!log_stub) {
        LOG_ERROR("SmartAR recon: failed to alloc log SVC for {}", name);
        return;
    }
    Address tramp = emit_generic_trampoline(mem, lle_addr, log_stub);
    if (!tramp) {
        LOG_ERROR("SmartAR recon: failed to alloc trampoline for {}", name);
        free(mem, log_stub);
        return;
    }

    kernel.invalidate_jit_cache(tramp, 64);
    kernel.invalidate_jit_cache(log_stub, 12);

    int patched = 0;
    {
        const std::lock_guard<std::mutex> guard(kernel.export_nids_mutex);
        auto range = kernel.func_binding_infos.equal_range(func_nid);
        for (auto it = range.first; it != range.second; ++it) {
            const Address stub_addr = it->second.entry_address;
            uint32_t *stub = Ptr<uint32_t>(stub_addr).get(mem);
            if (!stub || !stub_targets_address(stub, lle_addr))
                continue;
            write_branch_stub(stub, tramp);
            kernel.invalidate_jit_cache(stub_addr, 12);
            ++patched;
        }
        kernel.export_nids[func_nid] = tramp;
    }

    LOG_INFO("SmartAR recon: wrapped {} (NID {}) trampoline={} lle={} log_svc={} patched={}",
        name, log_hex(func_nid), log_hex(tramp), log_hex(lle_addr), log_hex(log_stub), patched);
}

void install_smartar_recon_hook(EmuEnvState &emuenv) {
    static bool installed = false;
    if (installed) {
        LOG_DEBUG("SmartAR recon: hooks already installed, skipping");
        return;
    }

    // DispatchAndQuery (per-frame query; result struct at entry r1)
    install_wrapper(emuenv, 0xF10FDDD1u, SMARTAR_RECON_DUMP_NID, "sceSmartSceneMappingDispatchAndQuery");
    // Count-returning queries — these are likely where the game reads
    // "color bar full" (init points) and "tracking locked" (landmarks).
    install_wrapper(emuenv, 0xDF900B29u, SMARTAR_RECON_NUM_INIT_POINTS_NID, "sceSmartSceneMappingGetNumInitializationPoints");
    install_wrapper(emuenv, 0x2260E8BBu, SMARTAR_RECON_NUM_LANDMARKS_NID, "sceSmartSceneMappingGetNumLandmarks");
    // Per-point info (fills a buffer at entry r0).
    install_wrapper(emuenv, 0x9A578A29u, SMARTAR_RECON_INIT_POINT_INFO_NID, "sceSmartSceneMappingGetInitializationPointInfo");
    // Landmark info (fills a buffer at entry r0, size at entry r1=512).
    install_wrapper(emuenv, 0xB17057EAu, SMARTAR_RECON_LANDMARK_INFO_NID, "sceSmartSceneMappingGetLandmarkInfo");
    // PropagateResult (entry r0 = result/state ptr; we dump 64 bytes at it).
    install_wrapper(emuenv, 0x5A9EC64Du, SMARTAR_RECON_PROPAGATE_RESULT_NID, "sceSmartSceneMappingPropagateResult");

    installed = true;
    LOG_INFO("SmartAR recon: all wrappers installed");
}

// ---- host-side dump handlers ----

static void dump_hex(EmuEnvState &emuenv, Address addr, size_t bytes, const char *label) {
    if (addr == 0) {
        LOG_INFO("    {}: <null>", label);
        return;
    }
    const uint8_t *data = Ptr<uint8_t>(addr).get(emuenv.mem);
    if (!data) {
        LOG_INFO("    {}: ptr {} maps to no host memory", label, log_hex(addr));
        return;
    }
    for (size_t off = 0; off < bytes; off += 16) {
        uint32_t words[4] = {};
        std::memcpy(words, data + off, sizeof(words));
        LOG_INFO("    {} +0x{:02x}: {:08X} {:08X} {:08X} {:08X}",
            label, off, words[0], words[1], words[2], words[3]);
    }
}

static uint32_t frame = 0;
static uint32_t logged = 0;

// DispatchAndQuery: entry r0=ctx, r1=result struct ptr; retval in r2.
static void dump_dispatch_and_query(EmuEnvState &emuenv, CPUState &cpu) {
    const Address result_ptr = cpu.get_reg(1); // entry r1
    const uint32_t retval = cpu.get_reg(2); // LLE retval

    LOG_INFO("SmartAR recon [frame {}] DispatchAndQuery retval={} result@={}",
        frame, log_hex(retval), log_hex(result_ptr));
    if (result_ptr == 0)
        return;

    dump_hex(emuenv, result_ptr, 128, "main");

    // Follow the double-buffered data pointers (+0x04, +0x10) and config (+0x18).
    const uint8_t *base = Ptr<uint8_t>(result_ptr).get(emuenv.mem);
    if (base) {
        uint32_t p04 = 0, p10 = 0, p18 = 0;
        std::memcpy(&p04, base + 0x04, 4);
        std::memcpy(&p10, base + 0x10, 4);
        std::memcpy(&p18, base + 0x18, 4);
        dump_hex(emuenv, p04, 128, "p04");
        dump_hex(emuenv, p10, 128, "p10");
        if (logged < 5 || frame % 300 == 0)
            dump_hex(emuenv, p18, 128, "p18");
    }
}

// GetNumInitializationPoints / GetNumLandmarks: count in retval (r2).
static void dump_count(EmuEnvState &emuenv, CPUState &cpu, const char *name) {
    const uint32_t count = cpu.get_reg(2);
    LOG_INFO("SmartAR recon [frame {}] {} -> count={}",
        frame, name, count);
}

// GetInitializationPointInfo: entry r0=out buf, r1=size, r2=index? (r2 is retval here)
// Note: our generic trampoline overwrites entry r2 with retval, so index isn't
// available; r0=buf, r1=size, r2=retval. retval is the init-point count.
static uint32_t ipi_logged = 0;
static void dump_init_point_info(EmuEnvState &emuenv, CPUState &cpu) {
    const Address buf = cpu.get_reg(0); // entry r0
    const uint32_t size = cpu.get_reg(1); // entry r1
    const uint32_t retval = cpu.get_reg(2); // LLE retval = init point count
    // Always log the retval (count) — it's the state signal. Throttle the
    // buffer dump (it's verbose and mostly coordinate floats).
    const bool dump_buf = (ipi_logged < 10) || (frame % 500 == 0);
    if (dump_buf) {
        LOG_INFO("SmartAR recon [frame {}] GetInitializationPointInfo count={} buf={} size={}",
            frame, retval, log_hex(buf), size);
        if (buf && size > 0 && size <= 256)
            dump_hex(emuenv, buf, size, "ipi");
        ++ipi_logged;
    } else {
        LOG_INFO("SmartAR recon [frame {}] GetInitializationPointInfo count={}", frame, retval);
    }
}

// GetLandmarkInfo: entry r0=out buf, r1=size(=512). retval in r2.
// Landmarks appear only when SLAM tracking locks — so any non-zero retval or
// non-zero buffer content here is the "tracking locked" signal we're hunting.
static void dump_landmark_info(EmuEnvState &emuenv, CPUState &cpu) {
    const Address buf = cpu.get_reg(0); // entry r0
    const uint32_t size = cpu.get_reg(1); // entry r1
    const uint32_t retval = cpu.get_reg(2); // LLE retval
    LOG_INFO("SmartAR recon [frame {}] GetLandmarkInfo retval={} buf={} size={}",
        frame, log_hex(retval), log_hex(buf), size);
    if (buf && size > 0 && size <= 1024)
        dump_hex(emuenv, buf, std::min<uint32_t>(size, 128), "lmi");
}

// PropagateResult: entry r0 = result/state ptr (we dump 64 bytes at it).
static void dump_propagate_result(EmuEnvState &emuenv, CPUState &cpu) {
    const Address state_ptr = cpu.get_reg(0); // entry r0
    const uint32_t arg1 = cpu.get_reg(1); // entry r1
    const uint32_t retval = cpu.get_reg(2); // LLE retval
    LOG_INFO("SmartAR recon [frame {}] PropagateResult retval={} state@={} arg1={}",
        frame, log_hex(retval), log_hex(state_ptr), log_hex(arg1));
    if (state_ptr)
        dump_hex(emuenv, state_ptr, 64, "prop");
}

void smartar_recon_dump_result(EmuEnvState &emuenv, CPUState &cpu, uint32_t log_nid) {
    ++frame;

    // Throttle: DispatchAndQuery is called every frame and is expensive to dump.
    // Count functions are cheap and more informative for state transitions —
    // log them every time they're called (they're called less often than
    // DispatchAndQuery, typically a few times per frame).
    switch (log_nid) {
    case SMARTAR_RECON_DUMP_NID: {
        const bool do_log = (logged < 20) || (frame % 30 == 0);
        if (!do_log)
            return;
        dump_dispatch_and_query(emuenv, cpu);
        ++logged;
        break;
    }
    case SMARTAR_RECON_NUM_INIT_POINTS_NID:
        dump_count(emuenv, cpu, "GetNumInitializationPoints");
        break;
    case SMARTAR_RECON_NUM_LANDMARKS_NID:
        dump_count(emuenv, cpu, "GetNumLandmarks");
        break;
    case SMARTAR_RECON_INIT_POINT_INFO_NID:
        dump_init_point_info(emuenv, cpu);
        break;
    case SMARTAR_RECON_LANDMARK_INFO_NID:
        dump_landmark_info(emuenv, cpu);
        break;
    case SMARTAR_RECON_PROPAGATE_RESULT_NID:
        dump_propagate_result(emuenv, cpu);
        break;
    default:
        // Unknown recon NID — ignore.
        break;
    }
}
