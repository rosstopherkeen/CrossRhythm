// IndicShapeTest — the on-device Indic shaper against HarfBuzz.
//
// IndicShape substitutes Indic clusters for the shaped forms the font actually
// draws (conjuncts, below-base consonants, merged vowel signs, repha). Those
// forms are unencoded GSUB output, so the build maps them to private-use
// codepoints and derives lookup tables from HarfBuzz offline into a .cpshape
// file that ships in the font's folder on the SD card.
//
// The generator verifies its own tables in Python, which proves the *model* is
// right but not that this C++ reproduces it. These tests close that gap by
// loading the committed .cpshape files -- the same bytes the font packs carry --
// through the device parser and replaying HarfBuzz output recorded in
// generated/<Script>Golden.h: every cluster the factored tables could not
// reproduce (the exception table), plus a sample of the rest. Regenerate a
// script's .cpshape and its golden set from the same font together -- a
// mismatched pair is exactly what this test is here to catch.
//
// The parser is also fed damaged files, because on the device these come off
// an SD card a reader can write anything to.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "IndicShape.h"
#include "IndicTables.h"
#include "Utf8.h"
#include "generated/BengaliGolden.h"
#include "generated/DevanagariGolden.h"
#include "generated/KannadaGolden.h"
#include "generated/MalayalamGolden.h"
#include "generated/OriyaGolden.h"
#include "generated/TamilGolden.h"
#include "generated/TeluguGolden.h"

namespace {

std::string encode(const std::vector<uint32_t>& codepoints) {
  std::string utf8;
  for (const uint32_t cp : codepoints) utf8AppendCodepoint(cp, utf8);
  return utf8;
}

std::vector<uint32_t> decode(const std::string& utf8) {
  std::vector<uint32_t> codepoints;
  auto* p = reinterpret_cast<const unsigned char*>(utf8.c_str());
  while (*p) {
    const uint32_t cp = utf8NextCodepoint(&p);
    if (!cp) break;
    codepoints.push_back(cp);
  }
  return codepoints;
}

/// Shape `logical`, falling back to the input when the shaper reports that
/// nothing changed — the same contract GfxRenderer::resolveVisualText() uses.
std::vector<uint32_t> shape(const std::vector<uint32_t>& logical) {
  const std::string input = encode(logical);
  std::string shaped;
  if (!IndicShape::applyShaping(input.c_str(), shaped) || shaped.empty()) {
    return decode(input);
  }
  return decode(shaped);
}

std::string describe(const std::vector<uint32_t>& codepoints) {
  std::string out;
  char buffer[8];
  for (size_t i = 0; i < codepoints.size(); i++) {
    std::snprintf(buffer, sizeof(buffer), "%04X", codepoints[i]);
    if (i) out += ' ';
    out += buffer;
  }
  return out;
}

/// One script's recorded HarfBuzz output, as a flat record stream, and the
/// .cpshape file it checks.
struct GoldenSet {
  const char* name;
  const char* file;
  const uint16_t* records;
  size_t recordCount;
  int caseCount;
};

#define GOLDEN_SET(label, file, ns) \
  {label, file, ns::RECORDS, sizeof(ns::RECORDS) / sizeof(ns::RECORDS[0]), ns::CASE_COUNT}

const GoldenSet GOLDEN_SETS[] = {
    GOLDEN_SET("Devanagari", "devanagari", devanagariGolden),
    GOLDEN_SET("Bengali", "bengali", bengaliGolden),
    GOLDEN_SET("Oriya", "oriya", oriyaGolden),
    GOLDEN_SET("Tamil", "tamil", tamilGolden),
    GOLDEN_SET("Telugu", "telugu", teluguGolden),
    GOLDEN_SET("Kannada", "kannada", kannadaGolden),
    GOLDEN_SET("Malayalam", "malayalam", malayalamGolden),
};

#undef GOLDEN_SET

/// A .cpshape file held the way the device holds one: in a 4-byte aligned
/// buffer that outlives its registration. uint32_t storage gives the alignment.
struct LoadedFile {
  std::vector<uint32_t> words;
  size_t size = 0;
  const uint8_t* data() const { return reinterpret_cast<const uint8_t*>(words.data()); }
  uint8_t* data() { return reinterpret_cast<uint8_t*>(words.data()); }
};

LoadedFile readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  LoadedFile file;
  file.size = bytes.size();
  file.words.resize((bytes.size() + 3) / 4);
  if (!bytes.empty()) std::memcpy(file.data(), bytes.data(), bytes.size());
  return file;
}

std::string shapingPath(const char* script) { return std::string(INDIC_SHAPING_DIR) + "/" + script + ".cpshape"; }

