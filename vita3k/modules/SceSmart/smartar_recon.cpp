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

#include <cpu/disasm/functions.h>
#include <cpu/disasm/state.h>
#include <cpu/state.h>
#include <emuenv/state.h>
#include <kernel/state.h>
#include <mem/functions.h>
#include <mem/ptr.h>
#include <mem/util.h>
#include <nids/functions.h>
#include <util/arm.h>
#include <util/log.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

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
//   1. saves entry r0 -> r4, r1 -> r5, r2 -> r7, r3 -> r8 (callee-saved across
//      the LLE call, since LLE may clobber r0-r3)
//   2. calls the original LLE function at lle_addr
//   3. saves retval (r0) -> r6
//   4. sets r0 = entry r0, r1 = entry r1, r2 = retval, r3 = entry r3, then
//      calls log_stub (the host dump handler reads r0/r1/r2/r3)
//   5. restores r0 = r6 and returns to the game
// r4-r8 + lr are saved/restored (AAPCS-correct). r6 is the retval holder — a
// host handler can change the game's return value by calling set_reg(6, ...),
// since the trampoline's final `mov r0, r6` runs after the SVC returns (and
// write_call_result only overwrites r0, not r6).
//
//       push {r4-r8, lr}
//       mov  r4, r0
//       mov  r5, r1
//       mov  r7, r2
//       mov  r8, r3
//       movw/movt r12, lle ; blx r12
//       mov  r6, r0
//       mov  r0, r4
//       mov  r1, r5
//       mov  r2, r6
//       mov  r3, r8
//       movw/movt r12, log ; blx r12
//       mov  r0, r6
//       pop  {r4-r8, pc}
static Address emit_generic_trampoline(MemState &mem, Address lle_addr, Address log_stub_addr) {
    Address tramp = alloc(mem, 80, "smartar_recon_trampoline");
    if (!tramp)
        return 0;
    uint32_t *tr = Ptr<uint32_t>(tramp).get(mem);
    const uint16_t lle_lo = static_cast<uint16_t>(lle_addr);
    const uint16_t lle_hi = static_cast<uint16_t>(lle_addr >> 16);
    const uint16_t log_lo = static_cast<uint16_t>(log_stub_addr);
    const uint16_t log_hi = static_cast<uint16_t>(log_stub_addr >> 16);
    int i = 0;
    tr[i++] = 0xE92D41F0u; // push {r4-r8, lr}
    tr[i++] = arm_mov(4, 0); // mov r4, r0
    tr[i++] = arm_mov(5, 1); // mov r5, r1
    tr[i++] = arm_mov(7, 2); // mov r7, r2
    tr[i++] = arm_mov(8, 3); // mov r8, r3
    tr[i++] = encode_arm_inst(INSTRUCTION_MOVW, lle_lo, 12);
    tr[i++] = encode_arm_inst(INSTRUCTION_MOVT, lle_hi, 12);
    tr[i++] = arm_blx_reg(12); // blx r12 (call LLE)
    tr[i++] = arm_mov(6, 0); // mov r6, r0 (save retval)
    tr[i++] = arm_mov(0, 4); // mov r0, r4 (entry r0)
    tr[i++] = arm_mov(1, 5); // mov r1, r5 (entry r1)
    tr[i++] = arm_mov(2, 6); // mov r2, r6 (retval)
    tr[i++] = arm_mov(3, 8); // mov r3, r8 (entry r3 = index, etc.)
    tr[i++] = encode_arm_inst(INSTRUCTION_MOVW, log_lo, 12);
    tr[i++] = encode_arm_inst(INSTRUCTION_MOVT, log_hi, 12);
    tr[i++] = arm_blx_reg(12); // blx r12 (log SVC)
    tr[i++] = arm_mov(0, 6); // mov r0, r6 (restore retval; r6 may have been changed by host)
    tr[i++] = 0xE8BD81F0u; // pop {r4-r8, pc}
    for (; i < 20; ++i)
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
        if (p && p[0] == 0xE92D41F0u) {
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

    kernel.invalidate_jit_cache(tramp, 80);
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

// ---- Experiment 4: disassemble game code around SmartAR call sites ----
// Scans the game's main code segment for BL/BLX instructions targeting the
// SmartAR import stubs, then disassembles the entire AR function with
// annotated BL/BLX targets so we can see exactly what the game does with the
// return values — i.e. which field/branch constitutes the "tracking locked"
// check.

// Disassemble and log a region of code, annotating BL/BLX targets that match
// known import stubs with their function names.
static void log_annotated_region(MemState &mem, DisasmState &disasm,
    Address start, Address end, bool thumb,
    const std::map<Address, std::string> &stub_names) {
    Address pc = start;
    if (thumb)
        pc &= ~1u;
    else
        pc &= ~3u;
    int insn_count = 0;
    while (pc + 2 <= end && insn_count < 800) {
        const uint8_t *code = Ptr<uint8_t>(pc).get(mem);
        if (!code)
            break;
        uint16_t insn_size = 0;
        std::string text = disassemble(disasm, code, end - pc, pc, thumb, &insn_size);
        if (insn_size == 0) {
            LOG_INFO("SmartAR disasm:    {} <undecoded>", log_hex(pc));
            pc += thumb ? 2 : 4;
            continue;
        }
        // Annotate BL/BLX with function name if target is a known import stub.
        std::string annotation;
        if ((text.rfind("bl ", 0) == 0 || text.rfind("blx ", 0) == 0) && text.find("0x") != std::string::npos) {
            size_t hx = text.find("0x");
            Address target = static_cast<Address>(std::strtoul(text.c_str() + hx, nullptr, 16));
            auto it = stub_names.find(target);
            if (it != stub_names.end())
                annotation = "  ; " + it->second;
        }
        LOG_INFO("SmartAR disasm:    {} {}{}", log_hex(pc), text, annotation);
        pc += insn_size;
        ++insn_count;
    }
}

static void disassemble_smartar_callsites(EmuEnvState &emuenv) {
    auto &kernel = emuenv.kernel;
    auto &mem = emuenv.mem;

    // 1. Build a COMPLETE reverse map: import stub entry_address → function
    //    name, for ALL imported functions (not just SmartAR). This lets us
    //    annotate every BL/BLX in the disassembly.
    std::map<Address, std::string> stub_names;
    {
        const std::lock_guard<std::mutex> guard(kernel.export_nids_mutex);
        for (const auto &[nid, info] : kernel.func_binding_infos) {
            const char *name = import_name(nid);
            if (name && *name)
                stub_names[info.entry_address] = name;
        }
    }
    if (stub_names.empty()) {
        LOG_INFO("SmartAR disasm: no import stubs found; skipping");
        return;
    }

    // Log the SmartAR stubs specifically.
    struct NidName { uint32_t nid; const char *name; };
    const NidName smartar_nids[] = {
        {0x58979C84u, "sceSmartInit"},
        {0xA3A82B99u, "SceneMappingStart"},
        {0xF10FDDD1u, "DispatchAndQuery"},
        {0x9A578A29u, "GetInitializationPointInfo"},
        {0xB17057EAu, "GetLandmarkInfo"},
        {0x5A9EC64Du, "PropagateResult"},
        {0xC1AD43BCu, "SceneMappingReset"},
    };
    LOG_INFO("SmartAR disasm: {} total import stub(s) found", stub_names.size());
    {
        const std::lock_guard<std::mutex> guard(kernel.export_nids_mutex);
        for (const auto &nn : smartar_nids) {
            auto range = kernel.func_binding_infos.equal_range(nn.nid);
            for (auto it = range.first; it != range.second; ++it)
                LOG_INFO("  SmartAR stub {} = {} (NID {})", log_hex(it->second.entry_address), nn.name, log_hex(nn.nid));
        }
    }

    // 2. Find the game's main code module (largest non-library code segment).
    Address code_addr = 0;
    SceSize code_size = 0;
    std::string game_module_name;
    {
        const std::lock_guard<std::mutex> guard(kernel.mutex);
        for (const auto &[id, module] : kernel.loaded_modules) {
            const std::string name(module->info.module_name);
            if (name.rfind("lib", 0) == 0)
                continue;
            const auto &seg = module->info.segments[0];
            if (seg.memsz > code_size) {
                code_addr = seg.vaddr.address();
                code_size = seg.memsz;
                game_module_name = name;
            }
        }
    }
    if (code_addr == 0 || code_size == 0) {
        LOG_INFO("SmartAR disasm: no game code segment found; skipping");
        return;
    }
    LOG_INFO("SmartAR disasm: scanning game module '{}' code {} size {}",
        game_module_name, log_hex(code_addr), log_hex(code_size));

    // 3. Scan using capstone for reliable instruction-boundary disassembly.
    //    Find all SmartAR call sites and record their addresses.
    DisasmState disasm;
    if (!init(disasm)) {
        LOG_ERROR("SmartAR disasm: failed to init capstone");
        return;
    }

    // Collect addresses of SmartAR import stubs for quick lookup.
    std::set<Address> smartar_stub_addrs;
    {
        const std::lock_guard<std::mutex> guard(kernel.export_nids_mutex);
        for (const auto &nn : smartar_nids) {
            auto range = kernel.func_binding_infos.equal_range(nn.nid);
            for (auto it = range.first; it != range.second; ++it)
                smartar_stub_addrs.insert(it->second.entry_address);
        }
    }

    for (bool thumb : {true, false}) {
        std::vector<std::pair<Address, std::string>> call_sites;
        Address pc = code_addr;
        const Address end = code_addr + code_size;
        while (pc + 2 <= end) {
            const uint8_t *code = Ptr<uint8_t>(pc).get(mem);
            if (!code)
                break;
            uint16_t insn_size = 0;
            std::string text = disassemble(disasm, code, end - pc, pc, thumb, &insn_size);
            if (insn_size == 0) {
                pc += thumb ? 2 : 4;
                continue;
            }
            if ((text.rfind("bl ", 0) == 0 || text.rfind("blx ", 0) == 0) && text.find("0x") != std::string::npos) {
                size_t hx = text.find("0x");
                Address target = static_cast<Address>(std::strtoul(text.c_str() + hx, nullptr, 16));
                if (smartar_stub_addrs.count(target)) {
                    auto it = stub_names.find(target);
                    call_sites.push_back({pc, it != stub_names.end() ? it->second : "unknown"});
                }
            }
            pc += insn_size;
        }
        if (!call_sites.empty()) {
            LOG_INFO("SmartAR disasm: found {} SmartAR call site(s) in {} mode", call_sites.size(), thumb ? "Thumb" : "ARM");
            for (const auto &[addr, name] : call_sites)
                LOG_INFO("  call site {} -> {}", log_hex(addr), name);

            // Dump the entire region covering all call sites + surrounding
            // functions. The PropagateResult call (0x814FEB12) is in a SEPARATE
            // function ending at 0x814FEB64, before the main AR update function
            // (0x814FEE48). Use a wide range to capture both.
            const Address region_start = call_sites.front().first - 0x300;
            const Address region_end = call_sites.back().first + 0x300;
            LOG_INFO("SmartAR disasm: === full AR function disassembly {} to {} ===", log_hex(region_start), log_hex(region_end));
            log_annotated_region(mem, disasm, region_start, region_end, thumb, stub_names);
            LOG_INFO("SmartAR disasm: === end AR function disassembly ===");
            break;
        }
        LOG_INFO("SmartAR disasm: no SmartAR call sites in {} mode, trying {}", thumb ? "Thumb" : "ARM", thumb ? "ARM" : "Thumb");
    }
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

    // Experiment 4: disassemble the game code around SmartAR call sites to
    // find the exact field/branch the game checks for "tracking locked".
    disassemble_smartar_callsites(emuenv);
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

    // NOTE: Experiment 2d (write identity pose to result struct +0x10..+0x28)
    // was REMOVED — it crashed because the LLE DispatchAndQuery reads back
    // the p10 pointer from result+0x10 on subsequent calls, and overwriting
    // it with a float broke the LLE function. The result struct's pointer
    // fields cannot be overwritten. The pose needs to go into the
    // sub-buffers pointed to by those pointers, or RunCore needs to be
    // HLE-overridden entirely.
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

// GetLandmarkInfo: entry r0=out buf, r1=size(=512), r3=constant(0x2090=8336).
// retval in r2. Recon shows the game calls this every frame with FIXED args
// (r3 is always 8336 — NOT a per-landmark index; it's a struct offset). The
// output buffer is at struct_base + 0x2798.
//
// Experiment 4 disassembly revealed the actual "tracking locked" gate:
// After GetLandmarkInfo returns, the game stores retval at [struct+0x794],
// then loads [struct+0x54] and tests bit 0 (`ands sb, sb, #1`). If bit 0 is
// 0, a critical function call (blx #0x8185df64) is SKIPPED. Our earlier
// Experiment 2 faked the retval/buffer but never touched struct+0x54, so the
// gate stayed closed.
//
// Experiment 2c: fake retval + buffer AND set struct+0x54 bit 0. The struct
// base is derived from the output buffer: struct_base = buf - 0x2798.
// Also set struct+0x794 = 1 (the stored landmark count the game writes).
// PropagateResult's prop+0x08=2 override (Experiment 1) remains active.
static void dump_landmark_info(EmuEnvState &emuenv, CPUState &cpu) {
    const Address buf = cpu.get_reg(0); // entry r0 = output buffer = struct + 0x2798
    const uint32_t size = cpu.get_reg(1); // entry r1
    const uint32_t retval = cpu.get_reg(2); // LLE retval
    const uint32_t index = cpu.get_reg(3); // entry r3 (struct offset 0x2090, not landmark index)

    const bool fake_it = (retval == 0 && buf != 0);
    if (fake_it) {
        uint8_t *out = Ptr<uint8_t>(buf).get(emuenv.mem);
        if (out) {
            // --- Fake the landmark buffer (Experiment 2) ---
            std::memset(out, 0, std::min<uint32_t>(size, 512));
            auto setf = [&](size_t off, float v) {
                std::memcpy(out + off, &v, 4);
            };
            auto setu = [&](size_t off, uint32_t v) {
                std::memcpy(out + off, &v, 4);
            };
            setf(0x00, 0.0f); // pos x
            setf(0x04, 0.0f); // pos y
            setf(0x08, 1.0f); // pos z (1m ahead of camera)
            setf(0x0C, 1.0f); // quality / confidence
            setu(0x10, 1u); // valid flag / id
            setf(0x18, 1.0f); // secondary quality / scale

            // Change the game-visible return value via set_reg(6, 1).
            cpu.set_reg(6, 1u);

            // --- NEW: set struct+0x2054 bit 0 (Experiment 2c, FIXED) ---
            // Disassembly shows the game tests `[struct_game + 0x2000 + 0x54] & 1` after
            // this call. struct_base (as computed below) = struct_game, but the game
            // reads from struct_game + 0x2000, so the actual offset is +0x2054, NOT +0x54.
            // Previous version used +0x54 (off by 0x2000 — that's why it didn't work!).
            // The landmark output buffer is at struct_game + 0x2798, so
            // struct_game = buf - 0x2798, and struct_game + 0x2000 = struct_game + 0x2798 - 0x2798 + 0x2000 = buf - 0x744.
            constexpr uint32_t LANDMARK_BUF_OFFSET = 0x2798;
            constexpr uint32_t STRUCT_STATE_OFFSET = 0x2054; // game reads [struct_game+0x2000+0x54]
            constexpr uint32_t STRUCT_LMCOUNT_OFFSET = 0x2794; // game writes [struct_game+0x2000+0x794]
            // struct_game + 0x2000 = buf - 0x2798 + 0x2000 = buf - 0x744
            const Address struct_plus_2000 = buf - 0x2798 + 0x2000; // = buf - 0x744
            uint8_t *strukt2k = Ptr<uint8_t>(struct_plus_2000).get(emuenv.mem);
            uint32_t orig_54 = 0, orig_794 = 0;
            if (strukt2k) {
                std::memcpy(&orig_54, strukt2k + 0x54, 4);
                std::memcpy(&orig_794, strukt2k + 0x794, 4);
                // Set bit 0 of struct+0x2054 (preserve other bits).
                uint32_t new_54 = orig_54 | 1u;
                std::memcpy(strukt2k + 0x54, &new_54, 4);
                // Set struct+0x2794 = 1 (landmark count, matching our faked retval).
                uint32_t one = 1u;
                std::memcpy(strukt2k + 0x794, &one, 4);
            }

            LOG_INFO("SmartAR recon [frame {}] GetLandmarkInfo retval={} buf={} size={} index={} "
                     "EXP2c: faked landmark (retval->1, buf filled, struct2k={} +0x2054: {}->{} (bit0 set), +0x2794: {}->1)",
                frame, log_hex(retval), log_hex(buf), size, index,
                log_hex(struct_plus_2000), log_hex(orig_54), log_hex(orig_54 | 1u), log_hex(orig_794));
            return;
        }
    }

    // Default: log LLE's actual output (throttled).
    static uint32_t lmi_logged = 0;
    const bool do_log = (lmi_logged < 10) || (frame % 500 == 0);
    if (do_log) {
        LOG_INFO("SmartAR recon [frame {}] GetLandmarkInfo retval={} buf={} size={} index={}",
            frame, log_hex(retval), log_hex(buf), size, index);
        if (buf && size > 0 && size <= 1024)
            dump_hex(emuenv, buf, std::min<uint32_t>(size, 64), "lmi");
        ++lmi_logged;
    }
}

// PropagateResult: entry r0 = result/state ptr (we dump 64 bytes at it).
//
// Step 2 experiment: after LLE runs, MODIFY the prop buffer to fake
// "tracking locked", then log. The prop buffer is constant across all phases
// at +0x08=1, +0x0C=1, +0x1C=1.0f, +0x20=0x80000000. Hypothesis (from Sony
// SmartAR SceSmartSceneMappingState: 0=NOT_INITIALIZED, 1=INITIALIZING,
// 2=NORMAL/tracking-locked, 3=LOST): +0x08 is the state enum, currently 1
// (INITIALIZING). We force it to 2 (NORMAL). +0x20=0x80000000 looks like an
// "invalid" flag (bit 31 set), so we clear it to 0.
static void dump_propagate_result(EmuEnvState &emuenv, CPUState &cpu) {
    const Address state_ptr = cpu.get_reg(0); // entry r0
    const uint32_t arg1 = cpu.get_reg(1); // entry r1
    const uint32_t retval = cpu.get_reg(2); // LLE retval

    // --- Step 2: fake "tracking locked" in the prop buffer (host-side write
    // after LLE already ran in the trampoline). The game reads this buffer
    // after PropagateResult returns, so it will see our modified values. ---
    if (state_ptr) {
        uint8_t *prop = Ptr<uint8_t>(state_ptr).get(emuenv.mem);
        if (prop) {
            auto set32 = [&](size_t off, uint32_t v) {
                std::memcpy(prop + off, &v, 4);
            };
            const uint32_t orig_state = *reinterpret_cast<const uint32_t *>(prop + 0x08);
            const uint32_t orig_flag = *reinterpret_cast<const uint32_t *>(prop + 0x20);
            set32(0x08, 2u); // state: INITIALIZING(1) -> NORMAL(2) (tracking locked)
            set32(0x20, 0u); // clear "invalid" flag (bit 31)
            LOG_INFO("SmartAR recon [frame {}] PropagateResult retval={} state@={} arg1={} "
                     "OVERRODE +0x08: {}->2 (STATE_NORMAL), +0x20: {}->0",
                frame, log_hex(retval), log_hex(state_ptr), log_hex(arg1),
                orig_state, log_hex(orig_flag));
        } else {
            LOG_INFO("SmartAR recon [frame {}] PropagateResult retval={} state@={} (unmapped)",
                frame, log_hex(retval), log_hex(state_ptr));
        }
    } else {
        LOG_INFO("SmartAR recon [frame {}] PropagateResult retval={} state@=<null> arg1={}",
            frame, log_hex(retval), log_hex(arg1));
    }
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
