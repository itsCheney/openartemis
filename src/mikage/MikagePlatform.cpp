// oa::plat for iOS, inside Mikage's ArtemisRuntime.framework.
//
// Mikage owns the app: its UIApplication delegate, its scenes and its
// lifecycle. The engine runs as one SDL session the app steps once per
// display refresh (MikageSDLHost), so:
//   - default_save_root(): the bridge always passes --save-root (the game's
//     savedata folder, which the app's Storage page measures and exports).
//     The fallback keeps the desktop rule.
//   - lifecycle: the app stops stepping the engine while it is in the
//     background and tells the bridge when it comes back
//     (mikage_platform_note_resume); the host loop then resets its tick
//     clock once.
//   - configure_window(): the SDL window joins the UIWindowScene the app
//     handed to the host, rather than whichever scene SDL finds active.
//   - menu_gesture(): a three-finger touch opens Mikage's own menu.
//   - host_paces_frames(): the app's display link paces frames; sleeping
//     at the end of a frame would only stall the app's main thread.
#include "platform/Platform.h"

#include "MikageArtemisInternal.h"
#include "MikageSDLHost.h"

#include <atomic>
#include <filesystem>

#include <SDL3/SDL_video.h>

namespace {
std::atomic<bool> s_resume{false};
std::atomic<bool> s_menu_gesture{false};
}

void mikage_platform_note_resume()
{
    s_resume.store(true);
}

void mikage_platform_set_menu_gesture(bool enabled)
{
    s_menu_gesture.store(enabled);
}

namespace oa::plat {

std::string default_save_root(const std::string& data_path, bool data_is_dir) {
    std::string save_root = (data_is_dir ? std::filesystem::path(data_path)
                                         : std::filesystem::path(data_path).parent_path())
                                .string();
    if (save_root.empty()) save_root = ".";
    return save_root;
}

void init() {}

void shutdown() {}

bool take_lifecycle_resume() {
    return s_resume.exchange(false);
}

void configure_window(SDL_PropertiesID props) {
    if (void* scene = MikageSDLHostWindowScene())
        SDL_SetPointerProperty(props, SDL_PROP_WINDOW_CREATE_WINDOWSCENE_POINTER, scene);
}

void menu_gesture() {
    if (s_menu_gesture.load())
        MikageSDLHostNotifyMenu();
}

bool host_paces_frames() { return true; }

} // namespace oa::plat