/// The seven committed files, read once for the whole run.
std::vector<LoadedFile>& sidecars() {
  static std::vector<LoadedFile> files = [] {
    std::vector<LoadedFile> loaded;
    for (const GoldenSet& set : GOLDEN_SETS) loaded.push_back(readFile(shapingPath(set.file)));
    return loaded;
  }();
  return files;
}

/// Register every committed script, as the font system does with every
/// installed Indic font. Tests that change the registration call this again.
void registerAllScripts() {
  IndicShape::clearScripts();
  for (LoadedFile& file : sidecars()) {
    ASSERT_GT(file.size, 0u) << "missing .cpshape; run gen_indic_shaping.py";
    ASSERT_TRUE(IndicShape::registerScript(file.data(), file.size));
  }
}

}  // namespace

// Every recorded HarfBuzz result must come back byte for byte. This is the
// test that matters: it is the only check that the C++ tables, the key packing
// and the cluster parser all agree with the shaper the tables came from.
TEST(IndicShapeTest, MatchesHarfBuzzGoldenSet) {
  registerAllScripts();
  for (const GoldenSet& set : GOLDEN_SETS) {
    SCOPED_TRACE(set.name);
    int checked = 0;
    int mismatches = 0;
    std::string firstFailure;

    size_t cursor = 0;
    while (cursor < set.recordCount) {
      const uint16_t logicalCount = set.records[cursor++];
      ASSERT_LE(cursor + logicalCount, set.recordCount) << "truncated golden record";
      std::vector<uint32_t> logical(set.records + cursor, set.records + cursor + logicalCount);
      cursor += logicalCount;

      const uint16_t expectedCount = set.records[cursor++];
      ASSERT_LE(cursor + expectedCount, set.recordCount) << "truncated golden record";
      std::vector<uint32_t> expected(set.records + cursor, set.records + cursor + expectedCount);
      cursor += expectedCount;

      const std::vector<uint32_t> actual = shape(logical);
      checked++;
      if (actual != expected) {
        mismatches++;
        if (firstFailure.empty()) {
          firstFailure = "logical [" + describe(logical) + "] expected [" + describe(expected) + "] got [" +
                         describe(actual) + "]";
        }
      }
    }

    EXPECT_EQ(checked, set.caseCount);
    EXPECT_EQ(mismatches, 0) << mismatches << " of " << checked
                             << " clusters disagree with HarfBuzz. First: " << firstFailure;
  }
}

// Text in no supported Indic script must be left strictly alone, and cheaply:
// the prefilter is what keeps shaping off the hot path for every other script.
TEST(IndicShapeTest, IgnoresUnsupportedText) {
  registerAllScripts();
  EXPECT_FALSE(IndicShape::mayContainIndic("Hello, world!"));
  EXPECT_FALSE(IndicShape::mayContainIndic(""));
  EXPECT_FALSE(IndicShape::mayContainIndic(nullptr));
  // Gurmukhi and Gujarati sit between Bengali and Malayalam, so the byte-range
  // prefilter cannot exclude them and mayContainIndic() says maybe. What must
  // hold is the contract that matters: no registered script claims them, so the
  // shaper leaves the text exactly as it was.
  std::string untouched;
  EXPECT_FALSE(IndicShape::applyShaping("ਪੰਜਾਬੀ", untouched));
  EXPECT_FALSE(IndicShape::applyShaping("ગુજરાતી", untouched));

  EXPECT_TRUE(IndicShape::mayContainIndic("हिन्दी"));
  EXPECT_TRUE(IndicShape::mayContainIndic("বাংলা"));
  EXPECT_TRUE(IndicShape::mayContainIndic("ଓଡ଼ିଆ"));
  EXPECT_TRUE(IndicShape::mayContainIndic("தமிழ்"));
  EXPECT_TRUE(IndicShape::mayContainIndic("తెలుగు"));
  EXPECT_TRUE(IndicShape::mayContainIndic("ಕನ್ನಡ"));
  EXPECT_TRUE(IndicShape::mayContainIndic("മലയാളം"));

  std::string out;
  EXPECT_FALSE(IndicShape::applyShaping("Hello, world!", out));
}

