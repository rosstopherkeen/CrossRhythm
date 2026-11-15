// Dashboard home footer: generated art and layout invariants.
//
// These cover the whole class, not one case: every digit, every icon, every
// streak value the firmware can show, so a regenerated asset or a layout tweak
// that breaks bottom alignment, sizing or packing fails here.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "components/themes/dashboard/HomeFooterLayout.h"

namespace {

constexpr int kBaseline = 755;  // X4 Pro footer baseline (800 px tall, touch, no hints)

// GfxRenderer::drawIcon layout: stored row r is logical column width-1-r, bits run
// down the column MSB first, 1 = white.
bool black(const uint8_t* bits, int w, int h, int lx, int ly) {
  const int stride = (h + 7) / 8;
  const uint8_t byte = bits[(w - 1 - lx) * stride + ly / 8];
  return ((byte >> (7 - (ly % 8))) & 1) == 0;
}

void expectTightAndPadded(const uint8_t* bits, int w, int h, const char* what) {
  bool top = false, bottom = false, left = false, right = false;
  for (int x = 0; x < w; ++x) {
    top |= black(bits, w, h, x, 0);
    bottom |= black(bits, w, h, x, h - 1);
  }
  for (int y = 0; y < h; ++y) {
    left |= black(bits, w, h, 0, y);
    right |= black(bits, w, h, w - 1, y);
  }
  EXPECT_TRUE(top && bottom && left && right) << what << " is not cropped to its ink";
  const int stride = (h + 7) / 8;
  for (int r = 0; r < w; ++r) {
    for (int y = h; y < stride * 8; ++y) {
      const uint8_t byte = bits[r * stride + y / 8];
      EXPECT_EQ((byte >> (7 - (y % 8))) & 1, 1) << what << " has ink in row padding";
    }
  }
}

const HomeFooterIcon* const kIcons[] = {&HomeFooterDawnIcon, &HomeFooterDayIcon, &HomeFooterDuskIcon,
                                        &HomeFooterTwilightIcon, &HomeFooterNewReaderIcon};

struct IconPair {
  const HomeFooterIcon* plate;  // source art: black plate with the picture cut out
  const HomeFooterIcon* lit;    // the picture alone as ink; what the footer draws
  const char* name;
};
const IconPair kIconPairs[] = {{&HomeFooterDawnIcon, &HomeFooterDawnLitIcon, "dawn"},
                               {&HomeFooterDayIcon, &HomeFooterDayLitIcon, "day"},
                               {&HomeFooterDuskIcon, &HomeFooterDuskLitIcon, "dusk"},
                               {&HomeFooterTwilightIcon, &HomeFooterTwilightLitIcon, "twilight"},
                               {&HomeFooterNewReaderIcon, &HomeFooterNewReaderLitIcon, "new reader"}};

// Share of the lit icon's ink that lands on the plate icon's paper (or outside it)
// when both are right-aligned and the lit icon is dy rows below the plate's top.
double litInkOnPlatePaper(const IconPair& p, const int dy) {
  int ink = 0, onPaper = 0;
  const int dx = p.plate->width - p.lit->width;
  for (int y = 0; y < p.lit->height; ++y) {
    for (int x = 0; x < p.lit->width; ++x) {
      if (!black(p.lit->bits, p.lit->width, p.lit->height, x, y)) continue;
      ++ink;
      const int px = x + dx, py = y + dy;
      const bool inside = px >= 0 && px < p.plate->width && py >= 0 && py < p.plate->height;
      if (!inside || !black(p.plate->bits, p.plate->width, p.plate->height, px, py)) ++onPaper;
    }
  }
  return ink == 0 ? 0.0 : static_cast<double>(onPaper) / ink;
}

int digitCount(int v) { return v >= 100 ? 3 : v >= 10 ? 2 : 1; }

}  // namespace

TEST(HomeFooterArt, EveryBitmapIsTightlyCroppedInDrawIconLayout) {
  for (int d = 0; d < 10; ++d) {
    const HomeFooterGlyph& g = HomeFooterDigits[d];
    expectTightAndPadded(g.bits, g.width, g.height, "digit");
  }
  for (const HomeFooterIcon* icon : kIcons) expectTightAndPadded(icon->bits, icon->width, icon->height, "icon");
}

TEST(HomeFooterArt, LitIconsAreTightlyCroppedAndFitInsideTheirPlateIcon) {
  for (const IconPair& p : kIconPairs) {
    expectTightAndPadded(p.lit->bits, p.lit->width, p.lit->height, p.name);
    EXPECT_LE(p.lit->width, p.plate->width) << p.name;
    EXPECT_LE(p.lit->height, p.plate->height) << p.name;
    // Same right-edge and baseline anchoring as the plate icons.
    EXPECT_EQ(HomeFooter::iconLeft(*p.lit, 460) + p.lit->width, 460) << p.name;
    EXPECT_EQ(HomeFooter::iconTop(*p.lit, kBaseline) + p.lit->height, kBaseline) << p.name;
  }
}

