#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

/// Indic (Brahmic) text shaping.
///
/// These scripts stack or attach the consonants of a cluster to its base letter
/// and merge most vowel signs into the base glyph. Those combined shapes come
/// out of the font's GSUB table and carry no Unicode codepoints, so drawing
/// text codepoint by codepoint produces separate letters with visible viramas
/// -- roughly as wrong as rendering "cafe'" for "café", and it affects nearly
/// every word.
///
/// Running a full shaper on device is out of budget, so the substitutions are
/// precomputed: lib/EpdFont/scripts/gen_indic_shaping.py shapes a script's
/// cluster space with HarfBuzz offline and writes a `.cpshape` file whose
/// private-use codepoints address the shaped glyphs rasterized into that font's
/// .cpfont files. The file ships inside the font's folder on the SD card, so the
/// firmware carries no per-script data: installing the font installs the
/// shaping. This translates a logical codepoint run into those codepoints.
///
/// Logical text is what gets stored and cached; shaping happens on the way to
/// measurement and drawing, so dictionary lookup, clippings and search keep
/// seeing real text.
namespace IndicShape {

/// One slot per script the generator's model covers (Devanagari, Bengali, Odia,
/// Tamil, Telugu, Kannada, Malayalam), plus a spare.
constexpr size_t MAX_SCRIPTS = 8;

/// Register a loaded `.cpshape` file. `data` must be 4-byte aligned and stay
/// valid until clearScripts(); the shaper reads it in place. Returns false
/// (logging why) for a malformed file, a script whose block is already
/// registered, or when every slot is taken.
bool registerScript(const uint8_t* data, size_t size);

/// Forget every registered script. Call before freeing the files they point at.
void clearScripts();

size_t scriptCount();

/// A consonant every font with real coverage of registered script `index`
/// carries -- the script's first consonant. 0 for an out-of-range index.
uint32_t probeCodepoint(size_t index);

/// True when `utf8` may hold text in a supported Indic script, judged from
/// UTF-8 lead bytes alone (the supported blocks all encode as 0xE0 followed by
/// one byte of a short range). Cheap enough to call on every string so other
/// scripts never reach the shaper.
///
/// Deliberately coarse: it tests one contiguous byte range, so a block sitting
/// between two registered ones -- Gurmukhi and Gujarati, which CrossInk cannot
/// shape -- also answers true. applyShaping() then passes that text through
/// unchanged, since no registered script claims those codepoints.
bool mayContainIndic(const char* utf8);

/// Shape `utf8` into `out`. Returns false when there is nothing to change --
/// no supported Indic text, or the text already renders correctly as-is -- in
/// which case `out` is untouched and the caller should keep using the original
/// string.
bool applyShaping(const char* utf8, std::string& out);

}  // namespace IndicShape
