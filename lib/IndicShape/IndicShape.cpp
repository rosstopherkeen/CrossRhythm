#include "IndicShape.h"

#include <Utf8.h>

#include <cstddef>
#include <cstring>

#include "IndicTables.h"

namespace {

using IndicTables::AkhandEntry;
using IndicTables::BaseEntry;
using IndicTables::ExceptionEntry;
using IndicTables::Script;
using IndicTables::SubEntry;

constexpr uint8_t NO_SECOND = 0xFF;

/// Longest consonant run a single cluster can contribute. Real text stops at
/// three (an akhand ligature plus one stacked consonant); the cap only bounds
/// the scratch array so a malformed run of viramas cannot walk off it.
constexpr size_t MAX_CLUSTER_CONSONANTS = 8;

/// Scripts whose `.cpshape` files are loaded. Fixed storage: a Script is a few
/// dozen bytes of pointers into its file, and there are only seven scripts this
/// shaper's model covers, so a static array avoids a heap block that would live
/// as long as the fonts do.
Script gScripts[IndicShape::MAX_SCRIPTS];
size_t gScriptCount = 0;

/// Range of second UTF-8 bytes the registered blocks encode to, for the
/// mayContainIndic() prefilter. FIRST > LAST means nothing is registered.
uint8_t gSecondByteFirst = 0xFF;
uint8_t gSecondByteLast = 0x00;

/// Second UTF-8 byte of a codepoint in U+0800..U+0FFF, which parse() requires
/// every block to be: 0xE0 then 0x80 | ((cp >> 6) & 0x3F), then the low six bits.
constexpr uint8_t secondUtf8Byte(const uint32_t cp) { return static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F)); }

/// The script owning `cp`, or nullptr when no registered script covers it.
const Script* scriptFor(const uint32_t cp) {
  for (size_t i = 0; i < gScriptCount; i++) {
    if (cp >= gScripts[i].blockFirst && cp <= gScripts[i].blockLast) return &gScripts[i];
  }
  return nullptr;
}

/// Index into `script.consonants`, or -1. The runs are short and mostly dense,
/// so a scan beats carrying a 128-entry reverse map per script.
int consonantIndex(const Script& script, const uint32_t cp) {
  for (uint8_t i = 0; i < script.consonantCount; i++) {
    if (script.consonants[i] == cp) return i;
  }
  return -1;
}

/// Consonant at `cursor`, advancing past it, or -1 (cursor untouched). A
/// consonant followed by the script's nukta folds into its precomposed letter
/// when the font has one, so decomposed Hindi text (ज + ़) reaches the tables as
/// the single letter (ज़) the font draws.
int readConsonant(const Script& script, const unsigned char*& cursor) {
  const unsigned char* after = cursor;
  const uint32_t cp = utf8NextCodepoint(&after);
  int index = consonantIndex(script, cp);
  if (index < 0) return -1;
  if (script.nukta != 0) {
    const unsigned char* afterNukta = after;
    if (utf8NextCodepoint(&afterNukta) == script.nukta) {
      for (uint16_t i = 0; i < script.nuktaPairCount; i++) {
        if (script.nuktaPairs[i].base != cp) continue;
        const int composed = consonantIndex(script, script.nuktaPairs[i].composed);
        if (composed >= 0) {
          index = composed;
          after = afterNukta;
        }
        break;
      }
    }
  }
  cursor = after;
  return index;
}

/// Matra slot: 0 means "no matra", otherwise 1-based into `script.matras`.
int matraSlot(const Script& script, const uint32_t cp) {
  for (uint8_t i = 0; i + 1 < script.matraSlots; i++) {
    if (script.matras[i] == cp) return static_cast<int>(i) + 1;
  }
  return 0;
}

template <typename Entry>
const Entry* findByKey(const Entry* table, const size_t count, const uint32_t key) {
  size_t low = 0;
  size_t high = count;
  while (low < high) {
    const size_t mid = low + (high - low) / 2;
    if (table[mid].key < key) {
      low = mid + 1;
    } else if (table[mid].key > key) {
      high = mid;
    } else {
      return &table[mid];
    }
  }
  return nullptr;
}

/// One parsed cluster: an optional leading repha, the consonant run, a matra,
/// and how many input codepoints it consumed.
struct Cluster {
  uint8_t consonants[MAX_CLUSTER_CONSONANTS];
  uint8_t consonantCount = 0;
  uint8_t matra = 0;
  bool repha = false;
  bool valid = false;
};

