#pragma once

#include <cstddef>
#include <cstdint>

/// Shaping tables for one Indic script, and the `.cpshape` file they load from.
///
/// The tables are precomputed HarfBuzz output (gen_indic_shaping.py) and live on
/// the SD card next to the font that draws them, as `<Family>/shaping.cpshape`,
/// rather than in firmware. Shaping data is tied to one font -- its private-use
/// codepoints address glyphs rasterized into that font's .cpfont files -- so it
/// ships with the font, and installing a font is all a reader has to do. It also
/// keeps the firmware free of the per-script tables, which together are far more
/// than any target's flash has free.
///
/// A loaded file is used in place: `parse()` validates it and points a `Script`
/// at its sections, so the only RAM cost is the file itself.
namespace IndicTables {

/// Base unit + matra rendering. `insertAt` is where attached forms go.
struct BaseEntry {
  uint16_t key;     ///< unitId * (matraSlots*2) + matraSlot*2 + hasAttached
  uint16_t offset;  ///< into baseCodepoints
  uint8_t count;
  uint8_t insertAt;  ///< index within this run where attached forms go
};

/// Attached form for (consonant, matra), or the matra-independent form when used
/// in subscriptPlain (where the key is the consonant id alone).
struct SubEntry {
  uint16_t key;  ///< consonantId * matraSlots + matraSlot
  uint16_t codepoint;
};

/// Consonant pairs that render as a single base unit (ksha, jnya).
struct AkhandEntry {
  uint16_t key;  ///< firstId * consonantCount + secondId
  uint16_t unitId;
};

/// A cluster the factored tables do not reproduce, stored literally.
struct ExceptionEntry {
  uint32_t key;     ///< reph<<24 | firstId<<16 | secondId<<8 | matraSlot (secondId 0xFF = none)
  uint16_t offset;  ///< into exceptionCodepoints
  uint8_t count;
  uint8_t reserved;
};

/// A consonant + nukta pair that has its own precomposed letter (Hindi qa, za,
/// fa, ...). Books write these decomposed, because Unicode keeps the
/// precomposed letters out of NFC, but the font only has glyphs for the
/// precomposed form.
struct NuktaPair {
  uint16_t base;
  uint16_t composed;
};

// The file stores these arrays byte for byte and parse() points straight into
// them, so their layout is part of the format. Both the ESP32 targets and the
// host are little-endian, which the generator also writes.
static_assert(sizeof(BaseEntry) == 6 && alignof(BaseEntry) == 2, "BaseEntry is part of the .cpshape format");
static_assert(sizeof(SubEntry) == 4, "SubEntry is part of the .cpshape format");
static_assert(sizeof(NuktaPair) == 4, "NuktaPair is part of the .cpshape format");
static_assert(sizeof(AkhandEntry) == 4, "AkhandEntry is part of the .cpshape format");
static_assert(sizeof(ExceptionEntry) == 8 && alignof(ExceptionEntry) == 4,
              "ExceptionEntry is part of the .cpshape format");

/// One script's block constants plus every table, as the shaper sees it. All
/// pointers address the loaded file; the file must outlive the Script.
///
/// `rephCodepoint` is 0 for a script with no repha, which also tells the parser
/// not to treat a cluster-initial ra + virama specially.
///
/// `baseIsLast` says which consonant of a conjunct is the base -- the one that
/// keeps its full form and carries the vowel sign. It is measured per font by
/// the generator (whichever reproduces more clusters), not assumed from the
/// script's grammar.
struct Script {
  uint32_t blockFirst = 0;
  uint32_t blockLast = 0;
  uint32_t virama = 0;
  uint32_t ra = 0;
  uint32_t rephCodepoint = 0;
  bool baseIsLast = false;

  const uint16_t* consonants = nullptr;
  uint8_t consonantCount = 0;
  const uint16_t* matras = nullptr;
  uint8_t matraSlots = 0;  ///< matra count + 1, slot 0 being "no matra"

  const BaseEntry* base = nullptr;
  uint16_t baseCount = 0;
  const uint16_t* baseCodepoints = nullptr;
  uint16_t baseCodepointCount = 0;

  const SubEntry* subscript = nullptr;
  uint16_t subscriptCount = 0;
  const SubEntry* subscriptPlain = nullptr;
  uint16_t subscriptPlainCount = 0;

  const AkhandEntry* akhand = nullptr;
  uint16_t akhandCount = 0;

  const ExceptionEntry* exceptions = nullptr;
  uint16_t exceptionCount = 0;
  const uint16_t* exceptionCodepoints = nullptr;
  uint16_t exceptionCodepointCount = 0;

  uint32_t nukta = 0;  ///< 0 when the script folds no nukta pairs
  const NuktaPair* nuktaPairs = nullptr;
  uint16_t nuktaPairCount = 0;
};

/// `.cpshape` v1, little-endian.
///
///   [0-3]    magic "CPSH"
///   [4-5]    version (= 1)
///   [6-7]    header size (= HEADER_SIZE)
///   [8-11]   file size
///   [12-15]  blockFirst
///   [16-19]  blockLast
///   [20-23]  virama
///   [24-27]  ra
///   [28-31]  rephCodepoint (0 = none)
///   [32]     baseIsLast
///   [33]     consonantCount
///   [34]     matraSlots
///   [35]     reserved
///   [36-37]  baseCount
///   [38-39]  baseCodepointCount
///   [40-41]  subscriptCount
///   [42-43]  subscriptPlainCount
///   [44-45]  akhandCount
///   [46-47]  exceptionCount
///   [48-49]  exceptionCodepointCount
///   [50-51]  nuktaPairCount (0 in files written before nukta folding)
///   [52-55]  nukta codepoint
///   [56-63]  reserved
///
/// Then, each starting on a 4-byte boundary, in this order: consonants
/// (uint16), matras (uint16, matraSlots - 1 of them), base, baseCodepoints,
/// subscript, subscriptPlain, akhand, exceptions, exceptionCodepoints,
/// nuktaPairs.
constexpr uint8_t MAGIC[4] = {'C', 'P', 'S', 'H'};
/// Name the file carries inside its font family's folder. Fixed so the font
/// system can find it with one lookup and the uploader can whitelist it.
constexpr char FILE_NAME[] = "shaping.cpshape";
constexpr uint16_t FORMAT_VERSION = 1;
constexpr size_t HEADER_SIZE = 64;

/// Largest file parse() accepts. Bengali, the largest script, is about 260 KB;
/// the cap rejects a garbage length before anything is allocated for it.
constexpr size_t MAX_FILE_SIZE = 512 * 1024;

/// Validate a loaded `.cpshape` file and point `out` at its sections. `data`
/// must be 4-byte aligned and outlive `out`. Every table is bounds-checked and
/// every key table checked for the sort order the lookups binary-search on, so
/// a damaged file on the SD card is rejected here rather than read out of
/// bounds while drawing. Returns false (and logs why) on any mismatch.
bool parse(const uint8_t* data, size_t size, Script& out);

}  // namespace IndicTables
