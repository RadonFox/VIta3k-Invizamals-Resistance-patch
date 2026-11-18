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

// SmartAR recon: capture what the LLE libsmart writes into the
// SceneMappingResult struct each frame, so we can identify the tracking-state
// field and its "locked" value. See PROJECT.md "Next Action" Step 1.

#include "smartar_recon.h"

#include <cpu/state.h>
#include <emuenv/state.h>
#include <kernel/state.h>
#include <mem/functions.h>
#include <mem/ptr.h>
#include <mem/util.h>
#include <util/arm.h>
#include <util/log.h>

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
// Returns true if the 12-byte stub at `stub` targets `addr`.
static bool stub_targets_address(const uint32_t *stub, Address addr) {
    return stub[0] == encode_arm_inst(INSTRUCTION_MOVW, static_cast<uint16_t>(addr), 12)
        && stub[1] == encode_arm_inst(INSTRUCTION_MOVT, static_cast<uint16_t>(addr >> 16), 12)
        && stub[2] == encode_arm_inst(INSTRUCTION_BRANCH, 0, 12);
}

// Write a 12-byte "MOVW/MOVT/BX r12 -> target" stub at `dest`.
static void write_branch_stub(uint32_t *dest, Address target) {
    dest[0] = encode_arm_inst(INSTRUCTION_MOVW, static_cast<uint16_t>(target), 12);
    dest[1] = encode_arm_inst(INSTRUCTION_MOVT, static_cast<uint16_t>(target >> 16), 12);
    dest[2] = encode_arm_inst(INSTRUCTION_BRANCH, 0, 12);
}

void install_smartar_recon_hook(EmuEnvState &emuenv) {
    auto &kernel = emuenv.kernel;
    auto &mem = emuenv.mem;

    Address lle_addr = 0;
    {
        const std::lock_guard<std::mutex> guard(kernel.export_nids_mutex);
        const auto it = kernel.export_nids.find(SMARTAR_DISPATCH_NID);
        if (it == kernel.export_nids.end() || it->second == 0) {
            LOG_INFO("SmartAR recon: sceSmartSceneMappingDispatchAndQuery export not found; "
                     "libsmart is HLE or not loaded yet. Recon hook not installed.");
            return;
        }
        lle_addr = it->second;
    }

    // If the export is already an SVC stub (HLE), there's nothing to wrap.
    {
        const uint32_t *stub = Ptr<uint32_t>(lle_addr).get(mem);
        if (stub && stub[0] == 0xEF000000u && stub[1] == 0xE1A0F00Eu) {
            LOG_INFO("SmartAR recon: export at {} is already an HLE SVC stub; nothing to wrap.",
                log_hex(lle_addr));
            return;
        }
    }

    // 1) Dump SVC stub: svc #0; mov pc, lr; SMARTAR_RECON_DUMP_NID
    //    The host call_import() special-cases this NID to dump the result struct.
    Address dump_stub_addr = alloc(mem, 12, "smartar_recon_dump_stub");
    if (!dump_stub_addr) {
        LOG_ERROR("SmartAR recon: failed to allocate dump SVC stub");
        return;
    }
    uint32_t *dump_stub = Ptr<uint32_t>(dump_stub_addr).get(mem);
    dump_stub[0] = 0xEF000000u; // svc #0
    dump_stub[1] = 0xE1A0F00Eu; // mov pc, lr
    dump_stub[2] = SMARTAR_RECON_DUMP_NID;

    // 2) Trampoline (ARM, 11 instructions = 44 bytes; allocate 64).
    //    Entry args: r0=ctx, r1=result, r2, r3. lr = caller (game) return.
    //    r4-r6 are callee-saved (AAPCS): the game expects them preserved, so
    //    we push/pop them. r12 is caller-saved scratch.
    //
    //      push {r4-r6, lr}     ; save callee-saved + game return addr
    //      mov  r4, r1          ; r4 = result ptr (preserved across LLE call)
    //      movw r12, lo(lle)    ;
    //      movt r12, hi(lle)    ;
    //      blx  r12             ; call original LLE; r0 = retval; r4 preserved
    //      mov  r6, r0          ; r6 = LLE retval (preserved across dump call)
    //      mov  r1, r4          ; r1 = result ptr (read by host dump handler)
    //      movw r12, lo(dump)   ;
    //      movt r12, hi(dump)   ;
    //      blx  r12             ; trap to host dump; r0 = 0; r4/r6 preserved
    //      mov  r0, r6          ; restore LLE retval
    //      pop  {r4-r6, pc}     ; restore r4-r6 and return to game
    Address trampoline_addr = alloc(mem, 64, "smartar_recon_trampoline");
    if (!trampoline_addr) {
        LOG_ERROR("SmartAR recon: failed to allocate trampoline");
        free(mem, dump_stub_addr);
        return;
    }
    uint32_t *tr = Ptr<uint32_t>(trampoline_addr).get(mem);
    const uint16_t lle_lo = static_cast<uint16_t>(lle_addr);
    const uint16_t lle_hi = static_cast<uint16_t>(lle_addr >> 16);
    const uint16_t dump_lo = static_cast<uint16_t>(dump_stub_addr);
    const uint16_t dump_hi = static_cast<uint16_t>(dump_stub_addr >> 16);
    int i = 0;
    tr[i++] = 0xE92D4070u; // push {r4-r6, lr}
    tr[i++] = arm_mov(4, 1); // mov r4, r1
    tr[i++] = encode_arm_inst(INSTRUCTION_MOVW, lle_lo, 12); // movw r12, lo(lle)
    tr[i++] = encode_arm_inst(INSTRUCTION_MOVT, lle_hi, 12); // movt r12, hi(lle)
    tr[i++] = arm_blx_reg(12); // blx r12 (call LLE)
    tr[i++] = arm_mov(6, 0); // mov r6, r0
    tr[i++] = arm_mov(1, 4); // mov r1, r4
    tr[i++] = encode_arm_inst(INSTRUCTION_MOVW, dump_lo, 12); // movw r12, lo(dump)
    tr[i++] = encode_arm_inst(INSTRUCTION_MOVT, dump_hi, 12); // movt r12, hi(dump)
    tr[i++] = arm_blx_reg(12); // blx r12 (dump SVC)
    tr[i++] = arm_mov(0, 6); // mov r0, r6
    tr[i++] = 0xE8BD8070u; // pop {r4-r6, pc}
    // NOP remaining bytes (instructions 12..15)
    for (; i < 16; ++i)
        tr[i] = 0xE1A00000u; // mov r0, r0 (NOP)

    kernel.invalidate_jit_cache(trampoline_addr, 64);
    kernel.invalidate_jit_cache(dump_stub_addr, 12);

    // 3) Re-patch every game import stub that currently targets the LLE address
    //    so it jumps to our trampoline instead. After libsmart loaded,
    //    load_func_exports() already rewrote matching stubs to -> lle_addr, so
    //    we identify them by that target. Stubs still in SVC (HLE fallback) form
    //    are left untouched.
    int patched = 0;
    {
        const std::lock_guard<std::mutex> guard(kernel.export_nids_mutex);
        auto range = kernel.func_binding_infos.equal_range(SMARTAR_DISPATCH_NID);
        for (auto it = range.first; it != range.second; ++it) {
            const Address stub_addr = it->second.entry_address;
            uint32_t *stub = Ptr<uint32_t>(stub_addr).get(mem);
            if (!stub)
                continue;
            if (!stub_targets_address(stub, lle_addr))
                continue;
            write_branch_stub(stub, trampoline_addr);
            kernel.invalidate_jit_cache(stub_addr, 12);
            ++patched;
        }
        // Point future resolutions to the trampoline too (modules loaded later
        // that import this NID should also be wrapped).
        kernel.export_nids[SMARTAR_DISPATCH_NID] = trampoline_addr;
    }

    LOG_INFO("SmartAR recon: installed trampoline at {} wrapping LLE DispatchAndQuery "
             "at {} (dump SVC at {}); patched {} import stub(s)",
        log_hex(trampoline_addr), log_hex(lle_addr), log_hex(dump_stub_addr), patched);
}

