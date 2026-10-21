// Runs openArtemis inside Mikage. See MikageArtemisRuntime.h.
//
// main.cpp is compiled into this framework unchanged apart from
// OA_HOST_LIBRARY (no SDL main(), Escape steps back instead of quitting).
// Its four SDL callbacks become a MikageSDLHostEngine, so Mikage's shared SDL
// host owns the event pump, the audio session, foreground gating and the
// end-of-session reset, exactly as for KiriKiri.

#include "MikageArtemisRuntime.h"
#include "MikageArtemisInternal.h"
#include "MikageSDLHost.h"

#define SDL_MAIN_HANDLED
#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "app_host.h"
#include "core/media/audio.h"
#include "core/media/image.h"
#include "core/render/renderer.h"
#include "core/runtime/runtime.h"
#include "core/util/stage_profile.h"
#include "platform/Platform.h"

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <algorithm>
#include <string>
#include <vector>

namespace {

std::atomic<MikageArtemisLogCallback> logCallback{nullptr};
// The running game's AppState (main.cpp), between init and quit.
AppState *appState = nullptr;
std::string platformName;

SDL_LogOutputFunction previousLogOutput = nullptr;
void *previousLogContext = nullptr;

void log(const char *source, int32_t level, const char *message)
{
    if (auto callback = logCallback.load(std::memory_order_acquire))
        callback(source, level, message ? message : "");
}

// The engine reports through printf/fprintf (image loads, E-mote and
// script warnings, render setup). stdout and stderr go through pipes whose
// reader threads copy everything to the original descriptors (Xcode's
// console) and, while a game runs, hand each line to the log callback.
std::atomic<bool> consoleForwarding{false};

void forwardConsoleLine(const char *source, int32_t level, const std::string &line)
{
    if (!consoleForwarding.load(std::memory_order_acquire) || line.empty())
        return;
    // A script printing every frame must not flood the diagnostics log.
    constexpr int linesPerSecond = 1000;
    static std::mutex mutex;
    static auto windowStart = std::chrono::steady_clock::now();
    static int linesInWindow = 0;
    static int dropped = 0;
    std::string note;
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto now = std::chrono::steady_clock::now();
        if (now - windowStart >= std::chrono::seconds(1)) {
            if (dropped > 0)
                note = std::to_string(dropped) + " console lines dropped (over " +
                       std::to_string(linesPerSecond) + "/s)";
            windowStart = now;
            linesInWindow = 0;
            dropped = 0;
        }
        if (linesInWindow >= linesPerSecond) {
            ++dropped;
            return;
        }
        ++linesInWindow;
    }
    if (!note.empty())
        log("console", 4, note.c_str());
    log(source, level, line.c_str());
}

void captureConsoleStream(int fd, const char *source, int32_t level)
{
    int pipeFds[2];
    if (pipe(pipeFds) != 0)
        return;
    const int original = dup(fd);
    if (original < 0 || dup2(pipeFds[1], fd) < 0) {
        if (original >= 0)
            close(original);
        close(pipeFds[0]);
        close(pipeFds[1]);
        return;
    }
    close(pipeFds[1]);
    std::thread([readFd = pipeFds[0], original, source, level] {
        std::string line;
        char buffer[4096];
        for (;;) {
            const ssize_t count = read(readFd, buffer, sizeof buffer);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                break;
            for (ssize_t written = 0; written < count;) {
                const ssize_t n = write(original, buffer + written, size_t(count - written));
                if (n < 0 && errno == EINTR)
                    continue;
                if (n <= 0)
                    break;
                written += n;
            }
            for (ssize_t i = 0; i < count; ++i) {
                const char c = buffer[i];
                if (c == '\n' || line.size() >= 2000) {
                    forwardConsoleLine(source, level, line);
                    line.clear();
                    if (c == '\n')
                        continue;
                }
                if (c != '\r')
                    line.push_back(c);
            }
        }
    }).detach();
}

void startConsoleCapture()
{
    static std::once_flag once;
    std::call_once(once, [] {
        std::fflush(stdout);
        std::fflush(stderr);
        captureConsoleStream(STDOUT_FILENO, "stdout", 3);
        captureConsoleStream(STDERR_FILENO, "stderr", 4);
        // A pipe makes stdout fully buffered; keep lines in order with stderr.
        std::setvbuf(stdout, nullptr, _IOLBF, 0);
    });
}

void forwardSDLLog(void *, int category, SDL_LogPriority priority, const char *message)
{
    const int32_t level = priority >= SDL_LOG_PRIORITY_ERROR ? 5
                        : priority == SDL_LOG_PRIORITY_WARN ? 4 : 3;
    log("SDL", level, message);
    if (previousLogOutput)
        previousLogOutput(previousLogContext, category, priority, message);
}

