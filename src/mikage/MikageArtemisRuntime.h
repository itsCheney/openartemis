#pragma once

// Host API that lets Mikage run openArtemis inside its own app.
//
// The engine is an SDL3 main-callback program (src/app/main.cpp). Mikage's
// SDL host (MikageSDLHost) drives those callbacks: MikageArtemisStart runs
// SDL_AppInit, and each MikageArtemisStep pumps SDL events and runs one
// SDL_AppIterate. Every call here must be made on the main thread.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum MikageArtemisStepResult {
    MIKAGE_ARTEMIS_STEP_IDLE = 0,
    MIKAGE_ARTEMIS_STEP_RUNNING = 1,
    MIKAGE_ARTEMIS_STEP_FINISHED = 2,
    MIKAGE_ARTEMIS_STEP_FAILED = 3
} MikageArtemisStepResult;

typedef void (*MikageArtemisMenuCallback)(void *context);
typedef void (*MikageArtemisCompletionCallback)(bool success, const char *message, void *context);
// level follows the KRKR host: 3 info, 4 warning, 5 error. Strings are
// borrowed for the duration of the call. May be called on engine worker
// threads.
typedef void (*MikageArtemisLogCallback)(const char *source, int32_t level, const char *message);

void MikageArtemisSetLogCallback(MikageArtemisLogCallback callback);

typedef struct MikageArtemisLaunchOptions {
    // The game folder. It holds root.pfs (or other .pfs packs), or an
    // extracted project with system.ini at its top.
    const char *gamePath;
    // Where saves go. Created if missing.
    const char *saveDirectory;
    // UIWindowScene the engine's window joins.
    void *windowScene;
    // Follow the Ring/Silent switch (ambient) instead of always playing.
    bool respectSilentMode;
    // A three-finger touch opens the host menu.
    bool menuGestureEnabled;
} MikageArtemisLaunchOptions;

// Starts the game: opens its project, boots its scripts and creates the
// window. Returns false with MikageArtemisLastError() set when it can't
// start; completion is then not called. Otherwise completion is called once,
// from a later step, when the game ends.
bool MikageArtemisStart(const MikageArtemisLaunchOptions *options,
                        MikageArtemisMenuCallback menu,
                        MikageArtemisCompletionCallback completion,
                        void *context);

// Call once per display refresh while running.
MikageArtemisStepResult MikageArtemisStep(void);

// Sends the game an Escape key press. openArtemis turns it into a right
// click, which Artemis games use to close menus and step back. Delivered on
// the next step.
bool MikageArtemisSendBack(void);

// Ends the game on the next step. Artemis saves as it goes (system data on
// every change), so nothing is lost by ending here.
void MikageArtemisRequestStop(void);

// Pauses the game and its audio in the background, and resumes them.
bool MikageArtemisSetForeground(bool foreground);

bool MikageArtemisIsRunning(void);
const char *MikageArtemisLastError(void);
// The engine's UIWindow, or NULL before it exists.
void *MikageArtemisNativeWindow(void);
// The system.ini section the game booted with ("android", "ios",
// "windows"); empty when not running.
const char *MikageArtemisPlatform(void);

// Copy of the game's stage at its own resolution, top-down RGBA8 and
// opaque. Free pixels with MikageArtemisFreeCapturedFrame.
typedef struct MikageArtemisCapturedFrame {
    uint8_t *pixels;
    int32_t width;
    int32_t height;
    int32_t pitch;
} MikageArtemisCapturedFrame;
bool MikageArtemisCaptureFrame(MikageArtemisCapturedFrame *frame);
void MikageArtemisFreeCapturedFrame(MikageArtemisCapturedFrame *frame);

// Frame statistics over the last second, for the host's performance
// display. False when no game is running.
typedef struct MikageArtemisStats {
    // Frames the game ran per second, and the average gap between them.
    double framesPerSecond;
    double frameTimeMilliseconds;
    // Time the engine ran per step (one display refresh), average and worst.
    double cpuFrameTimeMilliseconds;
    double maxCpuFrameTimeMilliseconds;
    // Size of the window's drawable in pixels.
    int32_t drawableWidth;
    int32_t drawableHeight;
    // SDL's renderer, e.g. "metal".
    char renderer[32];
    // Layers the engine drew in its last rendered frame.
    double drawCallsPerFrame;
} MikageArtemisStats;
bool MikageArtemisGetStats(MikageArtemisStats *stats);

#ifdef __cplusplus
}
#endif
