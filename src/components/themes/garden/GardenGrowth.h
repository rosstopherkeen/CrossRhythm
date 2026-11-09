#pragma once

// Garden home: a climber that grows up a column as a book is read.
//
// The finished plant is fixed: a jittered lattice of leaf sites covers the whole column, so 100%
// is always full. Only the path the growth takes is random per book: a seeded shortest-path spread
// from two roots on the column's bottom edge, with random edge costs and climbing cheaper than
// spreading sideways, sets the progress at which each site grows (0..1). At progress p every site
// reached by p has grown, its leaves easing in just behind the front; the path tree is drawn as the
// stems. Leaves are heart-shaped on short stalks, shade leaves in 50% Bayer and lit leaves white
// with a black edge and midrib, in painter's order. From about 55% a few sites fruit (flower, bud,
// small, large). In front sits a light perforated sheet: 1 px holes on a fine punch grid, open
// along an asanoha (hemp leaf) pattern.
//
// Pure C++ (no Arduino or renderer types) so it runs in host tests. It draws through Canvas, one
// pixel at a time, straight into the caller's framebuffer, so no column-sized buffer is needed.
// Working memory is a few small arrays sized by the site count (~450 sites for the X4 Pro column),
// allocated without throwing; a failed allocation makes build()/draw() return false.

#include <cstdint>
#include <memory>

namespace garden {

struct Column {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

// 1-bit drawing target. Pixels outside [clipX0, clipX1) x [clipY0, clipY1) are never set.
struct Canvas {
  void* ctx = nullptr;
  void (*setPixel)(void* ctx, int x, int y, bool black) = nullptr;
  int clipX0 = 0;
  int clipY0 = 0;
  int clipX1 = 0;
  int clipY1 = 0;
};

struct Site {
  float x = 0.0f;
  float y = 0.0f;
  float t = 1.0f;         // progress at which the site grows, 0..1
  int16_t parent = -1;    // stem comes from this site; -1 = a root (stem from the ground)
};

class Plan {
 public:
  // Lays out the sites for the column and runs the seeded growth. False if memory ran out or the
  // column is too small to hold a plant.
  bool build(const Column& column, uint32_t seed);

  int count() const { return count_; }
  const Site& site(int i) const { return sites_[i]; }
  const Column& column() const { return column_; }
  uint32_t seed() const { return seed_; }

 private:
  std::unique_ptr<Site[]> sites_;
  int count_ = 0;
  Column column_{};
  uint32_t seed_ = 0;
};

struct DrawOptions {
  // Draw only leaves (no stems, fruit or net), and only those the filter accepts. Used to redraw
  // the leaves that come in front of the progress line.
  bool leavesOnly = false;
  bool (*leafFilter)(void* ctx, float x, float y, float size) = nullptr;
  void* filterCtx = nullptr;
};

// Draws the plant at progress p (0..1). False if memory ran out (nothing partial is left unsafe:
// whatever was drawn before the failure stays, the caller just logs it).
bool draw(const Plan& plan, float progress, Canvas& canvas, const DrawOptions& options = {});

// Stable per-book seed: FNV-1a of the book path, so a book always grows the same way.
uint32_t seedForPath(const char* path);

}  // namespace garden
