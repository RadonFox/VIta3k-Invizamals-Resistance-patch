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

#include <module/module.h>

#include <emuenv/state.h>
#include <kernel/state.h>
#include <mem/functions.h>
#include <modules/module_parent.h>
#include <util/log.h>

#include <array>
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Hybrid SmartAR (Invizimals: The Resistance)
//
// The real libsmart.suprx stays loaded (LLE) because its 2D feature detection
// is what fills the color bar. After it loads, install_smart_hybrid_overrides()
// redirects sceSmartSceneMappingDispatchAndQuery to the HLE export below, which
// calls the original LLE function and then:
//   1. logs every 32-bit word of the result struct that changed since the last
//      call (so the tracking-state field can be found by watching the log while
//      the color bar fills / the button is pressed), and
//   2. optionally forces words in the result, driven by the env var
//      VITA3K_SMART_FORCE, e.g.  VITA3K_SMART_FORCE=0x30=2,0x34=1
//      (offset=value pairs, hex or decimal). No rebuild needed to experiment.
// ---------------------------------------------------------------------------

namespace {

constexpr uint32_t NID_sceSmartSceneMappingDispatchAndQuery = 0xF10FDDD1;

// How much of the result struct to watch. Recon showed real data past 0x40.
constexpr uint32_t SMART_RESULT_WATCH_SIZE = 0x100;
constexpr uint32_t SMART_RESULT_WORDS = SMART_RESULT_WATCH_SIZE / 4;

struct SmartHybridState {
    std::mutex mutex;
    Address lle_dispatch_and_query = 0;
    uint64_t call_count = 0;
    bool have_prev = false;
    std::array<uint32_t, SMART_RESULT_WORDS> prev{};
    std::vector<std::pair<uint32_t, uint32_t>> forced; // (offset, value)
};

SmartHybridState &smart_state() {
    static SmartHybridState state;
    return state;
}

std::vector<std::pair<uint32_t, uint32_t>> parse_force_env() {
    std::vector<std::pair<uint32_t, uint32_t>> out;
    const char *env = std::getenv("VITA3K_SMART_FORCE");
    if (!env || !*env)
        return out;

    std::string spec(env);
    size_t pos = 0;
    while (pos < spec.size()) {
        size_t end = spec.find(',', pos);
        if (end == std::string::npos)
            end = spec.size();
        const std::string item = spec.substr(pos, end - pos);
        const size_t eq = item.find('=');
        if (eq != std::string::npos) {
            try {
                const uint32_t offset = static_cast<uint32_t>(std::stoul(item.substr(0, eq), nullptr, 0));
                const uint32_t value = static_cast<uint32_t>(std::stoul(item.substr(eq + 1), nullptr, 0));
                if (offset % 4 == 0 && offset < SMART_RESULT_WATCH_SIZE)
                    out.emplace_back(offset, value);
                else
                    LOG_WARN("SmartAR hybrid: ignoring force entry '{}' (offset must be 4-aligned and < {})", item, log_hex(SMART_RESULT_WATCH_SIZE));
            } catch (...) {
                LOG_WARN("SmartAR hybrid: could not parse force entry '{}'", item);
            }
        }
        pos = end + 1;
    }
    return out;
}

} // namespace

void install_smart_hybrid_overrides(EmuEnvState &emuenv) {
    auto &st = smart_state();
    const Address lle = register_hle_override(emuenv, NID_sceSmartSceneMappingDispatchAndQuery);

    const std::lock_guard<std::mutex> guard(st.mutex);
    st.lle_dispatch_and_query = lle;
    st.call_count = 0;
    st.have_prev = false;
    st.forced = parse_force_env();

    if (!lle) {
        LOG_ERROR("SmartAR hybrid: libsmart has no LLE export for sceSmartSceneMappingDispatchAndQuery; override not active");
        return;
    }
    LOG_INFO("SmartAR hybrid: wrapping sceSmartSceneMappingDispatchAndQuery (LLE at {})", log_hex(lle));
    for (const auto &[offset, value] : st.forced)
        LOG_INFO("SmartAR hybrid: will force result+{} = {}", log_hex(offset), log_hex(value));
}

