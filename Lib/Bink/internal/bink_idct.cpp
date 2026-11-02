/* Inverse DCT and block-add helpers, ported from unbikit's src/transforms.ts. */

#include "bink_idct.h"

namespace mabink {
namespace {

/* DCT constants, scaled by 2048 (see gen_tables.py / unbikit bik-defines.ts). */
constexpr int kDctC0 = 2896;
constexpr int kDctC1 = 2217;
constexpr int kDctC2 = 3784;
constexpr int kDctC3 = -5352;

inline void store(int32_t* intDest, uint8_t* byteDest, size_t destSize,
                  int64_t index, int value, int destShift) {
  value >>= destShift;
  if (intDest) {
    intDest[index] = value;
    return;
  }
  if (index < 0 || static_cast<size_t>(index) >= destSize) return;
  byteDest[index] = static_cast<uint8_t>(value);
}

/* One 1-D pass. `column` selects the strided (vertical) or contiguous pass. */
void idct1d(const int32_t* src, int srcOffset, int32_t* intDest,
            uint8_t* byteDest, size_t destSize, int64_t destOffset,
            bool column) {
  const int indexShift = column ? 3 : 0;
  const int constantToAdd = column ? 0 : 0x7f;
  const int destShift = column ? 0 : 8;

  int a0 = src[srcOffset] + constantToAdd;
  int b0 = src[srcOffset + (1 << indexShift)];
  int a2 = src[srcOffset + (2 << indexShift)];
  const int x3 = src[srcOffset + (3 << indexShift)];
  const int x4 = src[srcOffset + (4 << indexShift)];
  int a4 = src[srcOffset + (5 << indexShift)];
  const int x6 = src[srcOffset + (6 << indexShift)];
  const int x7 = src[srcOffset + (7 << indexShift)];

  const int a1 = a0 - x4;
  const int a3 = (kDctC0 * (a2 - x6)) >> 11;
  const int a5 = a4 - x3;
  const int a7 = b0 - x7;
  a0 += x4;
  a2 += x6;
  a4 += x3;
  b0 += x7;

  const int a0pa2 = a0 + a2;
  const int a0ma2 = a0 - a2;
  const int a1pa3ma3 = a1 + a3 - a2;
  const int a1ma3pa2 = a1 - a3 + a2;

  const int b1 = (kDctC2 * (a5 + a7)) >> 11;
  int b3 = (kDctC0 * (b0 - a4)) >> 11;
  b0 += a4;
  const int b2 = ((kDctC3 * a5) >> 11) - b0 + b1;
  b3 -= b2;
  const int b4 = ((kDctC1 * a7) >> 11) + b3 - b1;

  store(intDest, byteDest, destSize, destOffset, a0pa2 + b0, destShift);
  store(intDest, byteDest, destSize, destOffset + (1 << indexShift),
        a1pa3ma3 + b2, destShift);
  store(intDest, byteDest, destSize, destOffset + (2 << indexShift),
        a1ma3pa2 + b3, destShift);
  store(intDest, byteDest, destSize, destOffset + (3 << indexShift),
        a0ma2 - b4, destShift);
  store(intDest, byteDest, destSize, destOffset + (4 << indexShift),
        a0ma2 + b4, destShift);
  store(intDest, byteDest, destSize, destOffset + (5 << indexShift),
        a1ma3pa2 - b3, destShift);
  store(intDest, byteDest, destSize, destOffset + (6 << indexShift),
        a1pa3ma3 - b2, destShift);
  store(intDest, byteDest, destSize, destOffset + (7 << indexShift),
        a0pa2 - b0, destShift);
}

}  // namespace

void addBlock8x8(const int32_t* block, uint8_t* dest, size_t destSize,
                 int64_t destOffset, int stride) {
  stride -= 8;
  int i = -1;
  int iMax = 6;
  while (i++ < 63) {
    const int64_t index = destOffset + i;
    if (index >= 0 && static_cast<size_t>(index) < destSize) {
      dest[index] = static_cast<uint8_t>(dest[index] + block[i]);
    }
    if (i > iMax) {
      destOffset += stride;
      iMax += 8;
    }
  }
}

void idctPut(const int32_t* block, uint8_t* dest, size_t destSize,
             int64_t destOffset, int stride) {
  /* Column pass is in-place on the coefficient block. */
  int32_t work[64];
  for (int i = 0; i < 64; ++i) work[i] = block[i];
  for (int i = 0; i < 8; ++i) {
    idct1d(work, i, work, nullptr, 0, i, true);
  }

  for (int i = 0; i < 64; i += 8) {
    if (destOffset < 0 || static_cast<size_t>(destOffset) >= destSize) break;
    idct1d(work, i, nullptr, dest, destSize, destOffset, false);
    destOffset += stride;
  }
}

void idctAdd(const int32_t* block, uint8_t* dest, size_t destSize,
             int64_t destOffset, int stride) {
  int32_t work[64];
  for (int i = 0; i < 64; ++i) work[i] = block[i];
  for (int i = 0; i < 8; ++i) {
    idct1d(work, i, work, nullptr, 0, i, true);
  }
  for (int i = 0; i < 64; i += 8) {
    idct1d(work, i, work, nullptr, 0, i, false);
  }
  addBlock8x8(work, dest, destSize, destOffset, stride);
}

}  // namespace mabink
