#include "ChargingAnimation.h"

#include "AppCapabilities.h"
#include "SceneFileName.h"

#if CROSSINK_APP_CAP_CHARGING_ANIMATION

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <InflateReader.h>
#include <Logging.h>
#include <Memory.h>
#ifndef SIMULATOR
#include <esp_attr.h>
#include <esp_sleep.h>
#else
// The simulator has no deep sleep: state lives for the process and the timer wake is a no-op.
#define RTC_DATA_ATTR
#endif

#include <strings.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "ChargingFramesBuiltin.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"

namespace {

namespace frames = charging_frames_builtin;

// RTC slow memory survives deep sleep; the magic tells a valid record from power-on garbage.
constexpr uint32_t kRtcMagic = 0xC4A6E5A2;
RTC_DATA_ATTR uint32_t rtcMagic = 0;
RTC_DATA_ATTR uint8_t rtcShownStep = 0;
RTC_DATA_ATTR char rtcScene[ChargingAnimation::kSceneIdMax] = "";

constexpr int kRowBytes = frames::kWidth / 8;
static_assert(ChargingAnimation::kFrameBufferBytes == frames::kFrameBytes + 1,
              "Frame buffer size must match the generated frames");

// .rhythmscene files (layout in docs/file-formats.md). Every scene uses the built-in
// frame layout, so the step arithmetic is shared.
constexpr char kSceneMagic[4] = {'C', 'P', 'S', 'C'};
constexpr uint16_t kSceneVersion = 1;
constexpr uint16_t kSceneHeaderBytes = 128;
constexpr uint32_t kSceneTableBytes = (3 * frames::kFrameCount + 1) * 4;
constexpr uint32_t kSceneDataStart = kSceneHeaderBytes + kSceneTableBytes;
// Largest compressed frame accepted. Real frames are 30-45 KB; a bigger length means a corrupt table.
constexpr uint32_t kMaxCompressedFrame = 128 * 1024;
const char* const kSceneDirs[] = {"/.scenes", "/scenes"};
constexpr size_t kMaxScenes = 16;

std::vector<ChargingAnimation::Scene> scenes;

// Plaque layout in pixels: where the four-stop underline goes over the stored plain plaque.
struct Plaque {
  int16_t x, y, width, barsHeight, textBottom, shadow, border, barX, barY, barHeight, barSegment, barGap;
};

constexpr Plaque kBuiltinPlaque = {frames::kPlaqueX,
                                   frames::kPlaqueY,
                                   frames::kPlaqueWidth,
                                   frames::kPlaqueBarsHeight,
                                   frames::kPlaqueTextBottom,
                                   frames::kPlaqueShadow,
                                   frames::kPlaqueBorder,
                                   frames::kBarX,
                                   frames::kBarY,
                                   frames::kBarHeight,
                                   frames::kBarSegment,
                                   frames::kBarGap};

uint8_t stepForPercent(const uint16_t percent) {
  const uint16_t clamped = std::min<uint16_t>(percent, 100);
  return static_cast<uint8_t>(clamped / frames::kStepPercent);
}

uint16_t readU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
int16_t readI16(const uint8_t* p) { return static_cast<int16_t>(readU16(p)); }
uint32_t readU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

// Reads and checks a scene file's header. Fills the plaque layout and the title when given.
bool readSceneHeader(HalFile& file, const char* path, Plaque* plaque, char* title) {
  uint8_t h[kSceneHeaderBytes];
  if (!file.seek(0) || file.read(h, sizeof(h)) != static_cast<int>(sizeof(h))) {
    LOG_ERR("CHG", "Scene %s: header unreadable", path);
    return false;
  }
  if (memcmp(h, kSceneMagic, sizeof(kSceneMagic)) != 0 || readU16(h + 4) != kSceneVersion ||
      readU16(h + 6) != kSceneHeaderBytes) {
    LOG_ERR("CHG", "Scene %s: not a version %u scene file", path, static_cast<unsigned>(kSceneVersion));
    return false;
  }
  if (readU16(h + 8) != frames::kWidth || readU16(h + 10) != frames::kHeight || h[12] != frames::kStepPercent ||
      h[13] != frames::kFrameCount) {
    LOG_ERR("CHG", "Scene %s: unsupported layout %ux%u, %u%% steps, %u frames", path,
            static_cast<unsigned>(readU16(h + 8)), static_cast<unsigned>(readU16(h + 10)), static_cast<unsigned>(h[12]),
            static_cast<unsigned>(h[13]));
    return false;
  }
  if (file.size() < kSceneDataStart) {
    LOG_ERR("CHG", "Scene %s: truncated", path);
    return false;
  }
  if (plaque) {
    int16_t g[12];
    for (int k = 0; k < 12; ++k) g[k] = readI16(h + 14 + k * 2);
    *plaque = {g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7], g[8], g[9], g[10], g[11]};
  }
  if (title) {
    memcpy(title, h + 38, ChargingAnimation::kSceneTitleMax);
    title[ChargingAnimation::kSceneTitleMax - 1] = '\0';
  }
  return true;
}