// A bare independent vowel already renders as itself, so the shaper should
// report no change and let the caller keep the original string.
TEST(IndicShapeTest, LeavesUnshapedTextUnchanged) {
  registerAllScripts();
  std::string out;
  EXPECT_FALSE(IndicShape::applyShaping("ಅ", out));  // U+0C85 Kannada A
  EXPECT_FALSE(IndicShape::applyShaping("అ", out));  // U+0C05 Telugu A
  EXPECT_FALSE(IndicShape::applyShaping("அ", out));  // U+0B85 Tamil A
  EXPECT_FALSE(IndicShape::applyShaping("अ", out));  // U+0905 Devanagari A
  EXPECT_FALSE(IndicShape::applyShaping("অ", out));  // U+0985 Bengali A
  EXPECT_FALSE(IndicShape::applyShaping("ଅ", out));  // U+0B05 Oriya A
  EXPECT_FALSE(IndicShape::applyShaping("അ", out));  // U+0D05 Malayalam A
}

// Mixed runs must shape only the Indic text and pass everything else through in
// order — reader text is full of Latin punctuation and digits.
TEST(IndicShapeTest, PreservesSurroundingText) {
  registerAllScripts();
  struct Case {
    const char* word;
    uint32_t interiorVirama;  ///< 0 when the word has no virama to swallow
  };
  // Each word must actually shape, or applyShaping() rightly reports no change.
  // Tamil's "கொடி" earns its place through the two-part matra U+0BCA, which
  // renders as a pre-base part before the consonant and a post-base part after.
  const Case cases[] = {
      {"ಕನ್ನಡ", 0x0CCDu}, {"తెలుగు", 0x0C4Du}, {"கொடி", 0u},     {"हिन्दी", 0x094Du},
      {"ক্ষমা", 0x09CDu}, {"ଓଡ଼ିଆ", 0u},        {"അമ്മ", 0x0D4Du},
  };

  for (const Case& test : cases) {
    SCOPED_TRACE(test.word);
    const std::string input = std::string("A ") + test.word + " 42.";
    std::string shaped;
    ASSERT_TRUE(IndicShape::applyShaping(input.c_str(), shaped));

    const std::vector<uint32_t> actual = decode(shaped);
    ASSERT_GE(actual.size(), 7u);
    EXPECT_EQ(actual[0], static_cast<uint32_t>('A'));
    EXPECT_EQ(actual[1], static_cast<uint32_t>(' '));
    EXPECT_EQ(actual[actual.size() - 4], static_cast<uint32_t>(' '));
    EXPECT_EQ(actual[actual.size() - 3], static_cast<uint32_t>('4'));
    EXPECT_EQ(actual[actual.size() - 2], static_cast<uint32_t>('2'));
    EXPECT_EQ(actual[actual.size() - 1], static_cast<uint32_t>('.'));

    // A virama inside a cluster must be consumed by the conjunct, never drawn.
    if (test.interiorVirama != 0u) {
      for (const uint32_t cp : actual) {
        EXPECT_NE(cp, test.interiorVirama) << "virama survived shaping";
      }
    }
  }
}

// Shaping must not leak across a script boundary inside one string: each run
// has to be parsed with its own script's tables.
TEST(IndicShapeTest, ShapesAdjacentScriptsIndependently) {
  registerAllScripts();
  const std::string input = std::string("ಕನ್ನಡ") + " " + "తెలుగు" + " " + "हिन्दी" + " " + "മലയാളം";
  std::string shaped;
  ASSERT_TRUE(IndicShape::applyShaping(input.c_str(), shaped));

  const std::vector<uint32_t> actual = decode(shaped);
  // The three spaces must survive, so the four runs stay four runs.
  int spaces = 0;
  for (const uint32_t cp : actual) {
    if (cp == static_cast<uint32_t>(' ')) spaces++;
  }
  EXPECT_EQ(spaces, 3);
  // No interior virama from any of these scripts may reach the output.
  for (const uint32_t cp : actual) {
    EXPECT_NE(cp, 0x0CCDu) << "Kannada virama survived shaping";
    EXPECT_NE(cp, 0x0C4Du) << "Telugu virama survived shaping";
    EXPECT_NE(cp, 0x094Du) << "Devanagari virama survived shaping";
    EXPECT_NE(cp, 0x0D4Du) << "Malayalam virama survived shaping";
  }
}

// A malformed run (stray or trailing viramas) must not drop characters or walk
// off the cluster buffer; EPUB text in the wild is not always well formed.
TEST(IndicShapeTest, HandlesMalformedClusters) {
  registerAllScripts();
  struct Case {
    const char* consonant;
    const char* virama;
  };
  const Case cases[] = {
      {"ಕ", "್"}, {"క", "్"}, {"க", "்"}, {"क", "्"}, {"ক", "্"}, {"କ", "୍"}, {"ക", "്"},
  };

  for (const Case& test : cases) {
    SCOPED_TRACE(test.consonant);
    std::string shaped;
    // Trailing virama with no consonant after it.
    const std::string trailing = std::string(test.consonant) + test.virama;
    IndicShape::applyShaping(trailing.c_str(), shaped);
    EXPECT_FALSE(decode(shaped.empty() ? trailing : shaped).empty());

    // A long virama run must terminate and keep the leading consonant.
    std::string viramas = test.consonant;
    for (int i = 0; i < 40; i++) viramas += test.virama;
    IndicShape::applyShaping(viramas.c_str(), shaped);
    EXPECT_FALSE(decode(shaped.empty() ? viramas : shaped).empty());
  }
}

