// Every character any UI translation uses must exist in every built-in UI font,
// so swapping or regenerating the UI font can never silently drop a language.
#include <EpdFont.h>
#include <builtinFonts/bricolage_10_bold.h>
#include <builtinFonts/bricolage_10_regular.h>
#include <builtinFonts/bricolage_12_bold.h>
#include <builtinFonts/bricolage_12_regular.h>
#include <builtinFonts/bricolage_8_regular.h>
#include <gtest/gtest.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

namespace {

std::set<uint32_t> decodeUtf8(const std::string& s) {
  std::set<uint32_t> out;
  for (size_t i = 0; i < s.size();) {
    const auto c = static_cast<unsigned char>(s[i]);
    uint32_t cp = c;
    int extra = 0;
    if (c >= 0xF0) cp = c & 0x07, extra = 3;
    else if (c >= 0xE0) cp = c & 0x0F, extra = 2;
    else if (c >= 0xC0) cp = c & 0x1F, extra = 1;
    for (int k = 1; k <= extra && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    i += extra + 1;
    if (cp >= 0x20 && cp != 0x7F) out.insert(cp);
  }
  return out;
}

struct Font {
  const char* name;
  EpdFont font;
};

}  // namespace

TEST(UiFontCoverage, EveryTranslationCharacterIsInEveryUiFont) {
  const Font fonts[] = {{"8 regular", EpdFont(&bricolage_8_regular)},   {"10 regular", EpdFont(&bricolage_10_regular)},
                        {"10 bold", EpdFont(&bricolage_10_bold)},       {"12 regular", EpdFont(&bricolage_12_regular)},
                        {"12 bold", EpdFont(&bricolage_12_bold)}};
  int files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(TRANSLATIONS_DIR)) {
    if (entry.path().extension() != ".yaml") continue;
    ++files;
    std::ifstream in(entry.path());
    std::stringstream text;
    text << in.rdbuf();
    for (const uint32_t cp : decodeUtf8(text.str())) {
      // Arabic UI text is shaped into presentation forms before drawing (checked below).
      if (cp >= 0x0600 && cp <= 0x06FF) continue;
      if (cp == 0x200C || cp == 0x200D || cp == 0x200F || cp == 0x200E) continue;  // joiners and bidi marks
      for (const auto& f : fonts) {
        EXPECT_TRUE(f.font.hasCodepoint(cp)) << entry.path().filename() << ": U+" << std::hex << cp
                                              << " missing from UI font " << f.name;
      }
    }
  }
  EXPECT_EQ(files, 28);
}

TEST(UiFontCoverage, ArabicShapingOutputIsInEveryUiFont) {
  // MiniBidi shapes Arabic UI text to Presentation Forms-B (U+FE80..FEFC).
  const EpdFont fonts[] = {EpdFont(&bricolage_8_regular),  EpdFont(&bricolage_10_regular), EpdFont(&bricolage_10_bold),
                           EpdFont(&bricolage_12_regular), EpdFont(&bricolage_12_bold)};
  for (const auto& font : fonts) {
    for (uint32_t cp = 0xFE80; cp <= 0xFEFC; ++cp) EXPECT_TRUE(font.hasCodepoint(cp)) << "U+" << std::hex << cp;
  }
}

namespace {
std::string readFile(const char* path) {
  std::ifstream in(path);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

// Identifiers like UI_12_FONT_ID / SMALL_FONT_ID that follow `marker` in `text`.
std::set<std::string> uiFontIdsAfter(const std::string& text, const std::string& marker, size_t from = 0,
                                     size_t to = std::string::npos) {
  std::set<std::string> ids;
  for (size_t pos = text.find(marker, from); pos != std::string::npos && pos < to;
       pos = text.find(marker, pos + 1)) {
    size_t start = pos + marker.size();
    while (start < text.size() && text[start] == ' ') ++start;
    size_t end = start;
    while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_')) ++end;
    const std::string id = text.substr(start, end - start);
    if ((id.rfind("UI_", 0) == 0 || id.rfind("SMALL_", 0) == 0) && id.size() > 8 &&
        id.compare(id.size() - 8, 8, "_FONT_ID") == 0) {
      ids.insert(id);
    }
  }
  return ids;
}
}  // namespace

TEST(UiFontCoverage, EveryUiFontSizeGetsTheIndicScriptFallback) {
  // Indic text in the UI (book titles, file names) is drawn by the SD-card script
  // fonts that SdCardFontSystem chains behind each built-in UI size listed in
  // kUiFontSizes. A UI font registered in main.cpp but missing from that list
  // draws Indic text with no fallback, i.e. broken.
  const std::string mainCpp = readFile(SRC_DIR "/main.cpp");
  const std::set<std::string> registered = uiFontIdsAfter(mainCpp, "renderer.insertFont(");
  ASSERT_FALSE(registered.empty());

  const std::string sdfs = readFile(SRC_DIR "/SdCardFontSystem.cpp");
  const size_t begin = sdfs.find("kUiFontSizes[] = {");
  ASSERT_NE(begin, std::string::npos);
  const size_t end = sdfs.find("};", begin);
  const std::set<std::string> withFallback = uiFontIdsAfter(sdfs, "{", begin + 1, end);

  for (const std::string& id : registered) {
    EXPECT_TRUE(withFallback.count(id)) << id << " is a UI font with no Indic script fallback in kUiFontSizes";
  }
}