EXPORT(int, sceSmartCreateInstantImageTarget) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartCreateLearnedImageTarget) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartDestroyTarget) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartGetTargetInfo) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartInit) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartRelease) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartSceneMappingDispatchAndQuery, uint32_t ctx, uint32_t result, uint32_t arg2, uint32_t arg3) {
    auto &st = smart_state();
    Address lle;
    {
        const std::lock_guard<std::mutex> guard(st.mutex);
        lle = st.lle_dispatch_and_query;
    }
    if (!lle)
        return UNIMPLEMENTED(); // libsmart running as HLE: nothing to wrap

    const auto thread = emuenv.kernel.get_thread(thread_id);
    const auto ret_opt = thread->call_guest(lle, { ctx, result, arg2, arg3 });
    if (!ret_opt)
        return 0; // thread/app is ending
    const int ret = static_cast<int>(*ret_opt);

    if (!result || !is_valid_addr_len(emuenv.mem, result, SMART_RESULT_WATCH_SIZE))
        return ret;

    uint32_t *words = Ptr<uint32_t>(result).get(emuenv.mem);

    const std::lock_guard<std::mutex> guard(st.mutex);
    const uint64_t n = st.call_count++;

    // Log what the real SmartAR wrote (before any forcing), only the words that changed.
    if (!st.have_prev) {
        std::string line;
        for (uint32_t i = 0; i < SMART_RESULT_WORDS; i++)
            line += fmt::format("{}{:08X}", (i % 8 == 0) ? fmt::format("\n  +{:03X}: ", i * 4) : " ", words[i]);
        LOG_INFO("SmartAR DispatchAndQuery #{} ret={} ctx={} result={} full dump:{}", n, log_hex(ret), log_hex(ctx), log_hex(result), line);
    } else {
        std::string changes;
        for (uint32_t i = 0; i < SMART_RESULT_WORDS; i++) {
            if (words[i] != st.prev[i])
                changes += fmt::format(" +{:03X}:{:08X}->{:08X}", i * 4, st.prev[i], words[i]);
        }
        if (!changes.empty())
            LOG_INFO("SmartAR DispatchAndQuery #{} ret={} changed:{}", n, log_hex(ret), changes);
    }
    for (uint32_t i = 0; i < SMART_RESULT_WORDS; i++)
        st.prev[i] = words[i];
    st.have_prev = true;

    // Experimental: force fields chosen via VITA3K_SMART_FORCE.
    for (const auto &[offset, value] : st.forced)
        words[offset / 4] = value;

    return ret;
}

EXPORT(int, sceSmartSceneMappingDispatchAndQueryWithMask) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartSceneMappingEnableMask) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartSceneMappingFixMap) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartSceneMappingForceLocalize) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartSceneMappingGetInitializationPointInfo) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartSceneMappingGetLandmarkInfo) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartSceneMappingGetNodePointInfo) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartSceneMappingGetNumInitializationPoints) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartSceneMappingGetNumLandmarks) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartSceneMappingGetNumNodePoints) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartSceneMappingLoadMap) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartSceneMappingPropagateResult) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartSceneMappingRegisterTarget) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartSceneMappingRemoveLandmark) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartSceneMappingReset) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartSceneMappingRun) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartSceneMappingRunCore) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartSceneMappingSaveMap) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartSceneMappingSetCustomListener) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartSceneMappingSetDenseMapMode) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartSceneMappingSetDispatchMode) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartSceneMappingStart) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartSceneMappingStop) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartSceneMappingUnregisterTarget) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartTargetTrackingDispatchAndQuery) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartTargetTrackingGetResults) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartTargetTrackingQuery) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartTargetTrackingQuery2) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartTargetTrackingRegisterTarget) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartTargetTrackingReset) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartTargetTrackingRun) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartTargetTrackingRun2) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartTargetTrackingRunWorker) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartTargetTrackingSetSearchPolicy) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartTargetTrackingStart) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartTargetTrackingStop) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceSmartTargetTrackingUnregisterTarget) {
    return UNIMPLEMENTED();
}
