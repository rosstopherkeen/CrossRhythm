#pragma once

#include <cstdint>
#include <string>

/// Kannada text shaping.
///
/// Kannada stacks the consonants of a cluster beneath its base letter and
/// merges most vowel signs into the base glyph. Those combined shapes come out
/// of the font's GSUB table and carry no Unicode codepoints, so drawing text
/// codepoint by codepoint produces separate letters with visible viramas --
/// roughly as wrong as rendering "cafe'" for "café", and it affects nearly
/// every Kannada word.
///
/// Running a full shaper on device is out of budget, so the substitutions are
/// precomputed: lib/EpdFont/scripts/gen_kannada_shaping.py shapes the cluster
/// space with HarfBuzz offline and emits KannadaTables.generated.h, whose
/// private-use codepoints address the shaped glyphs rasterized into the
/// .cpfont. This translates a logical codepoint run into those codepoints.
///
/// Logical text is what gets stored and cached; shaping happens on the way to
/// measurement and drawing, so dictionary lookup, clippings and search keep
/// seeing real Kannada.
namespace KannadaShape {

/// True when `utf8` may hold Kannada, judged from UTF-8 lead bytes alone
/// (U+0C80..U+0CFF encodes as 0xE0 followed by 0xB2 or 0xB3). Cheap enough to
/// call on every string so non-Kannada text never reaches the shaper.
bool mayContainKannada(const char* utf8);

/// Shape `utf8` into `out`. Returns false when there is nothing to change --
/// no Kannada, or the text already renders correctly as-is -- in which case
/// `out` is untouched and the caller should keep using the original string.
bool applyShaping(const char* utf8, std::string& out);

}  // namespace KannadaShape