/// Parse a cluster starting at `cursor`, which must sit on a consonant of
/// `script`. Leaves `cursor` just past what the cluster consumed.
Cluster parseCluster(const Script& script, const unsigned char*& cursor) {
  Cluster cluster;

  const unsigned char* afterFirst = cursor;
  int index = readConsonant(script, afterFirst);
  if (index < 0) return cluster;
  const unsigned char* probe = afterFirst;

  // Where the script has a repha, a leading ra + virama reorders out of the
  // cluster rather than stacking (Kannada's arkavattu, Telugu's ra-vattu).
  // Only the FIRST consonant behaves this way -- ra in any later position
  // becomes an ordinary below-base form. Scripts with rephCodepoint == 0 have
  // no such form, so their cluster-initial ra is left as a plain consonant.
  const unsigned char* afterRaw = cursor;
  if (script.rephCodepoint != 0 && utf8NextCodepoint(&afterRaw) == script.ra &&
      utf8NextCodepoint(&afterRaw) == script.virama) {
    const unsigned char* afterNext = afterRaw;
    const int nextIndex = readConsonant(script, afterNext);
    if (nextIndex >= 0) {
      cluster.repha = true;
      index = nextIndex;
      probe = afterNext;
    }
  }
  cluster.consonants[cluster.consonantCount++] = static_cast<uint8_t>(index);

  // (virama + consonant)* -- each stacks beneath the base.
  for (;;) {
    const unsigned char* afterVirama = probe;
    if (utf8NextCodepoint(&afterVirama) != script.virama) break;
    const unsigned char* afterConsonant = afterVirama;
    const int nextIndex = readConsonant(script, afterConsonant);
    if (nextIndex < 0) break;  // a trailing virama is left for the caller to emit
    if (cluster.consonantCount >= MAX_CLUSTER_CONSONANTS) break;
    cluster.consonants[cluster.consonantCount++] = static_cast<uint8_t>(nextIndex);
    probe = afterConsonant;
  }

  const unsigned char* afterMatra = probe;
  const int slot = matraSlot(script, utf8NextCodepoint(&afterMatra));
  if (slot > 0) {
    cluster.matra = static_cast<uint8_t>(slot);
    probe = afterMatra;
  }

  cluster.valid = true;
  cursor = probe;
  return cluster;
}

/// The base unit of a cluster plus the range of consonants attached to it.
struct Unit {
  uint16_t id = 0;
  uint8_t attachStart = 0;  ///< first attached consonant, index into cluster
  uint8_t attachCount = 0;
};

/// Resolve the base unit, taking it from whichever end of the consonant run
/// this script puts it and consuming an akhand pair (ksha, jnya) as one unit.
/// The consonants left over are the attached forms, in logical order.
Unit resolveUnit(const Script& script, const Cluster& cluster) {
  const uint8_t count = cluster.consonantCount;
  // Index of the first consonant of the candidate akhand pair.
  const uint8_t pairFirst = script.baseIsLast ? static_cast<uint8_t>(count - 2) : 0;
  if (count >= 2) {
    const uint16_t pairKey = static_cast<uint16_t>(cluster.consonants[pairFirst]) * script.consonantCount +
                             cluster.consonants[pairFirst + 1];
    if (const AkhandEntry* akhand = findByKey(script.akhand, script.akhandCount, pairKey)) {
      // The pair is the unit; everything outside it attaches to it.
      return {akhand->unitId, script.baseIsLast ? uint8_t{0} : uint8_t{2}, static_cast<uint8_t>(count - 2)};
    }
  }
  if (script.baseIsLast) {
    return {cluster.consonants[count - 1], 0, static_cast<uint8_t>(count - 1)};
  }
  return {cluster.consonants[0], 1, static_cast<uint8_t>(count - 1)};
}

/// Below-base codepoint for a consonant under a given matra, or 0.
uint32_t subscriptFor(const Script& script, const uint8_t consonant, const uint8_t matra) {
  const uint16_t pairKey = static_cast<uint16_t>(consonant) * script.matraSlots + matra;
  if (const SubEntry* entry = findByKey(script.subscript, script.subscriptCount, pairKey)) {
    return entry->codepoint;
  }
  if (const SubEntry* plain = findByKey(script.subscriptPlain, script.subscriptPlainCount, consonant)) {
    return plain->codepoint;
  }
  return 0;
}

/// Append the cluster's shaped codepoints. Returns false when the tables have
/// no rendering for it, leaving the caller to emit the logical codepoints.
bool emitCluster(const Script& script, const Cluster& cluster, std::string& out) {
  // Exceptions are clusters the factored tables do not reproduce; they are
  // keyed on the whole cluster and must win over the general path.
  if (cluster.consonantCount <= 2) {
    const uint8_t second = cluster.consonantCount > 1 ? cluster.consonants[1] : NO_SECOND;
    const uint32_t key = (static_cast<uint32_t>(cluster.repha ? 1 : 0) << 24) |
                         (static_cast<uint32_t>(cluster.consonants[0]) << 16) | (static_cast<uint32_t>(second) << 8) |
                         cluster.matra;
    if (const ExceptionEntry* entry = findByKey(script.exceptions, script.exceptionCount, key)) {
      for (uint8_t i = 0; i < entry->count; i++) {
        utf8AppendCodepoint(script.exceptionCodepoints[entry->offset + i], out);
      }
      return true;
    }
  }

  const Unit unit = resolveUnit(script, cluster);
  const bool hasSubscript = unit.attachCount > 0;

  const uint16_t baseKey =
      static_cast<uint16_t>(unit.id * (script.matraSlots * 2) + cluster.matra * 2 + (hasSubscript ? 1 : 0));
  const BaseEntry* entry = findByKey(script.base, script.baseCount, baseKey);
  if (entry == nullptr && hasSubscript) {
    // No dedicated with-subscript form; the plain base keeps its shape.
    entry = findByKey(script.base, script.baseCount,
                      static_cast<uint16_t>(unit.id * (script.matraSlots * 2) + cluster.matra * 2));
  }
  if (entry == nullptr) return false;

  uint32_t subscripts[MAX_CLUSTER_CONSONANTS] = {};
  uint8_t subscriptCount = 0;
  for (uint8_t i = 0; i < unit.attachCount; i++) {
    const uint32_t form = subscriptFor(script, cluster.consonants[unit.attachStart + i], cluster.matra);
    if (form == 0) return false;
    subscripts[subscriptCount++] = form;
  }

  for (uint8_t i = 0; i < entry->count; i++) {
    if (i == entry->insertAt) {
      for (uint8_t s = 0; s < subscriptCount; s++) utf8AppendCodepoint(subscripts[s], out);
    }
    utf8AppendCodepoint(script.baseCodepoints[entry->offset + i], out);
  }
  if (entry->insertAt >= entry->count) {
    for (uint8_t s = 0; s < subscriptCount; s++) utf8AppendCodepoint(subscripts[s], out);
  }
  if (cluster.repha) utf8AppendCodepoint(script.rephCodepoint, out);
  return true;
}

