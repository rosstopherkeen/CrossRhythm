#include "GardenGrowth.h"

#include <Memory.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

#include "GardenFruitSprites.h"

namespace garden {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kPitch = 13.0f;          // lattice spacing between leaf sites
constexpr float kRowStep = kPitch * 0.87f;
constexpr float kNeighbourDist = kPitch * 1.6f;
// The net is a perforated sheet: 1 px holes on one triangular punch grid of pitch kHolePitch, open
// only where the asanoha lines run. Its triangles are kNetCells grid steps a side; with kNetCells a
// multiple of 3 the triangle centres are grid points too, so every edge (holes kHolePitch apart)
// and every spoke (kHolePitch * sqrt 3 apart) lands exactly on grid holes and lines meeting at a
// star share holes instead of crowding.
constexpr float kHolePitch = 3.0f;
constexpr int kNetCells = 12;
constexpr int kEdgeStride = 2;  // edges open every 2nd grid hole, about the spokes' spacing
constexpr int kHoleSize = 1;    // holes are kHoleSize x kHoleSize px
static_assert(kNetCells % 3 == 0, "triangle centres must be punch grid points");
static_assert(kNetCells % kEdgeStride == 0, "edge holes must meet at the vertices");
constexpr int kNetSideFade = 10;  // holes thin out over this many px at the column's sides
constexpr int kMaxFruit = 8;

constexpr uint8_t kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

// splitmix64: small, fast and well distributed; seeded per purpose so each stream is stable.
class Rng {
 public:
  explicit Rng(uint64_t seed) : state_(seed) {}
  uint64_t next() {
    uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }
  float unit() { return static_cast<float>(next() >> 40) / static_cast<float>(1ULL << 24); }  // [0, 1)
  float uniform(float a, float b) { return a + (b - a) * unit(); }
  int range(int lo, int hi) { return lo + static_cast<int>(next() % static_cast<uint64_t>(hi - lo + 1)); }

