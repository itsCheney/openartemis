// Glyph drawing: a message layer with two faces, an outline and a shadow must
// draw the same pixels on a cold frame (every cache empty) and on warm frames
// (glyph textures, measures and per-font styles served from the caches), and
// a face the project does not ship must fall back to the default face without
// losing any glyph. Uses a system TrueType font as the project's default face
// and skips (77) without one or without an offscreen renderer.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
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

std::string slurp(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), {});
}

const char* const kFontCandidates[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/Library/Fonts/Arial Unicode.ttf",
    "/System/Library/Fonts/Supplemental/Arial.ttf",
};
const char* const kSecondFontCandidates[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",
    "/usr/share/fonts/TTF/DejaVuSerif.ttf",
    "/System/Library/Fonts/Supplemental/Georgia.ttf",
};

struct Scene {
    std::shared_ptr<MemFs> fs = std::make_shared<MemFs>();
    std::unique_ptr<oa::runtime::GameRuntime> rt;
    Scene(const std::string& main_font, const std::string& second_font,
          const std::string& body) {
        fs->files["system.ini"] =
            "[WINDOWS]\nWIDTH = 256\nHEIGHT = 128\nFPS = 60\nCHARSET = UTF-8\n"
            "BOOT = system/first.iet\nSAVEPATH = save\n";
        fs->files["system/first.iet"] = "*main\n" + body + "[stop]\n";
        fs->files["font/sourcehansans-medium.otf"] = main_font;
        fs->files["font/second.ttf"] = second_font;
        rt = std::make_unique<oa::runtime::GameRuntime>(fs);
        rt->open_project("windows");
        rt->boot_project();
        oa::runtime::FrameInput idle;
        for (int i = 0; i < 8; ++i) rt->tick(16, idle);
        (void)rt->drain_events();
    }
};

const std::string kBody =
    "[lyc id=\"1\" width=\"256\" height=\"128\" left=\"0\" top=\"0\"]\n"
    "[chgmsg id=\"1\" layered=\"1\"]\n"
    "[font left=\"4\" top=\"4\" width=\"248\" height=\"120\" size=\"22\" "
    "color=\"0xFFFFFF\" outline=\"2\" outlinecolor=\"0x2040C0\" shadow=\"2\" "
    "shadowcolor=\"0x000000\"]\n"
    "[print data=\"Glyph cache AV\"]\n"
    "[font face=\"font/second.ttf\" size=\"18\" color=\"0xFFE080\" outline=\"0\" shadow=\"0\"]\n"
    "[print data=\"Serif run\"]\n"
    "[font face=\"font/not-shipped\" size=\"16\"]\n"
    "[print data=\"fallback\"]\n"
    "[/chgmsg]\n";

struct Frame {
    std::vector<uint8_t> rgba;
    size_t glyphs = 0;
};

std::vector<Frame> render_frames(SDL_Window* window, Scene& scene, int n) {
    oa::render::RenderEngine re(scene.fs.get(), scene.rt.get());
    re.stage_w_ = 256;
    re.stage_h_ = 128;
    std::vector<Frame> out;
    if (!re.create_renderer(window)) return out;
    for (int i = 0; i < n; ++i) {
        Frame f;
        re.render_beigin();
        f.glyphs = re.draw_scene(scene.rt->scene(), 1.0);
        oa::media::Image img;
        re.snapshot_renderer(img);
        re.render_end();
        f.rgba = std::move(img.rgba);
        out.push_back(std::move(f));
    }
    return out;
}

size_t lit(const std::vector<uint8_t>& rgba) {
    size_t n = 0;
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) n += rgba[i] || rgba[i + 1] || rgba[i + 2];
    return n;
}
} // namespace

int main(int argc, char** argv) {
    std::string main_font, second_font;
    for (const char* p : kFontCandidates)
        if (main_font.empty()) main_font = slurp(p);
    for (const char* p : kSecondFontCandidates)
        if (second_font.empty()) second_font = slurp(p);
    if (main_font.empty()) {
        std::printf("SKIP: no system TrueType font\n");
        return 77;
    }
    if (second_font.empty()) second_font = main_font;
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("SKIP: no video (%s)\n", SDL_GetError());
        return 77;
    }
    SDL_Window* window = SDL_CreateWindow("text-render", 256, 128, SDL_WINDOW_HIDDEN);
    if (!window) {
        std::printf("SKIP: no window (%s)\n", SDL_GetError());
        SDL_Quit();
        return 77;
    }

    Scene scene(main_font, second_font, kBody);
    if (const char* dbg = std::getenv("OA_TEXT_RENDER_DEBUG"); dbg && *dbg) {
        const oa::render::MessageLayer* ml = scene.rt->text().layer("1");
        std::printf("probe: layer=%p page=%zu reveal=%llu pos=%d visible=%d drawable=%d bound='%s'\n",
            (const void*)ml, ml ? ml->page.size() : 0,
            ml ? (unsigned long long)ml->reveal_index : 0ull, ml ? ml->positioned : -1,
            scene.rt->scene().is_message_layer_visible("1"),
            scene.rt->scene().is_message_layer_drawable("1", ml && ml->positioned),
            scene.rt->scene().bound_scene_id("1").c_str());
    }
    const auto frames = render_frames(window, scene, 3);
    check(frames.size() == 3, "three frames rendered");
    if (frames.size() == 3) {
        const size_t px = lit(frames[0].rgba);
        std::printf("cold frame: %zu draws, %zu lit pixels\n", frames[0].glyphs, px);
        check(px > 1500, "text is drawn");
        check(frames[1].rgba == frames[0].rgba, "warm frame matches the cold frame");
        check(frames[2].rgba == frames[0].rgba, "second warm frame matches");
        check(frames[1].glyphs == frames[0].glyphs, "same draw count when warm");
        // Optional: dump the cold frame for comparisons across builds.
        if (argc > 1) {
            if (FILE* f = std::fopen(argv[1], "wb")) {
                std::fwrite(frames[0].rgba.data(), 1, frames[0].rgba.size(), f);
                std::fclose(f);
            }
        }
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("text render: all checks passed\n");
    return 0;
}
