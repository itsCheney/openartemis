#pragma once

// Shared between the bridge (MikageArtemisRuntime.mm) and the iOS platform
// layer (MikagePlatform.cpp); not part of the framework's API.

// The app brought the game back from the background: the host loop resets
// its tick clock on the next frame.
void mikage_platform_note_resume();
// Whether a three-finger touch opens Mikage's menu.
void mikage_platform_set_menu_gesture(bool enabled);
