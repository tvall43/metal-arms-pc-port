/* Decoder constant tables, ported from unbikit's src/bik-constants.ts. */

#include "bink_tables.h"

#include <cmath>
#include <cstdlib>
#include <string>

#include "bink_tables_data.h"

namespace mabink {
namespace {

int digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'z') return c - 'a' + 10;
  if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
  return 0;
}

/* Unpack base-36 values stored as `n` interleaved columns. */
std::vector<int32_t> unpackValues(size_t n, const char* s) {
  const size_t total = std::char_traits<char>::length(s);
  const size_t len = total / n;
  std::vector<int32_t> out;
  out.reserve(len);
  for (size_t index = 0; index < len; ++index) {
    int32_t value = 0;
    for (size_t i = 0; i < n; ++i) {
      value = value * 36 + digit(s[i * len + index]);
    }
    out.push_back(value);
  }
  return out;
}

/* Build the 32 intra/inter quantisation tables (16 levels each, 64 entries). */
std::vector<int32_t> buildQuantTables() {
  const std::vector<int32_t> base = unpackValues(4, PACKED_BIK_QUANT_BASE);
  static const double factors[16] = {
      1.0,      4.0 / 3,  5.0 / 3,  2.0,      8.0 / 3,  7.0 / 2,  4.0, 5.0,
      6.0,      8.0,      12.0,     17.0,     22.0,     28.0,     34.0, 44.0,
  };

  std::vector<int32_t> tables;
  tables.reserve(32 * 64);
  for (int level = 0; level < 32; ++level) {
    const double factor = factors[level % 16];
    const size_t subTableOffset = level > 15 ? 64 : 0;
    for (size_t i = 0; i < 64; ++i) {
      const size_t src = i + subTableOffset;
      const int32_t value = src < base.size() ? base[src] : 0;
      tables.push_back(static_cast<int32_t>(std::lround(value * factor)));
    }
  }
  return tables;
}

}  // namespace

const Tables& Tables::get() {
  static const Tables tables = [] {
    Tables t;
    t.patterns = unpackValues(2, PACKED_BIK_PATTERNS);
    t.scan = unpackValues(2, PACKED_BIK_SCAN);
    t.treeCodes = unpackValues(2, PACKED_BIK_TREE_CODES);
    t.treeLens = unpackValues(1, PACKED_BIK_TREE_LENS);
    t.quant = buildQuantTables();
    t.audioCriticalFreqs = unpackValues(3, PACKED_AUDIO_CRITICAL_FREQS);
    t.audioRleLengths = unpackValues(2, PACKED_AUDIO_RLE_LENGTH_TABLE);

    for (int index = 0; index < 16; ++index) {
      const size_t offset = static_cast<size_t>(index) << 4;
      HuffTable& table = t.huff[index];
      const int maxBits = t.treeLens[offset + 15];
      table.maxBits = maxBits;
      const size_t tableSize = static_cast<size_t>(1) << maxBits;
      table.symbolsLens.assign(tableSize, 0);

      for (int i = 0; i < 16; ++i) {
        const int code = t.treeCodes[offset + static_cast<size_t>(i)];
        const int len = t.treeLens[offset + static_cast<size_t>(i)];
        if (len < 1 || len > maxBits) continue;
        const size_t numEntries = static_cast<size_t>(1) << (maxBits - len);
        for (size_t j = 0; j < numEntries; ++j) {
          const size_t entry = (j << len) | static_cast<size_t>(code);
          if (entry < tableSize) {
            table.symbolsLens[entry] = static_cast<uint8_t>((i << 4) | len);
          }
        }
      }
    }
    return t;
  }();
  return tables;
}

}  // namespace mabink