// Every committed file parses and registers, one per script.
// Kannada, Tamil and Telugu draw a word-final consonant + virama as one glyph
// (kaviramaknda, kaprehalftamil, kahalanttelu), not the consonant plus a drawn
// virama mark, so every such pair must collapse to a single shaped codepoint --
// including before spaces, punctuation and Latin text, which is how a name like
// "ಕುಮಾರ್" ends a line.
TEST(IndicShapeTest, FusesWordFinalConsonantVirama) {
  registerAllScripts();
  struct FinalViramaScript {
    const char* name;
    std::vector<uint32_t> consonants;
    uint32_t virama;
  };
  std::vector<uint32_t> kannada, tamil{0x0B95, 0x0B99, 0x0B9A, 0x0B9C, 0x0B9E, 0x0B9F, 0x0BA3, 0x0BA4,
                                       0x0BA8, 0x0BA9, 0x0BAA, 0x0BAE, 0x0BAF, 0x0BB0, 0x0BB1, 0x0BB2,
                                       0x0BB3, 0x0BB4, 0x0BB5, 0x0BB6, 0x0BB7, 0x0BB8, 0x0BB9},
      telugu{0x0C58, 0x0C59, 0x0C5A};
  for (uint32_t cp = 0x0C95; cp <= 0x0CB9; cp++) {
    if (cp != 0x0CA9 && cp != 0x0CB4) kannada.push_back(cp);
  }
  for (uint32_t cp = 0x0C15; cp <= 0x0C39; cp++) {
    if (cp != 0x0C29) telugu.push_back(cp);
  }
  const FinalViramaScript scripts[] = {{"Kannada", kannada, 0x0CCD}, {"Tamil", tamil, 0x0BCD}, {"Telugu", telugu, 0x0C4D}};

  for (const FinalViramaScript& script : scripts) {
    SCOPED_TRACE(script.name);
    for (const uint32_t consonant : script.consonants) {
      for (const char* tail : {"", " ", ".", "x"}) {
        const std::string input = encode({consonant, script.virama}) + tail;
        std::string out;
        ASSERT_TRUE(IndicShape::applyShaping(input.c_str(), out)) << std::hex << consonant;
        const std::vector<uint32_t> shaped = decode(out);
        ASSERT_EQ(shaped.size(), 1 + decode(tail).size()) << std::hex << consonant << " [" << describe(shaped) << "]";
        EXPECT_GE(shaped[0], 0xE000u) << std::hex << consonant;
        EXPECT_LE(shaped[0], 0xF8FFu) << std::hex << consonant;
      }
    }
  }

  // The reported case: ra + virama closing a name, after a vowel sign.
  const std::vector<uint32_t> kumar = shape({0x0C95, 0x0CC1, 0x0CAE, 0x0CBE, 0x0CB0, 0x0CCD});
  ASSERT_FALSE(kumar.empty());
  EXPECT_NE(kumar.back(), 0x0CCDu);
  EXPECT_GE(kumar.back(), 0xE000u);
}

// Books spell Hindi's nukta letters decomposed (Unicode keeps them out of NFC),
// but the font only draws the precomposed letter, so a consonant + nukta must
// shape exactly as the precomposed letter does -- alone, with a vowel sign, and
// inside a conjunct.
TEST(IndicShapeTest, FoldsDecomposedNuktaIntoThePrecomposedLetter) {
  registerAllScripts();
  constexpr uint32_t NUKTA = 0x093C;
  constexpr uint32_t VIRAMA = 0x094D;
  const struct {
    uint32_t base, composed;
  } pairs[] = {{0x0915, 0x0958}, {0x0916, 0x0959}, {0x0917, 0x095A}, {0x091C, 0x095B},
               {0x0921, 0x095C}, {0x0922, 0x095D}, {0x092B, 0x095E}, {0x092F, 0x095F},
               {0x0928, 0x0929}, {0x0930, 0x0931}, {0x0933, 0x0934}};
  for (const auto& pair : pairs) {
    SCOPED_TRACE(pair.base);
    EXPECT_EQ(shape({pair.base, NUKTA}), shape({pair.composed}));
    for (const uint32_t matra : {0x093Eu, 0x093Fu, 0x0940u, 0x0941u, 0x0947u, 0x094Bu}) {
      EXPECT_EQ(shape({pair.base, NUKTA, matra}), shape({pair.composed, matra}));
    }
    EXPECT_EQ(shape({pair.base, NUKTA, VIRAMA, 0x0924}), shape({pair.composed, VIRAMA, 0x0924}));
    EXPECT_EQ(shape({0x0924, VIRAMA, pair.base, NUKTA}), shape({0x0924, VIRAMA, pair.composed}));
  }

  // A nukta with no precomposed letter stays as written.
  EXPECT_EQ(shape({0x0924, NUKTA}), (std::vector<uint32_t>{0x0924, NUKTA}));
}