// Opens /.scenes/<id>.rhythmscene, else /scenes/<id>.rhythmscene; a legacy .cpscene of the same id is
// tried in each folder after the current extension.
bool openScene(const char* id, HalFile& file, char* path, const size_t pathSize) {
  for (const char* dir : kSceneDirs) {
    for (const char* ext : {SceneFileName::kExtension, SceneFileName::kLegacyExtension}) {
      snprintf(path, pathSize, "%s/%s%s", dir, id, ext);
      if (Storage.exists(path) && Storage.openFileForRead("CHG", path, file)) return true;
    }
  }
  LOG_ERR("CHG", "Scene %s not found in /.scenes or /scenes", id);
  return false;
}

// Fills x1..x2, y1..y2 (inclusive, clipped) in a frame buffer: 1 bit per pixel, MSB first, 1 = white.
void fillFrameRect(uint8_t* pixels, const int x1, const int y1, const int x2, const int y2, const bool black) {
  for (int y = std::max(0, y1); y <= std::min(frames::kHeight - 1, y2); ++y) {
    uint8_t* row = pixels + y * kRowBytes;
    for (int x = std::max(0, x1); x <= std::min(frames::kWidth - 1, x2); ++x) {
      const uint8_t mask = 0x80 >> (x % 8);
      if (black) {
        row[x / 8] &= static_cast<uint8_t>(~mask);
      } else {
        row[x / 8] |= mask;
      }
    }
  }
}

// Grows the stored plain plaque by the four-stop underline (one stop per 25%), exactly as the scene
// draws it (scene files are checked against this when they are generated). Only rows below the
// title change: the taller shadow and border, the paper behind the stops, then the stops.
void drawChargeBars(uint8_t* pixels, const uint16_t percent, const Plaque& g) {
  const int right = g.x + g.width;
  const int bottom = g.y + g.barsHeight;
  fillFrameRect(pixels, g.x + g.shadow, g.textBottom, right + g.shadow, bottom + g.shadow, true);
  fillFrameRect(pixels, g.x, g.textBottom, right, bottom, true);
  fillFrameRect(pixels, g.x + g.border, g.textBottom, right - g.border, bottom - g.border, false);
  const int filled = std::min<int>(percent, 100) / 25;
  for (int k = 0; k < 4; ++k) {
    const int sx = g.barX + k * (g.barSegment + g.barGap);
    const int ex = sx + g.barSegment - 1;
    const int ey = g.barY + g.barHeight - 1;
    fillFrameRect(pixels, sx, g.barY, ex, ey, true);
    if (k >= filled) fillFrameRect(pixels, sx + 1, g.barY + 1, ex - 1, ey - 1, false);
  }
}

bool decodeBuiltin(uint8_t* pixels, const uint8_t step) {
  InflateReader inflate;
  inflate.init();
  inflate.setSource(frames::kData + frames::kOffsets[step], frames::kOffsets[step + 1] - frames::kOffsets[step]);
  if (!inflate.readExact(pixels, frames::kFrameBytes)) {
    LOG_ERR("CHG", "Built-in frame %u failed to inflate", static_cast<unsigned>(step));
    return false;
  }
  return true;
}

// Reads one frame of an SD scene. The compressed frame is read whole (30-45 KB) because InflateReader
// inflates from memory; the buffer lives only for this call.
bool decodeSdScene(uint8_t* pixels, const uint8_t step, const char* id, Plaque& plaque) {
  char path[64];
  HalFile file;
  if (!openScene(id, file, path, sizeof(path))) return false;
  bool ok = readSceneHeader(file, path, &plaque, nullptr);
  uint8_t span[8];
  uint32_t begin = 0;
  uint32_t end = 0;
  if (ok) {
    ok = file.seek(kSceneHeaderBytes + step * 4) && file.read(span, sizeof(span)) == static_cast<int>(sizeof(span));
    begin = readU32(span);
    end = readU32(span + 4);
    if (ok && (end <= begin || end - begin > kMaxCompressedFrame || kSceneDataStart + end > file.size())) {
      LOG_ERR("CHG", "Scene %s: bad offsets for frame %u", path, static_cast<unsigned>(step));
      ok = false;
    }
  }
  std::unique_ptr<uint8_t[]> packed;
  if (ok) {
    packed = makeUniqueNoThrow<uint8_t[]>(end - begin);
    if (!packed) {
      LOG_ERR("CHG", "Scene %s: no memory for frame %u (%u bytes)", path, static_cast<unsigned>(step),
              static_cast<unsigned>(end - begin));
      ok = false;
    }
  }
  if (ok &&
      (!file.seek(kSceneDataStart + begin) || file.read(packed.get(), end - begin) != static_cast<int>(end - begin))) {
    LOG_ERR("CHG", "Scene %s: frame %u unreadable", path, static_cast<unsigned>(step));
    ok = false;
  }
  file.close();
  if (!ok) return false;
  InflateReader inflate;
  inflate.init();
  inflate.setSource(packed.get(), end - begin);
  if (!inflate.readExact(pixels, frames::kFrameBytes)) {
    LOG_ERR("CHG", "Scene %s: frame %u failed to inflate", path, static_cast<unsigned>(step));
    return false;
  }
  return true;
}

}  // namespace