// Frame statistics over one-second windows, from the host's frame reports.
constexpr Uint64 nanosecondsPerSecond = 1000000000ULL;
Uint64 statsWindowStarted = 0;
Uint64 previousFrameAt = 0;
Uint64 frameIntervalTotal = 0;
Uint64 frameCount = 0;
Uint64 frameWorkTotal = 0, frameWorkMax = 0;
double currentFPS = 0, currentFrameTimeMS = 0;
double currentCpuFrameTimeMS = 0, currentMaxCpuFrameTimeMS = 0;

void resetStats()
{
    statsWindowStarted = SDL_GetTicksNS();
    previousFrameAt = 0;
    frameIntervalTotal = 0;
    frameCount = 0;
    frameWorkTotal = frameWorkMax = 0;
    currentFPS = currentFrameTimeMS = 0;
    currentCpuFrameTimeMS = currentMaxCpuFrameTimeMS = 0;
}

void frameCompleted(const MikageSDLHostFrame *frame)
{
    const Uint64 at = frame->finishedNS;
    if (!statsWindowStarted)
        statsWindowStarted = at;
    if (previousFrameAt)
        frameIntervalTotal += at - previousFrameAt;
    previousFrameAt = at;
    ++frameCount;
    frameWorkTotal += frame->workNS;
    if (frame->workNS > frameWorkMax)
        frameWorkMax = frame->workNS;
    const Uint64 elapsed = at - statsWindowStarted;
    if (elapsed < nanosecondsPerSecond)
        return;
    currentFPS = static_cast<double>(frameCount) * nanosecondsPerSecond / elapsed;
    currentFrameTimeMS = frameCount > 1
        ? static_cast<double>(frameIntervalTotal) / (frameCount - 1) / 1e6 : 0;
    currentCpuFrameTimeMS = static_cast<double>(frameWorkTotal) / frameCount / 1e6;
    currentMaxCpuFrameTimeMS = static_cast<double>(frameWorkMax) / 1e6;
    statsWindowStarted = at;
    previousFrameAt = 0;
    frameIntervalTotal = 0;
    frameCount = 0;
    frameWorkTotal = frameWorkMax = 0;
}

SDL_AppResult hostInit(void **state, int argc, char *argv[])
{
    const SDL_AppResult result = SDL_AppInit(state, argc, argv);
    appState = static_cast<AppState *>(*state);
    if (appState)
        platformName = appState->opt.platform;
    return result;
}

SDL_AppResult hostQuit(void *state, SDL_AppResult result)
{
    // SDL_AppQuit deletes the AppState and quits SDL.
    appState = nullptr;
    SDL_AppQuit(state, result);
    return result;
}

void hostSessionEnded()
{
    std::fflush(stdout);
    consoleForwarding.store(false, std::memory_order_release);
    appState = nullptr;
    platformName.clear();
    mikage_platform_set_menu_gesture(false);
}

void hostSetAudioSuspended(bool suspended)
{
    if (appState && appState->rt)
        appState->rt->media_players().set_output_paused(suspended);
}

void hostSetActive(bool active)
{
    // The background gap must not reach the engine as one huge tick.
    if (active)
        mikage_platform_note_resume();
}

const MikageSDLHostEngine artemisEngine = {
    "openArtemis",
    hostInit,
    SDL_AppEvent,
    SDL_AppIterate,
    hostQuit,
    hostSessionEnded,
    hostSetAudioSuspended,
    hostSetActive,
    frameCompleted,
    resetStats,
    log,
};

bool isPack(const std::filesystem::path &path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".pfs";
}

// What openArtemis is given to open: root.pfs when the folder has one, the
// folder itself when it is an extracted project, otherwise its first pack.
// A pack mounts its own folder underneath, so loose files beside it (movies
// outside the pack) still resolve.
std::string projectSource(const std::filesystem::path &folder)
{
    namespace fs = std::filesystem;
    std::error_code error;
    if (fs::is_regular_file(folder / "root.pfs", error))
        return (folder / "root.pfs").string();
    if (fs::is_regular_file(folder / "system.ini", error))
        return folder.string();
    // root.pfs.000 and later volumes end in their number, so only first
    // packs match.
    std::vector<fs::path> packs;
    for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        if (it->is_regular_file(error) && isPack(it->path()))
            packs.push_back(it->path());
    }
    if (packs.empty())
        return std::string();
    std::sort(packs.begin(), packs.end());
    return packs.front().string();
}

} // namespace

