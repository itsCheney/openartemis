#pragma once
// Per-stage CPU time of the frame loop, for the host's diagnostics. Every
// stage is timed on the thread that runs the frame; a host reads and resets
// the totals once per reporting window (take_summary). Stages nest: scene
// time includes the group bakes drawn inside it, host pump time includes the
// emote pose and its canvas draw.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

namespace oa::prof {

enum class Stage : int {
    Tick,        // GameRuntime::tick (script, Lua, tweens, media clocks)
    HostPump,    // emote poses + video frames into textures
    EmotePose,   // EmotePlayer::collect_pose_parts (CPU mesh evaluation)
    EmoteDraw,   // emote parts drawn into their canvas
    SceneDraw,   // draw_scene (all layers, glyphs, group bakes)
    GroupGpu,    // intermediate-render group baked on the GPU
    GroupCpu,    // group baked on the CPU (read-back + per-pixel + upload)
    Readback,    // GPU target read back to the CPU (inside GroupCpu)
    Present,     // render_end (window blit + present)
    Count
};

// Counters that are not timed stages.
enum class Counter : int {
    Frames,           // frames iterated
    Redraws,          // frames that redrew the scene
    BakeClearsAll,    // frames that dropped every group bake
    BakeClearsHost,   // frames that dropped only bakes holding a new host frame
    ReadbackBytes,    // bytes read back from the GPU
    Count
};

namespace detail {
struct StageTotals {
    std::atomic<uint64_t> calls{0}, ns{0}, max_ns{0};
};
inline StageTotals g_stages[int(Stage::Count)];
inline std::atomic<uint64_t> g_counters[int(Counter::Count)];
inline uint64_t now_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}
} // namespace detail

inline void add(Stage s, uint64_t ns) {
    auto& t = detail::g_stages[int(s)];
    t.calls.fetch_add(1, std::memory_order_relaxed);
    t.ns.fetch_add(ns, std::memory_order_relaxed);
    uint64_t prev = t.max_ns.load(std::memory_order_relaxed);
    while (ns > prev &&
           !t.max_ns.compare_exchange_weak(prev, ns, std::memory_order_relaxed)) {
    }
}

inline void count(Counter c, uint64_t n = 1) {
    detail::g_counters[int(c)].fetch_add(n, std::memory_order_relaxed);
}

class Scope {
public:
    explicit Scope(Stage s) : stage_(s), start_(detail::now_ns()) {}
    ~Scope() { add(stage_, detail::now_ns() - start_); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Stage stage_;
    uint64_t start_;
};

// "tick:calls/totalNS/maxNS,...;frames:N,redraws:N,..." since the last call,
// then resets every total.
inline std::string take_summary() {
    static const char* const stage_names[] = {
        "tick", "hostPump", "emotePose", "emoteDraw", "sceneDraw",
        "groupGPU", "groupCPU", "readback", "present"};
    static const char* const counter_names[] = {
        "frames", "redraws", "bakeClearsAll", "bakeClearsHost", "readbackBytes"};
    static_assert(sizeof(stage_names) / sizeof(*stage_names) == int(Stage::Count));
    static_assert(sizeof(counter_names) / sizeof(*counter_names) == int(Counter::Count));
    std::string out;
    char buf[96];
    for (int i = 0; i < int(Stage::Count); ++i) {
        auto& t = detail::g_stages[i];
        std::snprintf(buf, sizeof(buf), "%s%s:%llu/%llu/%llu", i ? "," : "",
                      stage_names[i],
                      (unsigned long long)t.calls.exchange(0, std::memory_order_relaxed),
                      (unsigned long long)t.ns.exchange(0, std::memory_order_relaxed),
                      (unsigned long long)t.max_ns.exchange(0, std::memory_order_relaxed));
        out += buf;
    }
    for (int i = 0; i < int(Counter::Count); ++i) {
        std::snprintf(buf, sizeof(buf), "%s%s:%llu", i ? "," : ";", counter_names[i],
                      (unsigned long long)detail::g_counters[i].exchange(
                          0, std::memory_order_relaxed));
        out += buf;
    }
    return out;
}

} // namespace oa::prof