namespace ChargingAnimation {

const char* builtinTitle() { return frames::kTitle; }

void refreshScenes() {
  scenes.clear();
  for (const char* dir : kSceneDirs) {
    if (!Storage.exists(dir)) continue;
    auto root = Storage.open(dir);
    if (!root || !root.isDirectory()) {
      if (root) root.close();
      continue;
    }
    root.rewindDirectory();
    char name[64];
    for (auto entry = root.openNextFile(); entry; entry = root.openNextFile()) {
      entry.getName(name, sizeof(name));
      const bool isDir = entry.isDirectory();
      entry.close();
      const size_t len = strlen(name);
      const size_t extLen = SceneFileName::extensionLength(name, len);
      if (isDir || strncmp(name, "._", 2) == 0 || extLen == 0 || len - extLen >= static_cast<size_t>(kSceneIdMax)) {
        continue;
      }
      Scene scene{};
      memcpy(scene.id, name, len - extLen);
      scene.id[len - extLen] = '\0';
      if (std::any_of(scenes.begin(), scenes.end(), [&](const Scene& s) { return strcmp(s.id, scene.id) == 0; })) {
        continue;  // the same stem in /.scenes wins
      }
      char path[96];
      if (SceneFileName::isLegacy(name, len)) {
        // openScene() prefers the current extension, so a legacy copy next to one is not a separate scene.
        snprintf(path, sizeof(path), "%s/%s%s", dir, scene.id, SceneFileName::kExtension);
        if (Storage.exists(path)) continue;
      }
      snprintf(path, sizeof(path), "%s/%s", dir, name);
      HalFile file;
      if (!Storage.openFileForRead("CHG", path, file)) continue;
      const bool valid = readSceneHeader(file, path, nullptr, scene.title);
      file.close();
      if (!valid) continue;
      if (scene.title[0] == '\0') strncpy(scene.title, scene.id, sizeof(scene.title) - 1);
      if (scenes.size() >= kMaxScenes) {
        LOG_INF("CHG", "More than %u scenes; ignoring %s", static_cast<unsigned>(kMaxScenes), path);
        continue;
      }
      scenes.push_back(scene);
    }
    root.close();
  }
  std::sort(scenes.begin(), scenes.end(), [](const Scene& a, const Scene& b) { return strcmp(a.title, b.title) < 0; });
  LOG_DBG("CHG", "Found %u SD scenes", static_cast<unsigned>(scenes.size()));
}

const std::vector<Scene>& sdScenes() { return scenes; }

const char* nextCycleScene(const char* last) {
  if (last == nullptr || last[0] == '\0') return scenes.empty() ? "" : scenes.front().id;
  for (size_t i = 0; i < scenes.size(); i++) {
    if (strcmp(scenes[i].id, last) == 0) return i + 1 < scenes.size() ? scenes[i + 1].id : "";
  }
  return "";
}

const char* cityScapeSceneForSleep() {
  if (strcmp(SETTINGS.cityScapeScene, kCycleAllScenes) != 0) return SETTINGS.cityScapeScene;
  refreshScenes();
  const char* scene = nextCycleScene(APP_STATE.cityScapeCycleScene.c_str());
  APP_STATE.cityScapeCycleScene = scene;
  if (!APP_STATE.saveToFile()) LOG_ERR("CHG", "Could not save the City Scape cycle position");
  return scene;
}

bool isSelected() { return SETTINGS.chargingAnimation == CrossPointSettings::CHARGING_ANIMATION_ON; }

bool shouldShow() { return isSelected() && gpio.isUsbConnected(); }

bool decodeFrame(uint8_t* pixels, const uint16_t percent, const bool chargeBars, const char* scene) {
  const uint8_t step = stepForPercent(percent);
  Plaque plaque = kBuiltinPlaque;
  bool ok = false;
  if (scene && scene[0] != '\0') {
    ok = decodeSdScene(pixels, step, scene, plaque);
    if (!ok) {
      LOG_ERR("CHG", "Scene %s unavailable; showing %s", scene, builtinTitle());
      plaque = kBuiltinPlaque;
    }
  }
  if (!ok && !decodeBuiltin(pixels, step)) return false;
  if (chargeBars) drawChargeBars(pixels, percent, plaque);
  return true;
}

uint32_t expectedFrameCrc(const uint16_t percent, const bool chargeBars, const char* scene) {
  const uint8_t step = stepForPercent(percent);
  if (scene && scene[0] != '\0') {
    char path[64];
    HalFile file;
    uint8_t crc[4];
    if (openScene(scene, file, path, sizeof(path))) {
      const uint32_t table =
          kSceneHeaderBytes + (frames::kFrameCount + 1) * 4 + (chargeBars ? frames::kFrameCount * 4 : 0);
      const bool ok = readSceneHeader(file, path, nullptr, nullptr) && file.seek(table + step * 4) &&
                      file.read(crc, sizeof(crc)) == static_cast<int>(sizeof(crc));
      file.close();
      if (ok) return readU32(crc);
    }
  }
  return chargeBars ? frames::kBarsCrc[step] : frames::kPlainCrc[step];
}

bool drawFrame(GfxRenderer& renderer, const uint16_t percent, const bool chargeBars, const char* scene) {
  const uint8_t step = stepForPercent(percent);
  // One frame inflates to 48 KB. It is needed only for this draw, so it lives for this call;
  // the extra byte lets readExact() reject a stream that would overrun the frame.
  auto pixels = makeUniqueNoThrow<uint8_t[]>(kFrameBufferBytes);
  if (!pixels) {
    LOG_ERR("CHG", "No memory for charging frame %u", static_cast<unsigned>(step));
    return false;
  }
  if (!decodeFrame(pixels.get(), percent, chargeBars, scene)) return false;

  const auto savedOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  renderer.clearScreen();
  const int width = std::min(renderer.getScreenWidth(), frames::kWidth);
  const int height = std::min(renderer.getScreenHeight(), frames::kHeight);
  for (int y = 0; y < height; ++y) {
    const uint8_t* row = pixels.get() + y * kRowBytes;
    for (int xb = 0; xb < kRowBytes; ++xb) {
      const uint8_t bits = row[xb];
      if (bits == 0xFF) continue;  // eight white pixels
      for (int bit = 0; bit < 8; ++bit) {
        const int x = xb * 8 + bit;
        if (x < width && (bits & (0x80 >> bit)) == 0) renderer.drawPixel(x, y, true);
      }
    }
  }
  renderer.setOrientation(savedOrientation);
  LOG_DBG("CHG", "Frame %u drawn for %u%%", static_cast<unsigned>(step), static_cast<unsigned>(percent));
  return true;
}

void markShown(const uint16_t percent, const char* scene) {
  rtcShownStep = stepForPercent(percent);
  strncpy(rtcScene, scene ? scene : "", sizeof(rtcScene) - 1);
  rtcScene[sizeof(rtcScene) - 1] = '\0';
  rtcMagic = kRtcMagic;
}

const char* shownScene() { return isOnScreen() ? rtcScene : ""; }

void clear() { rtcMagic = 0; }

bool isOnScreen() { return rtcMagic == kRtcMagic; }

bool stepAdvancedPastShown(const uint16_t percent) { return !isOnScreen() || stepForPercent(percent) > rtcShownStep; }

void armTimerWake() {
#ifndef SIMULATOR
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(kCheckIntervalSeconds) * 1000000ULL);
#endif
}

}  // namespace ChargingAnimation

#else  // !CROSSINK_APP_CAP_CHARGING_ANIMATION

namespace ChargingAnimation {
namespace {
const std::vector<Scene> kNoScenes;
}
const char* builtinTitle() { return ""; }
void refreshScenes() {}
const std::vector<Scene>& sdScenes() { return kNoScenes; }
const char* nextCycleScene(const char*) { return ""; }
const char* cityScapeSceneForSleep() { return ""; }
bool isSelected() { return false; }
bool shouldShow() { return false; }
bool decodeFrame(uint8_t*, uint16_t, bool, const char*) { return false; }
uint32_t expectedFrameCrc(uint16_t, bool, const char*) { return 0; }
bool drawFrame(GfxRenderer&, uint16_t, bool, const char*) { return false; }
void markShown(uint16_t, const char*) {}
const char* shownScene() { return ""; }
void clear() {}
bool isOnScreen() { return false; }
bool stepAdvancedPastShown(uint16_t) { return false; }
void armTimerWake() {}
}  // namespace ChargingAnimation

#endif