/// Re-emit a cluster's logical codepoints when the tables cannot render it, so
/// unshaped text degrades to today's behaviour instead of vanishing.
void emitLogical(const Script& script, const Cluster& cluster, std::string& out) {
  if (cluster.repha) {
    utf8AppendCodepoint(script.ra, out);
    utf8AppendCodepoint(script.virama, out);
  }
  for (uint8_t i = 0; i < cluster.consonantCount; i++) {
    if (i > 0) utf8AppendCodepoint(script.virama, out);
    utf8AppendCodepoint(script.consonants[cluster.consonants[i]], out);
  }
  if (cluster.matra > 0) utf8AppendCodepoint(script.matras[cluster.matra - 1], out);
}

}  // namespace

namespace IndicShape {

bool registerScript(const uint8_t* data, const size_t size) {
  if (gScriptCount >= MAX_SCRIPTS) return false;
  Script script;
  if (!IndicTables::parse(data, size, script)) return false;
  for (size_t i = 0; i < gScriptCount; i++) {
    // One script per block: a second font carrying the same script would make
    // the lookup order decide which tables win, so the first registration does.
    if (script.blockFirst <= gScripts[i].blockLast && gScripts[i].blockFirst <= script.blockLast) return false;
  }
  gScripts[gScriptCount++] = script;
  const uint8_t first = secondUtf8Byte(script.blockFirst);
  const uint8_t last = secondUtf8Byte(script.blockLast);
  if (first < gSecondByteFirst) gSecondByteFirst = first;
  if (last > gSecondByteLast) gSecondByteLast = last;
  return true;
}

void clearScripts() {
  gScriptCount = 0;
  gSecondByteFirst = 0xFF;
  gSecondByteLast = 0x00;
}

size_t scriptCount() { return gScriptCount; }

uint32_t probeCodepoint(const size_t index) { return index < gScriptCount ? gScripts[index].consonants[0] : 0; }

bool mayContainIndic(const char* utf8) {
  if (utf8 == nullptr || gScriptCount == 0) return false;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8); *p; ++p) {
    // Every registered block is 0xE0 <second byte> 0x80-0xBF in UTF-8. Reading
    // p[1] at the end of the string reads the terminator.
    if (*p == 0xE0 && p[1] >= gSecondByteFirst && p[1] <= gSecondByteLast) return true;
  }
  return false;
}

bool applyShaping(const char* utf8, std::string& out) {
  if (!mayContainIndic(utf8)) return false;

  out.clear();
  bool changed = false;
  const unsigned char* cursor = reinterpret_cast<const unsigned char*>(utf8);
  while (*cursor) {
    const unsigned char* const clusterStart = cursor;
    const unsigned char* peek = cursor;
    const uint32_t cp = utf8NextCodepoint(&peek);
    if (!cp || cp == REPLACEMENT_GLYPH) break;

    const Script* script = scriptFor(cp);
    if (script == nullptr || consonantIndex(*script, cp) < 0) {
      // Independent vowels, anusvara, visarga, digits and all text outside the
      // supported blocks are already encoded as the glyphs they render with.
      utf8AppendCodepoint(cp, out);
      cursor = peek;
      continue;
    }

    const Cluster cluster = parseCluster(*script, cursor);
    if (!cluster.valid || cursor == clusterStart) {
      utf8AppendCodepoint(cp, out);
      cursor = peek;
      continue;
    }

    const size_t before = out.size();
    if (!emitCluster(*script, cluster, out)) {
      out.resize(before);
      emitLogical(*script, cluster, out);
      continue;
    }
    const size_t consumedBytes = static_cast<size_t>(cursor - clusterStart);
    if (!changed &&
        (out.size() - before != consumedBytes || std::memcmp(out.data() + before, clusterStart, consumedBytes) != 0)) {
      changed = true;
    }
  }

  return changed;
}

}  // namespace IndicShape
