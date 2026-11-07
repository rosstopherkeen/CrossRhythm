#pragma once
// Dashboard home footer geometry: big streak number + two-line label on the left,
// two-line reader-type label + icon on the right. Pure arithmetic so host tests can
// check it; DashboardTheme.cpp does the drawing.
//
// Everything sits on one baseline: the label's second line ("streak", "reader")
// has its baseline there, the number's digits sit on it (round digits overshoot
// by their `below` rows, as in the font), and the icon's bottom ink row is the
// row above it, like flat digits and text.

#include <cstdint>

#include "components/icons/homeFooterArt.h"

namespace HomeFooter {

// Art sits this far inside the content inset on both sides (reference layout).
constexpr int kSideIndent = 10;
constexpr int kNumberLabelGap = 11;
constexpr int kIconLabelGap = 12;
// Baseline distance between the two label lines.
constexpr int kLabelLineGap = 22;
// The footer baseline relative to the old footer centre line.
constexpr int kBaselineBelowCenter = 12;
// The streak is computed from a 730-day history, so it never exceeds 3 digits.
constexpr int kMaxDigits = 3;

struct DigitPlacement {
  const HomeFooterGlyph* glyph;
  int x;  // left edge of the bitmap
  int y;  // top edge of the bitmap
};

struct NumberLayout {
  DigitPlacement digits[kMaxDigits];
  int count;
  int inkLeft;
  int inkRight;  // exclusive
};

// Lays the number out so its first ink column is at inkLeft. Values above 999 are
// clamped; the streak source caps at 730.
inline NumberLayout layoutNumber(uint16_t value, const int inkLeft, const int baselineY) {
  if (value > 999) value = 999;
  uint8_t d[kMaxDigits];
  int count = 0;
  do {
    d[count++] = static_cast<uint8_t>(value % 10);
    value = static_cast<uint16_t>(value / 10);
  } while (value > 0 && count < kMaxDigits);

  NumberLayout out{};
  out.count = count;
  out.inkLeft = inkLeft;
  int pen = inkLeft - HomeFooterDigits[d[count - 1]].xOffset;
  out.inkRight = inkLeft;
  for (int i = 0; i < count; ++i) {
    const HomeFooterGlyph& g = HomeFooterDigits[d[count - 1 - i]];
    const int x = pen + g.xOffset;
    out.digits[i] = {&g, x, baselineY + g.below - g.height};
    if (x + g.width > out.inkRight) out.inkRight = x + g.width;
    pen += g.advance;
  }
  return out;
}

inline int iconLeft(const HomeFooterIcon& icon, const int inkRight) { return inkRight - icon.width; }
inline int iconTop(const HomeFooterIcon& icon, const int baselineY) { return baselineY - icon.height; }

// Splits a label at its first space into two lines ("Morning reader" ->
// "Morning" / "reader"). A label without a space stays on the baseline line.
// line1 is written into buf; line2 points into label.
inline void splitLabel(const char* label, char* buf, const int bufLen, const char*& line1, const char*& line2) {
  line1 = nullptr;
  line2 = label;
  if (label == nullptr || bufLen <= 0) return;
  const char* space = label;
  while (*space != '\0' && *space != ' ') ++space;
  if (*space != ' ') return;
  int n = static_cast<int>(space - label);
  if (n >= bufLen) n = bufLen - 1;
  for (int i = 0; i < n; ++i) buf[i] = label[i];
  buf[n] = '\0';
  line1 = buf;
  line2 = space + 1;
}

}  // namespace HomeFooter
