// Intermediate-render groups: the GPU composite (blend passes on a
// premultiplied target) must match the CPU composite (read-back + per-pixel
// filters) for color multiply, negative and both together; and a new host
// frame (emote pose, video frame) must only re-bake the groups that hold it.
// Needs a renderer with custom blend modes (GLES through the offscreen video
// driver); skips (77) where only the software renderer exists.
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "core/fs/fs.h"
#include "core/media/image.h"
#include "core/render/renderer.h"
#include "core/runtime/runtime.h"
#include "core/util/stage_profile.h"

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

struct MemFs : oa::fs::IFileSystem {
    std::map<std::string, std::string> files;
    std::optional<std::vector<uint8_t>> read(std::string_view path) const override {
        const auto it = files.find(std::string(path));
        if (it == files.end()) return std::nullopt;
        return std::vector<uint8_t>(it->second.begin(), it->second.end());
    }
    bool exists(std::string_view path) const override {
        return files.count(std::string(path)) > 0;
    }
    const char* kind() const override { return "mem"; }
};

// 64x64: color gradients with a translucent band, so un-premultiplying and
// the alpha handling of every filter are exercised.
std::string fixture_png() {
    std::vector<uint8_t> rgba(64 * 64 * 4);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            uint8_t* p = rgba.data() + (y * 64 + x) * 4;
            p[0] = uint8_t(x * 4);
            p[1] = uint8_t(y * 4);
            p[2] = uint8_t(255 - x * 2);
            p[3] = y < 16 ? 96 : 255;
        }
    const std::vector<uint8_t> png = oa::media::encode_png(64, 64, rgba);
    return std::string(png.begin(), png.end());
}

// 16x16 alpha ramp for intermediate_render_mask (stretched over the stage).
std::string mask_png() {
    std::vector<uint8_t> rgba(16 * 16 * 4, 255);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) rgba[(y * 16 + x) * 4 + 3] = uint8_t(x * 16 + y);
    const std::vector<uint8_t> png = oa::media::encode_png(16, 16, rgba);
    return std::string(png.begin(), png.end());
}

std::string tag_attrs(const std::map<std::string, std::string>& props) {
    std::string out;
    for (const auto& [k, v] : props) out += " " + k + "=\"" + v + "\"";
    return out;
}

// Group 1 carries the filters under test; group 2 is a plain translucent
// group beside it.
struct Scene {
    std::shared_ptr<MemFs> fs = std::make_shared<MemFs>();
    std::unique_ptr<oa::runtime::GameRuntime> rt;
    explicit Scene(const std::map<std::string, std::string>& group_props) {
        fs->files["system.ini"] =
            "[WINDOWS]\nWIDTH = 128\nHEIGHT = 128\nFPS = 60\nCHARSET = UTF-8\n"
            "BOOT = system/first.iet\nSAVEPATH = save\n";
        fs->files["system/first.iet"] =
            "*main\n[lyc id=\"1\" intermediate_render=\"1\"" + tag_attrs(group_props) +
            "]\n[lyc id=\"1.1\" file=\"fg\" left=\"16\" top=\"24\"]\n"
            "[lyc id=\"2\" intermediate_render=\"1\" alpha=\"200\"]\n"
            "[lyc id=\"2.1\" file=\"fg\" left=\"60\" top=\"60\"]\n[stop]\n";
        fs->files["fg.png"] = fixture_png();
        fs->files["mask.png"] = mask_png();
        rt = std::make_unique<oa::runtime::GameRuntime>(fs);
        rt->open_project("windows");
        rt->boot_project();
        oa::runtime::FrameInput idle;
        rt->tick(16, idle);
        (void)rt->drain_events();
    }
};

std::vector<uint8_t> render(SDL_Window* window, Scene& scene, bool gpu) {
    oa::render::RenderEngine re(scene.fs.get(), scene.rt.get());
    re.stage_w_ = 128;
    re.stage_h_ = 128;
    re.set_gpu_group_composite(gpu);
    if (!re.create_renderer(window)) return {};
    re.render_beigin();
    re.draw_scene(scene.rt->scene(), 1.0);
    oa::media::Image out;
    re.snapshot_renderer(out);
    re.render_end();
    return out.rgba;
}

void compare(SDL_Window* window, const std::map<std::string, std::string>& props,
             const char* what) {
    Scene cpu_scene(props), gpu_scene(props);
    const auto cpu = render(window, cpu_scene, false);
    const auto gpu = render(window, gpu_scene, true);
    check(!cpu.empty() && cpu.size() == gpu.size(), what);
    if (cpu.empty() || cpu.size() != gpu.size()) return;
    int worst = 0, covered = 0;
    for (size_t i = 0; i < cpu.size(); ++i) {
        worst = std::max(worst, std::abs(int(cpu[i]) - int(gpu[i])));
        if (i % 4 == 0) covered += cpu[i] || cpu[i + 1] || cpu[i + 2];
    }
    std::printf("%s: worst channel difference %d, lit pixels %d\n", what, worst, covered);
    // 8-bit premultiplied blending rounds differently from the CPU's doubles
    // in the translucent band; anything above a few steps is a real mismatch.
    check(covered > 64 * 32 && worst <= 4, what);
}