void smartar_recon_dump_result(EmuEnvState &emuenv, CPUState &cpu) {
    // Throttle: log the first 5 frames, then every 60th, so we capture the
    // "color bar filling -> tracking attempting -> locked" progression without
    // flooding the log.
    static uint32_t frame = 0;
    static uint32_t logged = 0;
    ++frame;
    const bool do_log = (logged < 5) || (frame % 60 == 0);
    if (!do_log)
        return;

    const Address result_ptr = cpu.get_reg(1);
    if (result_ptr == 0) {
        LOG_INFO("SmartAR recon [frame {}]: result ptr r1 = 0 (null), skipping dump", frame);
        return;
    }

    constexpr size_t DUMP_BYTES = 96; // 64-byte struct + 32 extra to find trailing fields
    const uint8_t *data = Ptr<uint8_t>(result_ptr).get(emuenv.mem);
    if (!data) {
        LOG_INFO("SmartAR recon [frame {}]: result ptr {} maps to no host memory",
            frame, log_hex(result_ptr));
        return;
    }

    LOG_INFO("SmartAR recon [frame {}] SceneMappingResult @ {} (r0={})",
        frame, log_hex(result_ptr), log_hex(cpu.get_reg(0)));
    for (size_t off = 0; off < DUMP_BYTES; off += 16) {
        // 16 bytes per line as 4 little-endian uint32 words with struct offset.
        // memcpy keeps this well-defined regardless of host pointer alignment.
        uint32_t words[4] = {};
        std::memcpy(words, data + off, sizeof(words));
        LOG_INFO("  +0x{:02x}: {:08X} {:08X} {:08X} {:08X}",
            off, words[0], words[1], words[2], words[3]);
    }
    ++logged;
}
