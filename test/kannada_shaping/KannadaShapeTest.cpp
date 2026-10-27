// KannadaShapeTest — the on-device Kannada shaper against HarfBuzz.
//
// KannadaShape substitutes Kannada clusters for the shaped forms the font
// actually draws (conjuncts, below-base consonants, merged vowel signs,
// arkavattu). Those forms are unencoded GSUB output, so the build maps them to
// private-use codepoints and derives lookup tables from HarfBuzz offline.
//
// The generator verifies its own tables in Python, which proves the *model* is
// right but not that this C++ reproduces it. These tests close that gap by
// replaying HarfBuzz output recorded in KannadaGolden.generated.h: every
// cluster the factored tables could not reproduce (the exception table), plus a
// sample of the rest. Regenerate both headers from the same font together --
// a mismatched pair is exactly what this test is here to catch.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "generated/KannadaGolden.h"
#include "KannadaShape.h"
#include "Utf8.h"

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
  if (!KannadaShape::applyShaping(input.c_str(), shaped) || shaped.empty()) {
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

}  // namespace

// Every recorded HarfBuzz result must come back byte for byte. This is the
// test that matters: it is the only check that the C++ tables, the key packing
// and the cluster parser all agree with the shaper the tables came from.
TEST(KannadaShapeTest, MatchesHarfBuzzGoldenSet) {
  int checked = 0;
  int mismatches = 0;
  std::string firstFailure;

  size_t cursor = 0;
  const size_t total = sizeof(kannadaGolden::RECORDS) / sizeof(kannadaGolden::RECORDS[0]);
  while (cursor < total) {
    const uint16_t logicalCount = kannadaGolden::RECORDS[cursor++];
    ASSERT_LE(cursor + logicalCount, total) << "truncated golden record";
    std::vector<uint32_t> logical(kannadaGolden::RECORDS + cursor,
                                  kannadaGolden::RECORDS + cursor + logicalCount);
    cursor += logicalCount;

    const uint16_t expectedCount = kannadaGolden::RECORDS[cursor++];
    ASSERT_LE(cursor + expectedCount, total) << "truncated golden record";
    std::vector<uint32_t> expected(kannadaGolden::RECORDS + cursor,
                                   kannadaGolden::RECORDS + cursor + expectedCount);
    cursor += expectedCount;

    const std::vector<uint32_t> actual = shape(logical);
    checked++;
    if (actual != expected) {
      mismatches++;
      if (firstFailure.empty()) {
        firstFailure = "logical [" + describe(logical) + "] expected [" + describe(expected) +
                       "] got [" + describe(actual) + "]";
      }
    }
  }

  EXPECT_EQ(checked, kannadaGolden::CASE_COUNT);
  EXPECT_EQ(mismatches, 0) << mismatches << " of " << checked << " clusters disagree with HarfBuzz."
                           << " First: " << firstFailure;
}

// Text with no Kannada must be left strictly alone, and cheaply: the prefilter
// is what keeps shaping off the hot path for every other script.
TEST(KannadaShapeTest, IgnoresNonKannadaText) {
  EXPECT_FALSE(KannadaShape::mayContainKannada("Hello, world!"));
  EXPECT_FALSE(KannadaShape::mayContainKannada(""));
  EXPECT_FALSE(KannadaShape::mayContainKannada(nullptr));
  // Devanagari and Telugu sit either side of Kannada and must not false-positive.
  EXPECT_FALSE(KannadaShape::mayContainKannada("हिन्दी"));
  EXPECT_FALSE(KannadaShape::mayContainKannada("తెలుగు"));
  EXPECT_TRUE(KannadaShape::mayContainKannada("ಕನ್ನಡ"));

  std::string out;
  EXPECT_FALSE(KannadaShape::applyShaping("Hello, world!", out));
}

// A bare consonant already renders as itself, so the shaper should report no
// change and let the caller keep the original string instead of copying it.
TEST(KannadaShapeTest, LeavesUnshapedKannadaUnchanged) {
  std::string out;
  // U+0C85 is an independent vowel; it has its own glyph and needs no shaping.
  EXPECT_FALSE(KannadaShape::applyShaping("ಅ", out));
}

// Mixed runs must shape only the Kannada and pass everything else through in
// order — reader text is full of Latin punctuation and digits.
TEST(KannadaShapeTest, PreservesSurroundingText) {
  const std::string input = std::string("A ") + "ಕನ್ನಡ" + " 42.";
  std::string shaped;
  ASSERT_TRUE(KannadaShape::applyShaping(input.c_str(), shaped));

  const std::vector<uint32_t> actual = decode(shaped);
  ASSERT_GE(actual.size(), 7u);
  EXPECT_EQ(actual[0], static_cast<uint32_t>('A'));
  EXPECT_EQ(actual[1], static_cast<uint32_t>(' '));
  EXPECT_EQ(actual[actual.size() - 4], static_cast<uint32_t>(' '));
  EXPECT_EQ(actual[actual.size() - 3], static_cast<uint32_t>('4'));
  EXPECT_EQ(actual[actual.size() - 2], static_cast<uint32_t>('2'));
  EXPECT_EQ(actual[actual.size() - 1], static_cast<uint32_t>('.'));

  // The virama must be consumed by the conjunct, never drawn.
  for (const uint32_t cp : actual) EXPECT_NE(cp, 0x0CCDu) << "virama survived shaping";
}

// A malformed run (stray or trailing viramas) must not drop characters or walk
// off the cluster buffer; EPUB text in the wild is not always well formed.
TEST(KannadaShapeTest, HandlesMalformedClusters) {
  std::string shaped;
  // Trailing virama with no consonant after it.
  const std::string trailing = std::string("ಕ್");
  KannadaShape::applyShaping(trailing.c_str(), shaped);
  EXPECT_FALSE(decode(shaped.empty() ? trailing : shaped).empty());

  // A long virama run must terminate and keep the leading consonant.
  std::string viramas = "ಕ";
  for (int i = 0; i < 40; i++) viramas += "್";
  KannadaShape::applyShaping(viramas.c_str(), shaped);
  const std::vector<uint32_t> actual = decode(shaped.empty() ? viramas : shaped);
  EXPECT_FALSE(actual.empty());
}