// Grayscale stays on the CPU composite: every lit pixel is gray, the clip
// bounds the group, and the result is dumped for comparisons across builds.
void test_grayscale(SDL_Window* window, const char* dump_path) {
    Scene scene({{"grayscale", "1"},
                 {"colormultiply", "0xC0FF80"},
                 {"negative", "1"},
                 {"clip", "8,20,64,52"},
                 {"intermediate_render_mask", "mask"}});
    const auto px = render(window, scene, true);
    check(px.size() == 128 * 128 * 4, "grayscale frame rendered");
    if (px.size() != 128 * 128 * 4) return;
    int gray = 0, colored = 0;
    for (int y = 20; y < 72; ++y)
        for (int x = 8; x < 60; ++x) {
            // left of group 2's sprite (x >= 60): only group 1 lands here
            const uint8_t* p = px.data() + (y * 128 + x) * 4;
            if (!p[0] && !p[1] && !p[2]) continue;
            (p[0] == p[1] && p[1] == p[2] ? gray : colored)++;
        }
    std::printf("grayscale: %d gray, %d colored pixels in the clip\n", gray, colored);
    check(gray > 500 && colored == 0, "grayscale group composites gray");
    // The sprite spans x 16..80, y 24..88; the clip ends at x 72. Above
    // group 2 (y < 60) the strip right of the clip must stay empty.
    int outside = 0;
    for (int y = 24; y < 60; ++y)
        for (int x = 72; x < 80; ++x) {
            const uint8_t* p = px.data() + (y * 128 + x) * 4;
            outside += p[0] || p[1] || p[2];
        }
    check(outside == 0, "nothing of the grayscale group lands outside its clip");
    if (dump_path) {
        if (FILE* f = std::fopen(dump_path, "wb")) {
            std::fwrite(px.data(), 1, px.size(), f);
            std::fclose(f);
        }
    }
}

void test_host_frame_rebake(SDL_Window* window) {
    Scene scene({{"negative", "1"}});
    oa::render::RenderEngine re(scene.fs.get(), scene.rt.get());
    re.stage_w_ = 128;
    re.stage_h_ = 128;
    re.set_gpu_group_composite(true);
    check(re.create_renderer(window), "renderer for re-bake test");
    const auto bakes = [&] {
        re.render_beigin();
        re.draw_scene(scene.rt->scene(), 1.0);
        re.render_end();
        const std::string summary = oa::prof::take_summary();
        const size_t at = summary.find("groupGPU:");
        return at == std::string::npos ? -1 : std::atoi(summary.c_str() + at + 9);
    };
    oa::prof::take_summary();
    check(bakes() == 2, "first frame bakes both groups");
    check(bakes() == 0, "unchanged frame reuses both bakes");
    re.render_clear_tex_cache_containing({"3.5"});
    check(bakes() == 0, "a host frame outside every group keeps both bakes");
    re.render_clear_tex_cache_containing({"1.1"});
    check(bakes() == 1, "a host frame inside group 1 re-bakes only group 1");
    re.render_clear_tex_cache_containing({"12"});
    check(bakes() == 0, "id prefix without a dot is not inside group 1");
    re.render_clear_tex_cache();
    check(bakes() == 2, "a full clear re-bakes both groups");
}
} // namespace

int main(int argc, char** argv) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("SKIP: no video (%s)\n", SDL_GetError());
        return 77;
    }
    SDL_Window* window = SDL_CreateWindow("group-composite", 128, 128,
                                          SDL_WINDOW_HIDDEN | SDL_WINDOW_OPENGL);
    SDL_Renderer* probe = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    const char* name = probe ? SDL_GetRendererName(probe) : nullptr;
    if (!name || std::string(name) == "software") {
        std::printf("SKIP: no hardware renderer with custom blend modes\n");
        SDL_Quit();
        return 77;
    }
    SDL_DestroyRenderer(probe);

    compare(window, {{"colormultiply", "0xFF8040"}}, "color multiply");
    compare(window, {{"negative", "1"}}, "negative");
    compare(window, {{"colormultiply", "0x80FFC0"}, {"negative", "1"}, {"alpha", "180"}},
            "color multiply + negative + group alpha");
    test_host_frame_rebake(window);
    test_grayscale(window, argc > 1 ? argv[1] : nullptr);

    SDL_DestroyWindow(window);
    SDL_Quit();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("group composite: all checks passed\n");
    return 0;
}
