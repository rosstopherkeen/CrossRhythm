#pragma once

#include <cstdint>
#include <vector>

class GfxRenderer;

// Scenes for the charging animation and the City Scapes wallpaper (X4 Pro:
// CROSSINK_APP_CAP_CHARGING_ANIMATION). A scene is 21 frames, one per 5% step, 0..100%.
//
// Glass House is built into flash and is the default. More scenes are .rhythmscene files the user copies to
// /.scenes/ (preferred) or /scenes/ on the SD card; a scene is named by its file stem ("kr_market"),
// and "" means the built-in one. A scene file that is missing or fails its checks falls back to the
// built-in frames, so the sleep screen always has a picture.
//
// While the device sleeps on the charger, the sleep screen shows the frame for the current battery
// step. A timed deep-sleep wake checks the battery every few minutes and redraws only when the step
// rises; unplugging puts the normal sleep screen back. On builds without the capability every function
// is a cheap no-op so callers need no #if.
namespace ChargingAnimation {

constexpr uint32_t kCheckIntervalSeconds = 5 * 60;
constexpr int kSceneIdMax = 24;     // file stem, NUL included
constexpr int kSceneTitleMax = 32;  // plaque title stored in the file, NUL included

struct Scene {
  char id[kSceneIdMax];
  char title[kSceneTitleMax];
};

// Title of the built-in scene ("Glass House").
const char* builtinTitle();
// Rescans /.scenes/ and /scenes/ for valid .rhythmscene (or legacy .cpscene) files (sorted by title; a stem found in both
// folders is listed once, from /.scenes/). Cheap enough to call when Settings opens.
void refreshScenes();
// The SD scenes found by the last refreshScenes().
const std::vector<Scene>& sdScenes();

// City Scape setting value meaning "a different scene each sleep" ('*' cannot be a FAT file stem).
constexpr const char* kCycleAllScenes = "*";
// The scene after `last` in picker order (built-in, then sdScenes()), wrapping round; a `last` that
// is no longer listed starts again at the built-in one. The result stays valid until refreshScenes().
const char* nextCycleScene(const char* last);
// The scene the City Scapes wallpaper shows at this sleep: the City Scape setting, or under Cycle All
// the scene after the one shown last time (rescans the card and saves the choice to APP_STATE).
const char* cityScapeSceneForSleep();

// The build has the animation and the setting selects one.
bool isSelected();

// Should the sleep screen be the animation right now (selected and on USB power)?
bool shouldShow();

// One decoded frame: 480x800 portrait, 1 bit per pixel, row-major, MSB first, 1 = white, plus one
// guard byte so a corrupt stream cannot overrun it.
constexpr uint32_t kFrameBufferBytes = 480 * 800 / 8 + 1;

// The frame of `scene` ("" = built-in) for `percent` (5% steps). chargeBars adds the plaque's
// four-stop charge underline (the charging animation); without it the plaque is just the name (Life
// Views). Decodes into `pixels` (kFrameBufferBytes). An SD scene that cannot be read falls back to the
// built-in frame (logged). Returns false (logged) only if no frame could be decoded.
bool decodeFrame(uint8_t* pixels, uint16_t percent, bool chargeBars, const char* scene = "");
// CRC-32 the generator recorded for decodeFrame's output, following the same fallback (smoke test).
uint32_t expectedFrameCrc(uint16_t percent, bool chargeBars, const char* scene = "");
// Draws decodeFrame's output full-screen in portrait into the renderer's buffer. Does not refresh
// the panel. Returns false (logged) if no frame could be decoded.
bool drawFrame(GfxRenderer& renderer, uint16_t percent, bool chargeBars, const char* scene = "");

// RTC-retained state across deep sleep: the animation is on screen, which step and which scene.
void markShown(uint16_t percent, const char* scene);
const char* shownScene();
void clear();
bool isOnScreen();
// The charge only climbs on the charger, so only a higher step is redrawn. A gauge reading that dips
// back across a step boundary (or a failed read reporting 0%) never flashes the panel backwards.
bool stepAdvancedPastShown(uint16_t percent);

// Adds the periodic timer wake. Call just before deep sleep while the animation is on screen.
void armTimerWake();

}  // namespace ChargingAnimation