 private:
  uint64_t state_;
};

void put(Canvas& c, const int x, const int y, const bool black) {
  if (x < c.clipX0 || x >= c.clipX1 || y < c.clipY0 || y >= c.clipY1) return;
  c.setPixel(c.ctx, x, y, black);
}

void line(Canvas& c, int x0, int y0, const int x1, const int y1, const int width) {
  const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  const int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  const bool steep = -dy > dx;  // thicken across the run direction
  int err = dx + dy;
  for (;;) {
    for (int k = 0; k < width; ++k) {
      const int o = k - (width - 1) / 2;
      if (steep) {
        put(c, x0 + o, y0, true);
      } else {
        put(c, x0, y0 + o, true);
      }
    }
    if (x0 == x1 && y0 == y1) break;
    const int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

int roundi(const float v) { return static_cast<int>(std::lround(v)); }

// Heart/ovate leaf: base at (cx, cy), tip along ang, slight notch at the base (10 points).
constexpr float kHeart[10][2] = {{0, 0},      {.12f, .16f},  {.38f, .30f},  {.62f, .26f},  {.85f, .12f},
                                 {1.0f, 0},   {.85f, -.12f}, {.62f, -.26f}, {.38f, -.30f}, {.12f, -.16f}};

struct Leaf {
  float x, y, ang, size, key;
  bool lit;
};

void heartPoints(const Leaf& l, float* px, float* py) {
  const float c = std::cos(l.ang), s = std::sin(l.ang);
  for (int i = 0; i < 10; ++i) {
    const float u = kHeart[i][0] * l.size, v = kHeart[i][1] * l.size;
    px[i] = l.x + u * c - v * s;
    py[i] = l.y + u * s + v * c;
  }
}

// Fills pixels whose centres fall inside the polygon: white, or 50% Bayer for a shade leaf.
void fillPolygon(Canvas& c, const float* px, const float* py, const int n, const bool shade) {
  float ymin = py[0], ymax = py[0];
  for (int i = 1; i < n; ++i) {
    ymin = std::min(ymin, py[i]);
    ymax = std::max(ymax, py[i]);
  }
  float xs[16];
  for (int y = static_cast<int>(std::floor(ymin)); y <= static_cast<int>(std::ceil(ymax)); ++y) {
    const float yc = y + 0.5f;
    int k = 0;
    for (int i = 0, j = n - 1; i < n; j = i++) {
      if ((py[i] > yc) != (py[j] > yc) && k < 16) {
        xs[k++] = px[i] + (yc - py[i]) * (px[j] - px[i]) / (py[j] - py[i]);
      }
    }
    std::sort(xs, xs + k);
    for (int i = 0; i + 1 < k; i += 2) {
      const int xEnd = static_cast<int>(std::floor(xs[i + 1] - 0.5f));
      for (int x = static_cast<int>(std::ceil(xs[i] - 0.5f)); x <= xEnd; ++x) {
        // tone 125 of 255 against the 4x4 Bayer: black where the threshold index is 8 or more
        put(c, x, y, shade && kBayer[y & 3][x & 3] >= 8);
      }
    }
  }
}

void drawLeaf(Canvas& c, const Leaf& l) {
  const float ca = std::cos(l.ang), sa = std::sin(l.ang);
  line(c, roundi(l.x - 2 * ca), roundi(l.y - 2 * sa), roundi(l.x), roundi(l.y), 1);  // stalk
  float px[10], py[10];
  heartPoints(l, px, py);
  fillPolygon(c, px, py, 10, !l.lit);
  for (int i = 0, j = 9; i < 10; j = i++) line(c, roundi(px[j]), roundi(py[j]), roundi(px[i]), roundi(py[i]), 1);
  if (l.lit) line(c, roundi(l.x), roundi(l.y), roundi(l.x + l.size * .6f * ca), roundi(l.y + l.size * .6f * sa), 1);
}

bool bit(const uint8_t* plane, const int w, const int x, const int y) {
  return (plane[y * ((w + 7) / 8) + x / 8] >> (7 - (x % 8))) & 1;
}

// A sprite with a 1-px paper halo, so fruit reads against the leaves.
void stamp(Canvas& c, const GardenFruit::Sprite& s, const int X, const int Y) {
  const int ox = X - s.ax, oy = Y - s.ay;
  for (int y = 0; y < s.height; ++y) {
    for (int x = 0; x < s.width; ++x) {
      if (!bit(s.mask, s.width, x, y)) continue;
      put(c, ox + x + 1, oy + y, false);
      put(c, ox + x - 1, oy + y, false);
      put(c, ox + x, oy + y + 1, false);
      put(c, ox + x, oy + y - 1, false);
    }
  }
  for (int y = 0; y < s.height; ++y) {
    for (int x = 0; x < s.width; ++x) {
      if (bit(s.mask, s.width, x, y)) put(c, ox + x, oy + y, bit(s.ink, s.width, x, y));
    }
  }
}

void flower(Canvas& c, const int cx, const int cy) {
  for (int y = -7; y <= 7; ++y) {
    for (int x = -7; x <= 7; ++x) {
      const int dd = x * x + y * y;
      if (dd <= 49) put(c, cx + x, cy + y, !(dd > 34 || dd < 16));
    }
  }
  for (int k = 0; k < 5; ++k) {
    const float a = k * 2 * kPi / 5 - kPi / 2;
    for (int r = 2; r <= 3; ++r) put(c, cx + roundi(r * std::cos(a)), cy + roundi(r * std::sin(a)), true);
  }
  put(c, cx, cy, true);
}

struct NetBox {
  int x0, y0, x1, y1;  // exclusive
};

// The sheet fades out at the column's sides instead of stopping on a hard edge: a hole d px in
// from the nearer side stays open for d of every kNetSideFade positions (a fixed hash of the pixel,
// so the fade is stable and has no visible order).
bool openAtSide(const NetBox& box, const int x, const int y) {
  const int d = std::min(x - box.x0, box.x1 - 1 - x);
  if (d >= kNetSideFade) return true;
  const uint32_t hash = (static_cast<uint32_t>(x) * 73856093u) ^ (static_cast<uint32_t>(y) * 19349663u);
  return static_cast<int>((hash >> 4) % kNetSideFade) < d;
}

// One perforation: a kHoleSize square hole centred on (x, y), kept inside the net's box.
void hole(Canvas& c, const NetBox& box, const float x, const float y) {
  const int x0 = static_cast<int>(std::floor(x - (kHoleSize - 1) * 0.5f));
  const int y0 = static_cast<int>(std::floor(y - (kHoleSize - 1) * 0.5f));
  for (int dy = 0; dy < kHoleSize; ++dy) {
    for (int dx = 0; dx < kHoleSize; ++dx) {
      const int px = x0 + dx, py = y0 + dy;
      if (px >= box.x0 && px < box.x1 && py >= box.y0 && py < box.y1 && openAtSide(box, px, py)) {
        put(c, px, py, true);
      }
    }
  }
}

// Holes along a segment, `steps` grid pitches long, ends included (shared ends land on the same
// grid point, so they punch the same hole). `openNearB` false leaves out the hole next to B: a
// spoke's last hole before a star would sit at the same radius as the edges' first, and twelve
// of them in a ring read as a dark circle.
void perforate(Canvas& c, const NetBox& box, const float ax, const float ay, const float bx, const float by,
               const int steps, const bool openNearB = true) {
  for (int k = 0; k <= steps; ++k) {
    if (!openNearB && k == steps - 1 && steps > 1) continue;
    const float t = static_cast<float>(k) / static_cast<float>(steps);
    hole(c, box, ax + (bx - ax) * t, ay + (by - ay) * t);
  }
}

// Asanoha (hemp leaf): a triangle lattice with every triangle's centre joined to its corners, so
// six-pointed stars meet at each lattice vertex. Each vertex A owns the edges to its right and
// lower neighbours and the two triangles below it (one pointing up, one down), so every line and
// triangle is drawn exactly once.
void drawAsanohaNet(Canvas& c, const Column& col) {
  const NetBox box{col.x, col.y, col.x + col.width, col.y + col.height};
  const float side = kHolePitch * kNetCells;
  const float h = side * 0.8660254f;
  const int edgeSteps = kNetCells / kEdgeStride, spokeSteps = kNetCells / 3;
  const int rows = static_cast<int>(col.height / h) + 1;
  const int cols = static_cast<int>(col.width / side) + 3;
  // Vertical lines (vertices and triangle centres) fall every side / 2. Centre the pattern on the
  // column, shifted so the side edges land as far from a vertical line as the width allows; a
  // line right on an edge frames the column with a dotted rule.
  const float half = side / 2;
  const float centre = col.x + col.width / 2.0f;
  const float edgeOff = std::fmod(col.width / 2.0f, half);  // edge to nearest line, line on centre
  const float shift = std::fabs(edgeOff - half / 2) <= std::fabs(std::fmod(edgeOff + half / 2, half) - half / 2)
                          ? 0.0f
                          : half / 2;
  const float originX = centre + shift - std::ceil((centre + shift - col.x) / side) * side;
  for (int j = 0; j <= rows; ++j) {
    const float ay = col.y + j * h;
    for (int i = -1; i <= cols; ++i) {
      const float ax = originX + i * side + ((j & 1) ? side / 2 : 0.0f);
      const float bx = ax + side;                                     // right neighbour
      const float lx = ax - side / 2, rx = ax + side / 2, ly = ay + h;  // lower neighbours
      perforate(c, box, ax, ay, bx, ay, edgeSteps);
      perforate(c, box, ax, ay, lx, ly, edgeSteps);
      perforate(c, box, ax, ay, rx, ly, edgeSteps);
      // down-pointing triangle A, B, lower-right; up-pointing triangle A, lower-left, lower-right
      const float dcx = rx, dcy = ay + h / 3;
      perforate(c, box, dcx, dcy, ax, ay, spokeSteps, false);
      perforate(c, box, dcx, dcy, bx, ay, spokeSteps, false);
      perforate(c, box, dcx, dcy, rx, ly, spokeSteps, false);
      const float ucx = ax, ucy = ay + 2 * h / 3;
      perforate(c, box, ucx, ucy, ax, ay, spokeSteps, false);
      perforate(c, box, ucx, ucy, lx, ly, spokeSteps, false);
      perforate(c, box, ucx, ucy, rx, ly, spokeSteps, false);
    }
  }
}

}  // namespace

uint32_t seedForPath(const char* path) {
  uint32_t h = 2166136261u;
  for (const char* p = path; p != nullptr && *p != '\0'; ++p) {
    h ^= static_cast<uint8_t>(*p);
    h *= 16777619u;
  }
  return h;
}

bool Plan::build(const Column& column, const uint32_t seed) {
  column_ = column;
  seed_ = seed;
  count_ = 0;
  sites_.reset();
  if (column.width < 2 * kPitch || column.height < 2 * kPitch) return false;

  // Lattice: rows from the bottom, alternate rows offset half a pitch.
  const int rows = static_cast<int>(column.height / kRowStep) + 1;
  int n = 0;
  for (int r = 0; r < rows; ++r) {
    for (float x = column.x + 4 + (r % 2) * kPitch / 2; x <= column.x + column.width - 4; x += kPitch) ++n;
  }
  auto sites = makeUniqueNoThrow<Site[]>(n);
  if (!sites) return false;

  Rng rng(seed);
  int i = 0;
  for (int r = 0; r < rows; ++r) {
    const float y = column.y + column.height - 6 - r * kRowStep;
    for (float x = column.x + 4 + (r % 2) * kPitch / 2; x <= column.x + column.width - 4; x += kPitch) {
      const float jx = rng.uniform(-3, 3);
      const float jy = rng.uniform(-3, 3);
      sites[i].x = x + jx;
      sites[i].y = y + jy;
      ++i;
    }
  }

  // Two roots: the bottom-row sites nearest two seeded points, one in each half.
  const float bottomY = column.y + column.height - 6 - kPitch * 0.6f;
  int roots[2] = {-1, -1};
  const float targets[2] = {column.x + column.width * rng.uniform(.15f, .45f),
                            column.x + column.width * rng.uniform(.55f, .85f)};
  for (int k = 0; k < 2; ++k) {
    float best = std::numeric_limits<float>::max();
    for (int s = 0; s < n; ++s) {
      if (sites[s].y <= bottomY) continue;
      const float d = std::fabs(sites[s].x - targets[k]);
      if (d < best) {
        best = d;
        roots[k] = s;
      }
    }
  }
  if (roots[0] < 0) return false;

  // Neighbour graph (compressed rows): sites closer than 1.6 pitch, cost = length times a random
  // factor, sideways dearer than up, so the front fingers unevenly but climbs.
  auto degree = makeUniqueNoThrow<uint16_t[]>(n + 1);
  if (!degree) return false;
  int edges = 0;
  for (int a = 0; a < n; ++a) {
    for (int b = a + 1; b < n; ++b) {
      if (std::hypot(sites[b].x - sites[a].x, sites[b].y - sites[a].y) < kNeighbourDist) {
        ++degree[a];
        ++degree[b];
        ++edges;
      }
    }
  }
  auto offset = makeUniqueNoThrow<uint16_t[]>(n + 1);
  auto adj = makeUniqueNoThrow<int16_t[]>(2 * edges + 1);
  auto cost = makeUniqueNoThrow<float[]>(2 * edges + 1);
  auto fill = makeUniqueNoThrow<uint16_t[]>(n + 1);
  auto dist = makeUniqueNoThrow<float[]>(n);
  auto done = makeUniqueNoThrow<bool[]>(n);
  if (!offset || !adj || !cost || !fill || !dist || !done) return false;
  for (int a = 0; a < n; ++a) offset[a + 1] = offset[a] + degree[a];
  for (int a = 0; a < n; ++a) fill[a] = offset[a];
  for (int a = 0; a < n; ++a) {
    for (int b = a + 1; b < n; ++b) {
      const float dd = std::hypot(sites[b].x - sites[a].x, sites[b].y - sites[a].y);
      if (dd >= kNeighbourDist) continue;
      const float horiz = std::fabs(sites[b].x - sites[a].x) / std::max(dd, 0.001f);
      const float c = dd * (1 + .9f * horiz) * rng.uniform(.35f, 2.4f);
      adj[fill[a]] = static_cast<int16_t>(b);
      cost[fill[a]++] = c;
      adj[fill[b]] = static_cast<int16_t>(a);
      cost[fill[b]++] = c;
    }
  }

  // Dijkstra from the roots (dense selection: a few hundred sites, no heap needed).
  for (int s = 0; s < n; ++s) dist[s] = std::numeric_limits<float>::infinity();
  for (const int r : roots) {
    if (r >= 0) dist[r] = 0;
  }
  for (int it = 0; it < n; ++it) {
    int u = -1;
    for (int s = 0; s < n; ++s) {
      if (!done[s] && std::isfinite(dist[s]) && (u < 0 || dist[s] < dist[u])) u = s;
    }
    if (u < 0) break;
    done[u] = true;
    for (int e = offset[u]; e < offset[u + 1]; ++e) {
      const int v = adj[e];
      if (dist[u] + cost[e] < dist[v]) {
        dist[v] = dist[u] + cost[e];
        sites[v].parent = static_cast<int16_t>(u);
      }
    }
  }
  float dmax = 0;
  for (int s = 0; s < n; ++s) {
    if (std::isfinite(dist[s])) dmax = std::max(dmax, dist[s]);
  }
  for (int s = 0; s < n; ++s) {
    sites[s].t = (std::isfinite(dist[s]) && dmax > 0) ? dist[s] / dmax : 1.0f;
    if (!std::isfinite(dist[s])) sites[s].parent = -1;
  }

  sites_ = std::move(sites);
  count_ = n;
  return true;
}

bool draw(const Plan& plan, float p, Canvas& canvas, const DrawOptions& options) {
  if (plan.count() == 0 || canvas.setPixel == nullptr) return false;
  p = std::clamp(p, 0.0f, 1.0f);
  const Column& col = plan.column();
  const uint32_t seed = plan.seed();
  const int n = plan.count();

  // stems: the growth tree; older (earlier) stems are thicker
  if (!options.leavesOnly) {
    for (int i = 0; i < n; ++i) {
      const Site& s = plan.site(i);
      if (s.t > p) continue;
      if (s.parent < 0) {
        line(canvas, roundi(s.x), col.y + col.height, roundi(s.x), roundi(s.y), 2);
        continue;
      }
      const Site& from = plan.site(s.parent);
      line(canvas, roundi(from.x), roundi(from.y), roundi(s.x), roundi(s.y), std::max(1, roundi(2.2f - 1.4f * s.t)));
    }
  }

  // leaves: 2-3 per grown site, easing in just behind the growth front; shade first, then lit
  int live = 0;
  for (int i = 0; i < n; ++i) live += plan.site(i).t <= p ? 1 : 0;
  auto leaves = makeUniqueNoThrow<Leaf[]>(live * 3 + 1);
  if (!leaves) return false;
  int count = 0;
  for (int i = 0; i < n; ++i) {
    const Site& s = plan.site(i);
    if (s.t > p) continue;
    Rng lr(static_cast<uint64_t>(seed) * 7919u + static_cast<uint64_t>(i));
    const float age = p < 1 ? std::min(1.0f, (p - s.t) / .12f) : 1.0f;
    const int k = lr.range(2, 3);
    for (int j = 0; j < k; ++j) {
      Leaf& l = leaves[count];
      l.ang = -kPi / 2 + lr.uniform(-1.6f, 1.6f);
      l.size = (12.5f + lr.uniform(0, 4.5f)) * (.45f + .55f * age);
      l.x = s.x + lr.uniform(-3, 3);
      l.y = s.y + lr.uniform(-3, 3);
      l.lit = lr.unit() < .62f;
      l.key = lr.unit();
      if (options.leafFilter == nullptr || options.leafFilter(options.filterCtx, l.x, l.y, l.size)) ++count;
    }
  }
  std::sort(leaves.get(), leaves.get() + count, [](const Leaf& a, const Leaf& b) {
    return a.lit != b.lit ? !a.lit : a.key < b.key;
  });
  for (int i = 0; i < count; ++i) drawLeaf(canvas, leaves[i]);
  if (options.leavesOnly) return true;

  // fruit: a few late sites, flower -> bud -> small -> large
  Rng fr(static_cast<uint64_t>(seed) + 7u);
  int kinds[GardenFruit::kKindCount];
  for (int k = 0; k < GardenFruit::kKindCount; ++k) kinds[k] = k;
  for (int k = GardenFruit::kKindCount - 1; k > 0; --k) std::swap(kinds[k], kinds[fr.range(0, k)]);
  int picked[kMaxFruit];
  int pickedCount = 0;
  const int start = fr.range(0, n - 1);  // walk the sites from a seeded start, stride coprime to n
  int stride = 1 + fr.range(0, std::max(0, n - 2));
  while (n > 1 && std::gcd(stride, n) != 1) stride = stride % (n - 1) + 1;
  for (int step = 0; step < n && pickedCount < kMaxFruit; ++step) {
    const int i = (start + step * stride) % n;
    const Site& s = plan.site(i);
    if (s.y > col.y + col.height - 40 || s.x < col.x + 12 || s.x > col.x + col.width - 12) continue;
    bool clear = true;
    for (int q = 0; q < pickedCount; ++q) {
      const Site& o = plan.site(picked[q]);
      if ((s.x - o.x) * (s.x - o.x) + (s.y - o.y) * (s.y - o.y) <= 32.0f * 32.0f) clear = false;
    }
    if (clear) picked[pickedCount++] = i;
  }
  for (int q = 0; q < pickedCount; ++q) {
    const Site& s = plan.site(picked[q]);
    const float s0 = std::max(s.t + .05f, .5f + fr.uniform(0, .12f));  // drawn for every pick: stable stages
    if (s.t > p) continue;
    const int kind = kinds[static_cast<int>((s.x - col.x) / std::max(1, col.width) * GardenFruit::kKindCount) %
                           GardenFruit::kKindCount];
    const int X = static_cast<int>(s.x), Y = static_cast<int>(s.y);
    if (p >= s0 + .33f) {
      stamp(canvas, GardenFruit::large(kind), X, Y);
    } else if (p >= s0 + .22f) {
      stamp(canvas, GardenFruit::small(kind), X, Y);
    } else if (p >= s0 + .11f) {
      stamp(canvas, GardenFruit::bud(), X, Y);
    } else if (p >= s0) {
      flower(canvas, X, Y);
    }
  }

  // the net: a perforated sheet in front, its holes punched along an asanoha pattern
  drawAsanohaNet(canvas, col);
  return true;
}

}  // namespace garden