extern "C" void MikageArtemisSetLogCallback(MikageArtemisLogCallback callback)
{
    logCallback.store(callback, std::memory_order_release);
}

extern "C" bool MikageArtemisStart(const MikageArtemisLaunchOptions *options,
                                   MikageArtemisMenuCallback menu,
                                   MikageArtemisCompletionCallback completion,
                                   void *context)
{
    if (MikageSDLHostIsRunning()) {
        MikageSDLHostSetLastError("An Artemis game is already running.");
        return false;
    }
    if (!options || !options->gamePath || !*options->gamePath ||
        !options->saveDirectory || !*options->saveDirectory) {
        MikageSDLHostSetLastError("The Artemis game or save folder is missing.");
        return false;
    }

    {
        namespace fs = std::filesystem;
        std::error_code error;
        const fs::path folder(options->gamePath);
        if (!fs::is_directory(folder, error)) {
            MikageSDLHostSetLastError("The Artemis game folder does not exist.");
            return false;
        }
        const std::string source = projectSource(folder);
        if (source.empty()) {
            MikageSDLHostSetLastError("No root.pfs or system.ini was found in the game folder.");
            return false;
        }
        fs::create_directories(fs::path(options->saveDirectory), error);

        if (!previousLogOutput) {
            SDL_GetLogOutputFunction(&previousLogOutput, &previousLogContext);
            SDL_SetLogOutputFunction(forwardSDLLog, nullptr);
        }
        startConsoleCapture();
        consoleForwarding.store(true, std::memory_order_release);
        mikage_platform_set_menu_gesture(options->menuGestureEnabled);
        // A resume latched by an earlier game must not reach this one.
        oa::plat::take_lifecycle_resume();

        std::vector<std::string> arguments = {
            "openartemis",
            "--save-root", options->saveDirectory,
            "--platform", "auto",
            source,
        };
        std::vector<char *> argv;
        for (auto &argument : arguments)
            argv.push_back(argument.data());
        argv.push_back(nullptr);

        std::string message = "session.start source=" + source;
        log("bridge", 3, message.c_str());
        try {
            const MikageSDLHostConfiguration configuration = {
                static_cast<int>(arguments.size()),
                argv.data(),
                options->windowScene,
                options->respectSilentMode,
                menu,
                completion,
                context,
            };
            // The host finishes the session itself when SDL_AppInit fails.
            if (!MikageSDLHostStart(&artemisEngine, &configuration)) {
                hostSessionEnded();
                return false;
            }
        } catch (const std::exception &error) {
            MikageSDLHostAbortStart(error.what() ? error.what() : "C++ exception while starting openArtemis.");
            return false;
        } catch (...) {
            MikageSDLHostAbortStart("Unknown C++ exception while starting openArtemis.");
            return false;
        }
        message = "session.started platform=" + platformName;
        log("bridge", 3, message.c_str());
    }
    return true;
}

extern "C" MikageArtemisStepResult MikageArtemisStep(void)
{
    try {
        return static_cast<MikageArtemisStepResult>(MikageSDLHostStep());
    } catch (const std::exception &error) {
        MikageSDLHostFinish(SDL_APP_FAILURE, error.what());
    } catch (...) {
        MikageSDLHostFinish(SDL_APP_FAILURE, "Unknown C++ exception while running openArtemis.");
    }
    return MIKAGE_ARTEMIS_STEP_FAILED;
}

extern "C" bool MikageArtemisSendBack(void)
{
    if (!MikageSDLHostIsForeground())
        return false;
    const bool sent = MikageSDLHostSendBackKey();
    log("bridge", sent ? 3 : 4, sent ? "runtime.back.sent" : "runtime.back.unavailable");
    return sent;
}

namespace {

oa::runtime::GameRuntime *activeRuntime()
{
    return MikageSDLHostIsRunning() && appState ? appState->rt.get() : nullptr;
}

} // namespace

extern "C" bool MikageArtemisRunCommand(int32_t command)
{
    // Between steps on the main thread, so never inside a runtime tick.
    oa::runtime::GameRuntime *runtime = activeRuntime();
    bool done = false;
    if (runtime) {
        switch (command) {
        case MikageArtemisCommandAuto:
            if (runtime->automode_allowed()) {
                runtime->host_set_automode(!runtime->automode_active());
                done = true;
            }
            break;
        case MikageArtemisCommandSkip:
            if (runtime->skip_allowed()) {
                runtime->host_set_skip(!runtime->skip_enabled());
                done = true;
            }
            break;
        case MikageArtemisCommandBacklog:
            runtime->host_open_backlog();
            done = true;
            break;
        default:
            break;
        }
    }
    const std::string message = "runtime.command." + std::to_string(command) + (done ? ".done" : ".unavailable");
    log("bridge", done ? 3 : 4, message.c_str());
    return done;
}

