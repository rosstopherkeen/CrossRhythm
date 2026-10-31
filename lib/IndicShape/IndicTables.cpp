#include "IndicTables.h"

#include <Logging.h>

#include <cstring>

namespace IndicTables {

namespace {

uint16_t readLe16(const uint8_t* d) { return static_cast<uint16_t>(d[0] | (d[1] << 8)); }

uint32_t readLe32(const uint8_t* d) {
  return static_cast<uint32_t>(d[0]) | (static_cast<uint32_t>(d[1]) << 8) | (static_cast<uint32_t>(d[2]) << 16) |
         (static_cast<uint32_t>(d[3]) << 24);
}

/// Walks the sections in file order, handing out aligned, bounds-checked views.
class SectionCursor {
 public:
  SectionCursor(const uint8_t* data, const size_t size) : data_(data), size_(size), offset_(HEADER_SIZE) {}

  /// Point `out` at the next `count` elements of T, or fail if they overrun.
  template <typename T>
  bool take(const size_t count, const T*& out) {
    // An empty section occupies nothing, and files written before a section
    // existed end without the padding that would align it.
    if (count == 0) {
      out = nullptr;
      return true;
    }
    offset_ = (offset_ + 3) & ~static_cast<size_t>(3);
    const size_t bytes = count * sizeof(T);
    if (offset_ > size_ || bytes > size_ - offset_) return false;
    out = reinterpret_cast<const T*>(data_ + offset_);
    offset_ += bytes;
    return true;
  }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t offset_;
};

template <typename Entry>
bool keysAscending(const Entry* table, const size_t count) {
  for (size_t i = 1; i < count; i++) {
    if (!(table[i - 1].key < table[i].key)) return false;
  }
  return true;
}

}  // namespace

bool parse(const uint8_t* data, const size_t size, Script& out) {
  if (data == nullptr || size < HEADER_SIZE || size > MAX_FILE_SIZE) {
    LOG_ERR("SHAPE", "Rejecting .cpshape: size %u", static_cast<unsigned>(size));
    return false;
  }
  if ((reinterpret_cast<uintptr_t>(data) & 3) != 0) {
    LOG_ERR("SHAPE", "Rejecting .cpshape: buffer not 4-byte aligned");
    return false;
  }
  if (std::memcmp(data, MAGIC, sizeof(MAGIC)) != 0 || readLe16(data + 4) != FORMAT_VERSION ||
      readLe16(data + 6) != HEADER_SIZE || readLe32(data + 8) != size) {
    LOG_ERR("SHAPE", "Rejecting .cpshape: bad magic, version or length");
    return false;
  }

  Script s;
  s.blockFirst = readLe32(data + 12);
  s.blockLast = readLe32(data + 16);
  s.virama = readLe32(data + 20);
  s.ra = readLe32(data + 24);
  s.rephCodepoint = readLe32(data + 28);
  s.baseIsLast = data[32] != 0;
  s.consonantCount = data[33];
  s.matraSlots = data[34];
  s.baseCount = readLe16(data + 36);
  s.baseCodepointCount = readLe16(data + 38);
  s.subscriptCount = readLe16(data + 40);
  s.subscriptPlainCount = readLe16(data + 42);
  s.akhandCount = readLe16(data + 44);
  s.exceptionCount = readLe16(data + 46);
  s.exceptionCodepointCount = readLe16(data + 48);
  s.nuktaPairCount = readLe16(data + 50);
  s.nukta = readLe32(data + 52);

  // The shaper's UTF-8 prefilter and the fallback router only know blocks with
  // a 0xE0 lead byte; anything else is not a script this firmware can drive.
  if (s.blockFirst < 0x0800 || s.blockLast > 0x0FFF || s.blockFirst > s.blockLast || s.consonantCount == 0 ||
      s.matraSlots == 0) {
    LOG_ERR("SHAPE", "Rejecting .cpshape: block U+%04X-U+%04X out of range", static_cast<unsigned>(s.blockFirst),
            static_cast<unsigned>(s.blockLast));
    return false;
  }

  SectionCursor cursor(data, size);
  if (!cursor.take(s.consonantCount, s.consonants) || !cursor.take(s.matraSlots - 1u, s.matras) ||
      !cursor.take(s.baseCount, s.base) || !cursor.take(s.baseCodepointCount, s.baseCodepoints) ||
      !cursor.take(s.subscriptCount, s.subscript) || !cursor.take(s.subscriptPlainCount, s.subscriptPlain) ||
      !cursor.take(s.akhandCount, s.akhand) || !cursor.take(s.exceptionCount, s.exceptions) ||
      !cursor.take(s.exceptionCodepointCount, s.exceptionCodepoints) ||
      !cursor.take(s.nuktaPairCount, s.nuktaPairs)) {
    LOG_ERR("SHAPE", "Rejecting .cpshape: a section overruns the file");
    return false;
  }

  // Every run an entry points at must sit inside its pool, so the shaper can
  // index without checking on the hot path.
  for (uint16_t i = 0; i < s.baseCount; i++) {
    if (static_cast<uint32_t>(s.base[i].offset) + s.base[i].count > s.baseCodepointCount) {
      LOG_ERR("SHAPE", "Rejecting .cpshape: base entry %u overruns its pool", static_cast<unsigned>(i));
      return false;
    }
  }
  for (uint16_t i = 0; i < s.exceptionCount; i++) {
    if (static_cast<uint32_t>(s.exceptions[i].offset) + s.exceptions[i].count > s.exceptionCodepointCount) {
      LOG_ERR("SHAPE", "Rejecting .cpshape: exception %u overruns its pool", static_cast<unsigned>(i));
      return false;
    }
  }
  if (!keysAscending(s.base, s.baseCount) || !keysAscending(s.subscript, s.subscriptCount) ||
      !keysAscending(s.subscriptPlain, s.subscriptPlainCount) || !keysAscending(s.akhand, s.akhandCount) ||
      !keysAscending(s.exceptions, s.exceptionCount)) {
    LOG_ERR("SHAPE", "Rejecting .cpshape: a key table is not sorted");
    return false;
  }

  out = s;
  return true;
}

}  // namespace IndicTables
