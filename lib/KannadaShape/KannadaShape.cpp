#include "KannadaShape.h"

#include <Utf8.h>

#include <cstddef>
#include <cstring>

#include "generated/KannadaTables.h"

namespace {

using namespace kannadaTables;

constexpr uint32_t KANNADA_FIRST = 0x0C80;
constexpr uint32_t KANNADA_LAST = 0x0CFF;
constexpr uint32_t VIRAMA = 0x0CCD;
constexpr uint32_t RA = 0x0CB0;
constexpr uint8_t NO_SECOND = 0xFF;

/// Longest consonant run a single cluster can contribute. Real Kannada stops
/// at three (ksha plus one stacked consonant); the cap only bounds the scratch
/// array so a malformed run of viramas cannot walk off it.
constexpr size_t MAX_CLUSTER_CONSONANTS = 8;

constexpr bool isKannada(const uint32_t cp) { return cp >= KANNADA_FIRST && cp <= KANNADA_LAST; }

/// Index into CONSONANTS, or -1. The block order is dense apart from two
/// unassigned slots, so a short scan beats carrying a 128-entry reverse map.
int consonantIndex(const uint32_t cp) {
  for (uint8_t i = 0; i < CONSONANT_COUNT; i++) {
    if (CONSONANTS[i] == cp) return i;
  }
  return -1;
}

/// Matra slot: 0 means "no matra", otherwise 1-based into MATRAS.
int matraSlot(const uint32_t cp) {
  for (size_t i = 0; i < sizeof(MATRAS) / sizeof(MATRAS[0]); i++) {
    if (MATRAS[i] == cp) return static_cast<int>(i) + 1;
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

/// Parse a cluster starting at `cursor`, which must sit on a Kannada
/// consonant. Leaves `cursor` just past what the cluster consumed.
Cluster parseCluster(const unsigned char*& cursor) {
  Cluster cluster;
  const unsigned char* probe = cursor;

  const unsigned char* afterFirst = probe;
  const uint32_t first = utf8NextCodepoint(&afterFirst);
  int index = consonantIndex(first);
  if (index < 0) return cluster;

  // A leading ra + virama is arkavattu: it reorders to the end of the cluster
  // rather than stacking. Only the FIRST consonant behaves this way -- ra in
  // any later position becomes an ordinary below-base form.
  if (first == RA) {
    const unsigned char* afterVirama = afterFirst;
    if (utf8NextCodepoint(&afterVirama) == VIRAMA) {
      const unsigned char* afterNext = afterVirama;
      const int nextIndex = consonantIndex(utf8NextCodepoint(&afterNext));
      if (nextIndex >= 0) {
        cluster.repha = true;
        probe = afterVirama;
        index = nextIndex;
      }
    }
  }

  if (!cluster.repha) {
    probe = afterFirst;
  } else {
    const unsigned char* afterNext = probe;
    utf8NextCodepoint(&afterNext);
    probe = afterNext;
  }
  cluster.consonants[cluster.consonantCount++] = static_cast<uint8_t>(index);

  // (virama + consonant)* -- each stacks beneath the base.
  for (;;) {
    const unsigned char* afterVirama = probe;
    if (utf8NextCodepoint(&afterVirama) != VIRAMA) break;
    const unsigned char* afterConsonant = afterVirama;
    const int nextIndex = consonantIndex(utf8NextCodepoint(&afterConsonant));
    if (nextIndex < 0) break;  // a trailing virama is left for the caller to emit
    if (cluster.consonantCount >= MAX_CLUSTER_CONSONANTS) break;
    cluster.consonants[cluster.consonantCount++] = static_cast<uint8_t>(nextIndex);
    probe = afterConsonant;
  }

  const unsigned char* afterMatra = probe;
  const int slot = matraSlot(utf8NextCodepoint(&afterMatra));
  if (slot > 0) {
    cluster.matra = static_cast<uint8_t>(slot);
    probe = afterMatra;
  }

  cluster.valid = true;
  cursor = probe;
  return cluster;
}

/// Resolve the base unit id, consuming the akhand pair when the first two
/// consonants form one (ksha, jnya). Returns the count of consumed consonants.
uint16_t resolveUnit(const Cluster& cluster, uint8_t& consumed) {
  if (cluster.consonantCount >= 2) {
    const uint16_t pairKey =
        static_cast<uint16_t>(cluster.consonants[0]) * CONSONANT_COUNT + cluster.consonants[1];
    const AkhandEntry* akhand = findByKey(AKHAND, sizeof(AKHAND) / sizeof(AKHAND[0]), pairKey);
    if (akhand != nullptr) {
      consumed = 2;
      return akhand->unitId;
    }
  }
  consumed = 1;
  return cluster.consonants[0];
}

/// Below-base codepoint for a consonant under a given matra, or 0.
uint32_t subscriptFor(const uint8_t consonant, const uint8_t matra) {
  const uint16_t pairKey = static_cast<uint16_t>(consonant) * MATRA_SLOTS + matra;
  if (const SubEntry* entry = findByKey(SUBSCRIPT, sizeof(SUBSCRIPT) / sizeof(SUBSCRIPT[0]), pairKey)) {
    return entry->codepoint;
  }
  if (const SubEntry* plain =
          findByKey(SUBSCRIPT_PLAIN, sizeof(SUBSCRIPT_PLAIN) / sizeof(SUBSCRIPT_PLAIN[0]), consonant)) {
    return plain->codepoint;
  }
  return 0;
}

/// Append the cluster's shaped codepoints. Returns false when the tables have
/// no rendering for it, leaving the caller to emit the logical codepoints.
bool emitCluster(const Cluster& cluster, std::string& out) {
  // Exceptions are clusters the factored tables do not reproduce; they are
  // keyed on the whole cluster and must win over the general path.
  if (cluster.consonantCount <= 2) {
    const uint8_t second = cluster.consonantCount > 1 ? cluster.consonants[1] : NO_SECOND;
    const uint32_t key = (static_cast<uint32_t>(cluster.repha ? 1 : 0) << 24) |
                         (static_cast<uint32_t>(cluster.consonants[0]) << 16) |
                         (static_cast<uint32_t>(second) << 8) | cluster.matra;
    if (const ExceptionEntry* entry =
            findByKey(EXCEPTIONS, sizeof(EXCEPTIONS) / sizeof(EXCEPTIONS[0]), key)) {
      for (uint8_t i = 0; i < entry->count; i++) {
        utf8AppendCodepoint(EXCEPTION_CODEPOINTS[entry->offset + i], out);
      }
      return true;
    }
  }

  uint8_t consumed = 0;
  const uint16_t unit = resolveUnit(cluster, consumed);
  const bool hasSubscript = cluster.consonantCount > consumed;

  const uint16_t baseKey = static_cast<uint16_t>(unit * (MATRA_SLOTS * 2) + cluster.matra * 2 +
                                                 (hasSubscript ? 1 : 0));
  const BaseEntry* entry = findByKey(BASE, sizeof(BASE) / sizeof(BASE[0]), baseKey);
  if (entry == nullptr && hasSubscript) {
    // No dedicated with-subscript form; the plain base keeps its shape.
    entry = findByKey(BASE, sizeof(BASE) / sizeof(BASE[0]),
                      static_cast<uint16_t>(unit * (MATRA_SLOTS * 2) + cluster.matra * 2));
  }
  if (entry == nullptr) return false;

  uint32_t subscripts[MAX_CLUSTER_CONSONANTS] = {};
  uint8_t subscriptCount = 0;
  for (uint8_t i = consumed; i < cluster.consonantCount; i++) {
    const uint32_t form = subscriptFor(cluster.consonants[i], cluster.matra);
    if (form == 0) return false;
    subscripts[subscriptCount++] = form;
  }

  for (uint8_t i = 0; i < entry->count; i++) {
    if (i == entry->insertAt) {
      for (uint8_t s = 0; s < subscriptCount; s++) utf8AppendCodepoint(subscripts[s], out);
    }
    utf8AppendCodepoint(BASE_CODEPOINTS[entry->offset + i], out);
  }
  if (entry->insertAt >= entry->count) {
    for (uint8_t s = 0; s < subscriptCount; s++) utf8AppendCodepoint(subscripts[s], out);
  }
  if (cluster.repha) utf8AppendCodepoint(REPH_CODEPOINT, out);
  return true;
}

/// Re-emit a cluster's logical codepoints when the tables cannot render it, so
/// unshaped text degrades to today's behaviour instead of vanishing.
void emitLogical(const Cluster& cluster, std::string& out) {
  if (cluster.repha) {
    utf8AppendCodepoint(RA, out);
    utf8AppendCodepoint(VIRAMA, out);
  }
  for (uint8_t i = 0; i < cluster.consonantCount; i++) {
    if (i > 0) utf8AppendCodepoint(VIRAMA, out);
    utf8AppendCodepoint(CONSONANTS[cluster.consonants[i]], out);
  }
  if (cluster.matra > 0) utf8AppendCodepoint(MATRAS[cluster.matra - 1], out);
}

}  // namespace

namespace KannadaShape {

bool mayContainKannada(const char* utf8) {
  if (utf8 == nullptr) return false;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8); *p; ++p) {
    // U+0C80..U+0CFF is 0xE0 0xB2|0xB3 0x80-0xBF in UTF-8.
    if (*p == 0xE0 && (p[1] == 0xB2 || p[1] == 0xB3)) return true;
  }
  return false;
}

bool applyShaping(const char* utf8, std::string& out) {
  if (!mayContainKannada(utf8)) return false;

  out.clear();
  bool changed = false;
  const unsigned char* cursor = reinterpret_cast<const unsigned char*>(utf8);
  while (*cursor) {
    const unsigned char* const clusterStart = cursor;
    const unsigned char* peek = cursor;
    const uint32_t cp = utf8NextCodepoint(&peek);
    if (!cp || cp == REPLACEMENT_GLYPH) break;

    if (!isKannada(cp) || consonantIndex(cp) < 0) {
      // Independent vowels, anusvara, visarga, digits and all non-Kannada text
      // are already encoded as the glyphs they render with.
      utf8AppendCodepoint(cp, out);
      cursor = peek;
      continue;
    }

    const Cluster cluster = parseCluster(cursor);
    if (!cluster.valid || cursor == clusterStart) {
      utf8AppendCodepoint(cp, out);
      cursor = peek;
      continue;
    }

    const size_t before = out.size();
    if (!emitCluster(cluster, out)) {
      out.resize(before);
      emitLogical(cluster, out);
      continue;
    }
    const size_t consumedBytes = static_cast<size_t>(cursor - clusterStart);
    if (!changed && (out.size() - before != consumedBytes ||
                     std::memcmp(out.data() + before, clusterStart, consumedBytes) != 0)) {
      changed = true;
    }
  }

  return changed;
}

}  // namespace KannadaShape