extern "C" uint32_t MikageArtemisGameControls(void)
{
    oa::runtime::GameRuntime *runtime = activeRuntime();
    if (!runtime)
        return 0;
    uint32_t controls = MikageArtemisControlCanBacklog;
    if (runtime->automode_allowed())
        controls |= MikageArtemisControlCanAuto;
    if (runtime->skip_allowed())
        controls |= MikageArtemisControlCanSkip;
    if (runtime->automode_active())
        controls |= MikageArtemisControlAuto;
    if (runtime->skip_enabled())
        controls |= MikageArtemisControlSkip;
    return controls;
}

extern "C" bool MikageArtemisSetControlSkip(bool held)
{
    return MikageSDLHostSendKey(SDL_SCANCODE_LCTRL, SDLK_LCTRL, held);
}

extern "C" bool MikageArtemisSendPointer(int32_t phase, float x, float y)
{
    return MikageSDLHostSendPointer(phase, x, y);
}

extern "C" void MikageArtemisRequestStop(void)
{
    MikageSDLHostRequestStop();
}

extern "C" bool MikageArtemisSetForeground(bool foreground)
{
    return MikageSDLHostSetForeground(foreground);
}

extern "C" bool MikageArtemisIsRunning(void)
{
    return MikageSDLHostIsRunning();
}

extern "C" const char *MikageArtemisLastError(void)
{
    return MikageSDLHostLastError();
}

extern "C" void *MikageArtemisNativeWindow(void)
{
    if (!appState || !appState->window)
        return nullptr;
    return MikageSDLHostUIKitWindow(appState->window);
}

extern "C" const char *MikageArtemisPlatform(void)
{
    return platformName.c_str();
}

extern "C" bool MikageArtemisCaptureFrame(MikageArtemisCapturedFrame *frame)
{
    if (!frame || frame->pixels || !appState || !appState->oaRender || MikageSDLHostIsStepping())
        return false;
    try {
        oa::media::Image image;
        if (!appState->oaRender->snapshot_renderer(image) || image.w <= 0 || image.h <= 0 ||
            image.rgba.size() < static_cast<size_t>(image.w) * image.h * 4)
            return false;
        const int32_t pitch = image.w * 4;
        auto *pixels = static_cast<uint8_t *>(std::malloc(static_cast<size_t>(pitch) * image.h));
        if (!pixels)
            return false;
        std::memcpy(pixels, image.rgba.data(), static_cast<size_t>(pitch) * image.h);
        // The stage has no meaningful alpha; it is always shown opaque.
        for (size_t i = 3; i < static_cast<size_t>(pitch) * image.h; i += 4)
            pixels[i] = 0xFF;
        frame->pixels = pixels;
        frame->width = image.w;
        frame->height = image.h;
        frame->pitch = pitch;
        return true;
    } catch (...) {
        // Never let an engine exception cross the C boundary into Swift.
        return false;
    }
}

extern "C" void MikageArtemisFreeCapturedFrame(MikageArtemisCapturedFrame *frame)
{
    if (!frame)
        return;
    std::free(frame->pixels);
    *frame = {};
}

extern "C" bool MikageArtemisGetStats(MikageArtemisStats *stats)
{
    if (!stats || !MikageSDLHostIsRunning() || !appState)
        return false;
    std::memset(stats, 0, sizeof(*stats));
    stats->framesPerSecond = currentFPS;
    stats->frameTimeMilliseconds = currentFrameTimeMS;
    stats->cpuFrameTimeMilliseconds = currentCpuFrameTimeMS;
    stats->maxCpuFrameTimeMilliseconds = currentMaxCpuFrameTimeMS;
    stats->drawCallsPerFrame = static_cast<double>(appState->s_last_drawn);
    std::snprintf(stats->stageProfile, sizeof(stats->stageProfile), "%s",
                  oa::prof::take_summary().c_str());
    if (appState->window) {
        int width = 0, height = 0;
        SDL_GetWindowSizeInPixels(appState->window, &width, &height);
        stats->drawableWidth = width;
        stats->drawableHeight = height;
        if (SDL_Renderer *renderer = SDL_GetRenderer(appState->window)) {
            if (const char *name = SDL_GetRendererName(renderer))
                std::snprintf(stats->renderer, sizeof(stats->renderer), "%s", name);
        } else {
            std::snprintf(stats->renderer, sizeof(stats->renderer), "%s", "gles");
        }
    }
    return true;
}
