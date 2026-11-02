/* Decoder constant tables, ported from unbikit's src/bik-constants.ts.
 *
 * The packed strings themselves live in bink_tables_data.h, which is
 * generated from the reference source by gen_tables.py.
 */
#ifndef MABINK_TABLES_H
#define MABINK_TABLES_H

#include <cstdint>
#include <vector>

#include "bink_bits.h"

namespace mabink {

/* Number of entries in each 8x8-block parameter array. */
enum BlockParam {
  kBikParamBlockTypes = 0,
  kBikParamSubBlockTypes = 1,
  kBikParamColors = 2,
  kBikParamPattern = 3,
  kBikParamXOff = 4,
  kBikParamYOff = 5,
  kBikParamIntraDC = 6,
  kBikParamInterDC = 7,
  kBikParamRun = 8,
  kNumBlockParams = 9,
};

/* Block/sub-block type constants. */
enum BlockType {
  kBikBlockSkip = 0,
  kBikBlockScaled = 1,
  kBikBlockMotion = 2,
  kBikBlockRun = 3,
  kBikBlockResidue = 4,
  kBikBlockIntra = 5,
  kBikBlockFill = 6,
  kBikBlockInter = 7,
  kBikBlockPattern = 8,
  kBikBlockRaw = 9,
};

struct HuffTable {
  int maxBits = 0;
  std::vector<uint8_t> symbolsLens; /* (symbol << 4) | length */
};

/* Process-wide immutable tables, initialised on first use. */
struct Tables {
  std::vector<int32_t> patterns;
  std::vector<int32_t> scan;
  std::vector<int32_t> treeCodes;
  std::vector<int32_t> treeLens;
  std::vector<int32_t> quant;
  std::vector<int32_t> audioCriticalFreqs;
  std::vector<int32_t> audioRleLengths;
  HuffTable huff[16];

  static const Tables& get();
};

/* A Huffman tree: a predefined table plus a per-plane symbol permutation. */
struct Tree {
  const HuffTable* table = nullptr;
  uint8_t symbolMap[16] = {};
};

inline int getHuff(BitReader& reader, const Tree& tree) {
  const HuffTable& table = *tree.table;
  const int peek = static_cast<int>(reader.readBits(table.maxBits, true));
  const int entry = table.symbolsLens[static_cast<size_t>(peek)];
  reader.skip(static_cast<size_t>(entry & 0xf));
  return tree.symbolMap[entry >> 4];
}

}  // namespace mabink

#endif