TEST(HomeFooterArt, LitIconsLightThePictureNotThePlate) {
  // The footer shows the pictured shapes (black on paper, white in Night Mode), never the
  // plate. So the lit ink has to be the plate icon's paper: at the best vertical alignment
  // nearly all of it lands off the plate's ink (the rest is the thin overhang outline).
  for (const IconPair& p : kIconPairs) {
    double best = 0.0;
    for (int dy = 0; dy <= p.plate->height - p.lit->height; ++dy) best = std::max(best, litInkOnPlatePaper(p, dy));
    EXPECT_GE(best, 0.85) << p.name << " lit icon overlaps the plate's ink";
  }
}

TEST(HomeFooterArt, DigitsShareOneHeightOnTheBaseline) {
  // Lining figures: flat digits end on the baseline row above, round ones overshoot by 1.
  for (int d = 0; d < 10; ++d) {
    const HomeFooterGlyph& g = HomeFooterDigits[d];
    EXPECT_NEAR(g.height - g.below, 42, 1) << "digit " << d;
    EXPECT_GE(g.below, 0);
    EXPECT_LE(g.below, 1);
    EXPECT_GT(g.advance, g.xOffset) << "digit " << d;
  }
}

TEST(HomeFooterArt, IconsHaveTheSameOpticalSize) {
  // Icons are sized by sqrt(w*h), so a square sun reads as big as the tall cup.
  const double target = 42 * 1.25;
  for (const HomeFooterIcon* icon : kIcons) {
    EXPECT_NEAR(std::sqrt(icon->width * icon->height), target, target * 0.05);
  }
}

TEST(HomeFooterLayout, EveryStreakValueSitsOnTheBaseline) {
  for (int v = 0; v <= 999; ++v) {
    const HomeFooter::NumberLayout n = HomeFooter::layoutNumber(static_cast<uint16_t>(v), 30, kBaseline);
    ASSERT_EQ(n.count, digitCount(v)) << v;
    EXPECT_EQ(n.digits[0].x, 30) << v;
    int lastRight = 0;
    for (int i = 0; i < n.count; ++i) {
      const HomeFooter::DigitPlacement& p = n.digits[i];
      EXPECT_EQ(p.y + p.glyph->height - p.glyph->below, kBaseline) << v;
      if (i > 0) EXPECT_GE(p.x, lastRight) << "digits overlap in " << v;
      lastRight = p.x + p.glyph->width;
      EXPECT_LE(lastRight, n.inkRight);
    }
  }
}

TEST(HomeFooterLayout, WidestStreakLeavesRoomForItsLabel) {
  // X4 Pro: 480 px wide, content inset 20, so the number starts at 20 (the cover edge) and the
  // left group must stay inside the left half with room for "streak".
  int widest = 0;
  for (int v = 0; v <= 730; ++v) {
    const HomeFooter::NumberLayout n = HomeFooter::layoutNumber(static_cast<uint16_t>(v), 20, kBaseline);
    if (n.inkRight > widest) widest = n.inkRight;
  }
  EXPECT_LE(widest + HomeFooter::kNumberLabelGap + 60, 240);
}

TEST(HomeFooterLayout, ArtStartsAndEndsExactlyOnTheGivenEdges) {
  // The caller passes the cover's x and the stats column's right edge; the art's ink
  // must land on them exactly, for every value and every icon.
  for (int v = 0; v <= 999; ++v) {
    const HomeFooter::NumberLayout n = HomeFooter::layoutNumber(static_cast<uint16_t>(v), 20, kBaseline);
    EXPECT_EQ(n.digits[0].x, 20) << v;
  }
  for (const HomeFooterIcon* icon : kIcons) EXPECT_EQ(HomeFooter::iconLeft(*icon, 460) + icon->width, 460);
}

TEST(HomeFooterLayout, IconBottomSitsOnTheBaseline) {
  for (const HomeFooterIcon* icon : kIcons) {
    EXPECT_EQ(HomeFooter::iconTop(*icon, kBaseline) + icon->height, kBaseline);
    EXPECT_EQ(HomeFooter::iconLeft(*icon, 460) + icon->width, 460);
  }
}

TEST(HomeFooterLayout, LabelsSplitAtTheFirstSpace) {
  char buf[48];
  const char* l1;
  const char* l2;
  HomeFooter::splitLabel("Morning reader", buf, sizeof(buf), l1, l2);
  EXPECT_STREQ(l1, "Morning");
  EXPECT_STREQ(l2, "reader");
  HomeFooter::splitLabel("day streak", buf, sizeof(buf), l1, l2);
  EXPECT_STREQ(l1, "day");
  EXPECT_STREQ(l2, "streak");
  HomeFooter::splitLabel("Lecteur du matin", buf, sizeof(buf), l1, l2);
  EXPECT_STREQ(l1, "Lecteur");
  EXPECT_STREQ(l2, "du matin");
  HomeFooter::splitLabel("NoSpace", buf, sizeof(buf), l1, l2);
  EXPECT_EQ(l1, nullptr);
  EXPECT_STREQ(l2, "NoSpace");
  char tiny[4];
  HomeFooter::splitLabel("Afternoon reader", tiny, sizeof(tiny), l1, l2);
  EXPECT_STREQ(l1, "Aft");
  EXPECT_STREQ(l2, "reader");
}
