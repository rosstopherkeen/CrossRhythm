// Garden home growth: invariants over the whole class, not one book.
//
// Every test runs over many seeds (book paths) and several column sizes, so a change to the
// lattice, the growth path, the leaves, the fruit or the clip that breaks "full at 100%", "growth
// only adds", "stays inside its clip" or "same book, same plant" fails here.
//
// GARDEN_DUMP_DIR=/some/dir writes the X4 Pro column at a few progress points as PGM images for
// a visual check against the home-studies W mockup.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "components/themes/garden/GardenFruitSprites.h"
#include "components/themes/garden/GardenGrowth.h"

namespace {

constexpr int kW = 480;
constexpr int kH = 800;

// The X4 Pro Garden column (W layout) plus a narrower and a wider one.
const garden::Column kColumns[] = {
    {300, 70, 160, 384},
    {320, 90, 120, 300},
    {260, 60, 220, 420},
};

struct Frame {
  std::vector<uint8_t> px = std::vector<uint8_t>(kW * kH, 255);   // 0 = black
  std::vector<uint8_t> white = std::vector<uint8_t>(kW * kH, 0);  // 1 = a white write landed here
  int outside = 0;

  static void set(void* ctx, int x, int y, bool black) {
    auto* f = static_cast<Frame*>(ctx);
    if (x < 0 || y < 0 || x >= kW || y >= kH) {
      ++f->outside;
      return;
    }
    f->px[y * kW + x] = black ? 0 : 255;
    if (!black) f->white[y * kW + x] = 1;
  }
};

garden::Canvas canvasFor(Frame& f, const garden::Column& c) {
  garden::Canvas cv;
  cv.ctx = &f;
  cv.setPixel = &Frame::set;
  cv.clipX0 = c.x - 12;
  cv.clipY0 = c.y - 12;
  cv.clipX1 = std::min(kW - 6, c.x + c.width + 14);
  cv.clipY1 = c.y + c.height + 2;
  return cv;
}

std::vector<std::string> bookPaths() {
  std::vector<std::string> v;
  for (int i = 0; i < 60; ++i) v.push_back("/Books/Library/book-" + std::to_string(i) + ".epub");
  v.push_back("/The Robots of Dawn.epub");
  v.push_back("");
  return v;
}

// Share of the column's 4x4 blocks that leaves have reached (a white write is a leaf fill: shade
// leaves are half white by their Bayer tone, lit leaves all white; the net only ever adds black).
double leafCoverage(const Frame& f, const garden::Column& c) {
  int hit = 0, total = 0;
  for (int by = c.y + 8; by + 4 <= c.y + c.height - 4; by += 4) {
    for (int bx = c.x + 6; bx + 4 <= c.x + c.width - 6; bx += 4) {
      ++total;
      bool any = false;
      for (int y = by; y < by + 4 && !any; ++y) {
        for (int x = bx; x < bx + 4 && !any; ++x) any = f.white[y * kW + x];
      }
      hit += any;
    }
  }
  return static_cast<double>(hit) / total;
}

TEST(GardenGrowth, SeedIsStableFnv1a) {
  EXPECT_EQ(garden::seedForPath(""), 2166136261u);
  EXPECT_EQ(garden::seedForPath("a"), 0xE40C292Cu);
  EXPECT_EQ(garden::seedForPath(nullptr), 2166136261u);
  EXPECT_NE(garden::seedForPath("/a.epub"), garden::seedForPath("/b.epub"));
}

TEST(GardenGrowth, EveryPlanIsOneTreeRootedOnTheGround) {
  for (const auto& col : kColumns) {
    for (const auto& path : bookPaths()) {
      garden::Plan plan;
      ASSERT_TRUE(plan.build(col, garden::seedForPath(path.c_str())));
      ASSERT_GT(plan.count(), 50);
      int roots = 0;
      for (int i = 0; i < plan.count(); ++i) {
        const garden::Site& s = plan.site(i);
        ASSERT_GE(s.t, 0.0f);
        ASSERT_LE(s.t, 1.0f);
        ASSERT_GE(s.x, col.x - 1.0f);
        ASSERT_LE(s.x, col.x + col.width + 1.0f);
        ASSERT_GE(s.y, col.y - 14.0f);  // the top lattice row may sit a row-step above, as in the mockup
        ASSERT_LE(s.y, col.y + col.height);
        if (s.parent < 0) {
          ++roots;
          EXPECT_EQ(s.t, 0.0f) << "only roots start without a stem";
          EXPECT_GT(s.y, col.y + col.height - 20.0f) << "roots sit on the ground";
          continue;
        }
        // walking up the stems always reaches a root, and every stem is older than its tip
        int at = i, steps = 0;
        while (plan.site(at).parent >= 0 && steps <= plan.count()) {
          EXPECT_LT(plan.site(plan.site(at).parent).t, plan.site(at).t);
          at = plan.site(at).parent;
          ++steps;
        }
        ASSERT_LE(steps, plan.count()) << "stem cycle";
      }
      EXPECT_GE(roots, 1);
      EXPECT_LE(roots, 2);
    }
  }
}

TEST(GardenGrowth, FullAtOneHundredAndBareAtZero) {
  for (const auto& col : kColumns) {
    for (const auto& path : bookPaths()) {
      garden::Plan plan;
      ASSERT_TRUE(plan.build(col, garden::seedForPath(path.c_str())));
      Frame full, bare;
      auto cf = canvasFor(full, col);
      auto cb = canvasFor(bare, col);
      ASSERT_TRUE(garden::draw(plan, 1.0f, cf));
      ASSERT_TRUE(garden::draw(plan, 0.0f, cb));
      EXPECT_GT(leafCoverage(full, col), 0.90) << path << " not full at 100%";
      EXPECT_LT(leafCoverage(bare, col), 0.05) << path << " grown at 0%";
    }
  }
}

TEST(GardenGrowth, GrowthOnlyAdds) {
  for (const auto& col : kColumns) {
    for (const auto& path : bookPaths()) {
      garden::Plan plan;
      ASSERT_TRUE(plan.build(col, garden::seedForPath(path.c_str())));
      double last = 0;
      for (int pct = 0; pct <= 100; pct += 5) {
        Frame f;
        auto c = canvasFor(f, col);
        ASSERT_TRUE(garden::draw(plan, pct / 100.0f, c));
        const double cov = leafCoverage(f, col);
        EXPECT_GE(cov, last - 0.01) << path << " shrank at " << pct << "%";
        last = std::max(last, cov);
      }
    }
  }
}

TEST(GardenGrowth, NeverDrawsOutsideItsClip) {
  for (const auto& col : kColumns) {
    for (const auto& path : bookPaths()) {
      garden::Plan plan;
      ASSERT_TRUE(plan.build(col, garden::seedForPath(path.c_str())));
      for (int pct : {0, 9, 55, 72, 100}) {
        Frame f;
        auto c = canvasFor(f, col);
        ASSERT_TRUE(garden::draw(plan, pct / 100.0f, c));
        EXPECT_EQ(f.outside, 0);
        for (int y = 0; y < kH; ++y) {
          for (int x = 0; x < kW; ++x) {
            const bool inClip = x >= c.clipX0 && x < c.clipX1 && y >= c.clipY0 && y < c.clipY1;
            if (!inClip) {
              ASSERT_EQ(f.px[y * kW + x], 255) << "ink at " << x << "," << y;
              ASSERT_EQ(f.white[y * kW + x], 0);
            }
          }
        }
      }
    }
  }
}

TEST(GardenGrowth, SameBookSamePlantOtherBookOtherPath) {
  const auto& col = kColumns[0];
  garden::Plan a1, a2, b;
  ASSERT_TRUE(a1.build(col, garden::seedForPath("/a.epub")));
  ASSERT_TRUE(a2.build(col, garden::seedForPath("/a.epub")));
  ASSERT_TRUE(b.build(col, garden::seedForPath("/b.epub")));
  Frame f1, f2, f3;
  auto c1 = canvasFor(f1, col), c2 = canvasFor(f2, col), c3 = canvasFor(f3, col);
  ASSERT_TRUE(garden::draw(a1, .6f, c1));
  ASSERT_TRUE(garden::draw(a2, .6f, c2));
  ASSERT_TRUE(garden::draw(b, .6f, c3));
  EXPECT_EQ(f1.px, f2.px);
  int differ = 0;
  for (size_t i = 0; i < f1.px.size(); ++i) differ += f1.px[i] != f3.px[i];
  EXPECT_GT(differ, 500) << "two books grew the same way";
}

struct Box {
  float x0, y0, x1, y1;
};

TEST(GardenGrowth, FrontLeavesOnlyComeFromTheFilteredBand) {
  const auto& col = kColumns[0];
  garden::Plan plan;
  ASSERT_TRUE(plan.build(col, garden::seedForPath("/front.epub")));

  garden::DrawOptions none;
  none.leavesOnly = true;
  none.leafFilter = [](void*, float, float, float) { return false; };
  Frame empty;
  auto ce = canvasFor(empty, col);
  ASSERT_TRUE(garden::draw(plan, 1.0f, ce, none));
  for (size_t i = 0; i < empty.px.size(); ++i) {
    ASSERT_EQ(empty.px[i], 255);
    ASSERT_EQ(empty.white[i], 0);
  }

  Box band{310, 80, 450, 95};
  garden::DrawOptions some;
  some.leavesOnly = true;
  some.filterCtx = &band;
  some.leafFilter = [](void* ctx, float x, float y, float) {
    const Box& b = *static_cast<Box*>(ctx);
    return x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1;
  };
  Frame front;
  auto cf = canvasFor(front, col);
  ASSERT_TRUE(garden::draw(plan, 1.0f, cf, some));
  int touched = 0;
  for (int y = 0; y < kH; ++y) {
    for (int x = 0; x < kW; ++x) {
      if (front.px[y * kW + x] == 255 && !front.white[y * kW + x]) continue;
      ++touched;
      // a leaf is at most ~17 px long from its base
      ASSERT_GE(x, band.x0 - 20);
      ASSERT_LE(x, band.x1 + 20);
      ASSERT_GE(y, band.y0 - 20);
      ASSERT_LE(y, band.y1 + 20);
    }
  }
  EXPECT_GT(touched, 0);
}

TEST(GardenFruit, SpritesAreInkInsideMaskWithAnchorsInside) {
  ASSERT_EQ(GardenFruit::kSpriteCount, 2 * GardenFruit::kKindCount + 1);
  for (int i = 0; i < GardenFruit::kSpriteCount; ++i) {
    const auto& s = GardenFruit::kSprites[i];
    ASSERT_GT(s.width, 0);
    ASSERT_GT(s.height, 0);
    EXPECT_GE(s.ax, 0);
    EXPECT_LT(s.ax, s.width);
    EXPECT_GE(s.ay, 0);
    EXPECT_LT(s.ay, s.height);
    const int stride = (s.width + 7) / 8;
    int ink = 0, inside = 0;
    for (int y = 0; y < s.height; ++y) {
      for (int x = 0; x < s.width; ++x) {
        const bool k = (s.ink[y * stride + x / 8] >> (7 - x % 8)) & 1;
        const bool m = (s.mask[y * stride + x / 8] >> (7 - x % 8)) & 1;
        if (k) EXPECT_TRUE(m) << "sprite " << i << " ink outside its mask";
        ink += k;
        inside += m;
      }
    }
    EXPECT_GT(ink, 0);
    EXPECT_GT(inside, ink) << "sprite " << i << " has no paper (highlight)";
  }
}

// The net is a perforated sheet: at 0% (nothing grown above the ground) every ink pixel in the
// column's upper part is a hole of the net, which stays inside [x - 2, x + w + 2) x [y, y + h),
// is made of separate holes (no ink run longer than 5 px in any row or column) and
// covers the whole column (every 40 x 40 window has some).
TEST(GardenGrowth, NetIsPerforatedInsideTheColumnAndCoversIt) {
  for (const auto& col : kColumns) {
    garden::Plan plan;
    ASSERT_TRUE(plan.build(col, garden::seedForPath("/The Robots of Dawn.epub")));
    Frame f;
    auto c = canvasFor(f, col);
    ASSERT_TRUE(garden::draw(plan, 0.0f, c));
    const int upperBottom = col.y + col.height / 2;  // the bare plant stays near the ground
    auto black = [&](int x, int y) { return f.px[y * kW + x] == 0; };
    for (int y = c.clipY0; y < upperBottom; ++y) {
      for (int x = c.clipX0; x < c.clipX1; ++x) {
        if (!black(x, y)) continue;
        ASSERT_TRUE(x >= col.x - 2 && x < col.x + col.width + 2 && y >= col.y) << "net ink outside at " << x << "," << y;
      }
    }
    for (int y = col.y; y < upperBottom; ++y) {
      int run = 0;
      for (int x = col.x - 2; x < col.x + col.width + 2; ++x) {
        run = black(x, y) ? run + 1 : 0;
        ASSERT_LE(run, 5) << "solid net line in row " << y;
      }
    }
    for (int x = col.x - 2; x < col.x + col.width + 2; ++x) {
      int run = 0;
      for (int y = col.y; y < upperBottom; ++y) {
        run = black(x, y) ? run + 1 : 0;
        ASSERT_LE(run, 5) << "solid net line in column " << x;
      }
    }
    // No dotted rule at the sides: the outer 10 px on each side carry less ink per pixel column
    // than the interior does, so the sheet fades out instead of being framed.
    auto inkInColumns = [&](int x0, int x1) {
      int ink = 0;
      for (int x = x0; x < x1; ++x) {
        for (int y = col.y; y < upperBottom; ++y) ink += black(x, y);
      }
      return static_cast<double>(ink) / (x1 - x0);
    };
    const double interior = inkInColumns(col.x + 10, col.x + col.width - 10);
    EXPECT_LT(inkInColumns(col.x - 2, col.x + 10), interior) << "left side framed";
    EXPECT_LT(inkInColumns(col.x + col.width - 10, col.x + col.width + 2), interior) << "right side framed";
    for (int wy = col.y; wy + 40 <= upperBottom; wy += 20) {
      for (int wx = col.x; wx + 40 <= col.x + col.width; wx += 20) {
        int ink = 0;
        for (int y = wy; y < wy + 40; ++y) {
          for (int x = wx; x < wx + 40; ++x) ink += black(x, y);
        }
        EXPECT_GT(ink, 0) << "net gap at " << wx << "," << wy;
      }
    }
  }
}

TEST(GardenGrowth, DumpForVisualCheck) {
  const char* dir = std::getenv("GARDEN_DUMP_DIR");
  if (dir == nullptr) GTEST_SKIP() << "set GARDEN_DUMP_DIR to write PGM images";
  const auto& col = kColumns[0];
  garden::Plan plan;
  ASSERT_TRUE(plan.build(col, garden::seedForPath("/The Robots of Dawn.epub")));
  for (int pct : {9, 60, 72, 84, 92, 100}) {
    Frame f;
    auto c = canvasFor(f, col);
    ASSERT_TRUE(garden::draw(plan, pct / 100.0f, c));
    const std::string path = std::string(dir) + "/garden-" + std::to_string(pct) + ".pgm";
    FILE* out = std::fopen(path.c_str(), "wb");
    ASSERT_NE(out, nullptr);
    std::fprintf(out, "P5\n%d %d\n255\n", kW, kH);
    std::fwrite(f.px.data(), 1, f.px.size(), out);
    std::fclose(out);
  }
}

}  // namespace
