/* Inverse DCT and block-add helpers, ported from unbikit's src/transforms.ts.
 *
 * This is the integer AAN approximation used by Bink 1, not an exact
 * floating-point IDCT. Keep the constants and shifts identical to the
 * reference if you touch this file; the output must match bit for bit.
 */
#ifndef MABINK_IDCT_H
#define MABINK_IDCT_H

#include <cstddef>
#include <cstdint>

namespace mabink {

/* Dequantise and transform 64 coefficients into an 8x8 destination block.
 * Writes outside [0, destSize) are ignored, matching typed-array semantics. */
void idctPut(const int32_t* block, uint8_t* dest, size_t destSize,
             int64_t destOffset, int stride);

/* Same transform, but add the result to the destination instead of storing. */
void idctAdd(const int32_t* block, uint8_t* dest, size_t destSize,
             int64_t destOffset, int stride);

/* Add a transformed 8x8 block to a destination plane (in place). */
void addBlock8x8(const int32_t* block, uint8_t* dest, size_t destSize,
                 int64_t destOffset, int stride);

}  // namespace mabink

#endif