TEST(IndicShapeTest, RegistersEveryShippedScript) {
  registerAllScripts();
  EXPECT_EQ(IndicShape::scriptCount(), sizeof(GOLDEN_SETS) / sizeof(GOLDEN_SETS[0]));
  for (size_t i = 0; i < IndicShape::scriptCount(); i++) {
    EXPECT_NE(IndicShape::probeCodepoint(i), 0u);
  }
}

// With no font installed nothing is shaped, and the prefilter says so without
// reading the text.
TEST(IndicShapeTest, ShapesNothingWithoutAFont) {
  IndicShape::clearScripts();
  EXPECT_FALSE(IndicShape::mayContainIndic("हिन्दी"));
  std::string out;
  EXPECT_FALSE(IndicShape::applyShaping("ಕನ್ನಡ", out));
  registerAllScripts();
}

// A second font for a script already registered must not displace the first:
// the lookup order would otherwise decide which tables draw the text.
TEST(IndicShapeTest, RejectsADuplicateScript) {
  registerAllScripts();
  LoadedFile& kannada = sidecars()[5];
  EXPECT_FALSE(IndicShape::registerScript(kannada.data(), kannada.size));
}

// The file comes off an SD card, so every way it can be damaged must be refused
// at load time rather than read out of bounds while drawing.
TEST(IndicShapeTest, RejectsDamagedFiles) {
  const LoadedFile& good = sidecars()[5];  // Kannada, the smallest model
  IndicTables::Script script;
  ASSERT_TRUE(IndicTables::parse(good.data(), good.size, script));

  auto damaged = [&](const auto& mutate) {
    LoadedFile copy = good;
    mutate(copy);
    IndicTables::Script out;
    return !IndicTables::parse(copy.data(), copy.size, out);
  };

  EXPECT_TRUE(damaged([](LoadedFile& f) { f.size -= 1; })) << "truncated";
  EXPECT_TRUE(damaged([](LoadedFile& f) { f.size = 10; })) << "shorter than the header";
  EXPECT_TRUE(damaged([](LoadedFile& f) { f.data()[0] = 'X'; })) << "bad magic";
  EXPECT_TRUE(damaged([](LoadedFile& f) { f.data()[4] = 99; })) << "unknown version";
  // baseCount far beyond what the file holds.
  EXPECT_TRUE(damaged([](LoadedFile& f) { f.data()[36] = 0xFF, f.data()[37] = 0xFF; })) << "section overrun";
  // Block outside the 0xE0 lead-byte range the prefilter understands.
  EXPECT_TRUE(damaged([](LoadedFile& f) { f.data()[13] = 0x30; })) << "block out of range";
  // First base entry's run pointing past its pool.
  EXPECT_TRUE(damaged([&](LoadedFile& f) {
    const size_t baseOffset = reinterpret_cast<const uint8_t*>(script.base) - good.data();
    f.data()[baseOffset + 2] = 0xFF, f.data()[baseOffset + 3] = 0xFF;
  })) << "base run overruns its pool";
  // Two base keys out of order breaks the binary search.
  EXPECT_TRUE(damaged([&](LoadedFile& f) {
    const size_t baseOffset = reinterpret_cast<const uint8_t*>(script.base) - good.data();
    std::swap(f.data()[baseOffset], f.data()[baseOffset + 6]);
    std::swap(f.data()[baseOffset + 1], f.data()[baseOffset + 7]);
  })) << "unsorted keys";

  // A buffer the device did not align is refused rather than read misaligned.
  std::vector<uint8_t> shifted(good.size + 1);
  std::memcpy(shifted.data() + 1, good.data(), good.size);
  EXPECT_FALSE(IndicTables::parse(shifted.data() + 1, good.size, script));
}
