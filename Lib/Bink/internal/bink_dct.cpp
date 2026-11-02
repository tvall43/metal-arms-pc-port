/* Inverse DCT used by Bink Audio (DCT variant). */

#include "bink_dct.h"

#include <cmath>

namespace mabink {

InverseDct::InverseDct(int nBits) : nBase_(1 << nBits), nBits_(nBits) {
  reciprocalCosTables_.resize(static_cast<size_t>(nBits) + 1);
  for (int tableIndex = 0; tableIndex <= nBits; ++tableIndex) {
    const int n = 1 << tableIndex;
    std::vector<float>& table = reciprocalCosTables_[tableIndex];
    table.resize(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      table[i] =
          static_cast<float>(0.5 / std::cos((i + 0.5) * 3.14159265358979323846 / n));
    }
  }
  temp_.assign(static_cast<size_t>(nBase_), 0.0f);
}

void InverseDct::recurse(float* data, int off, int n, int nBits, float* temp) {
  if (n < 2) return;

  const std::vector<float>& reciprocalCosTable =
      reciprocalCosTables_[static_cast<size_t>(nBits)];
  const int halfLen = n >> 1;

  temp[off] = data[off];
  temp[off + halfLen] = data[off + 1];
  for (int i = 1; i < halfLen; ++i) {
    const int iDoubled = i << 1;
    temp[off + i] = data[off + iDoubled];
    temp[off + i + halfLen] =
        data[off - 1 + iDoubled] + data[off + 1 + iDoubled];
  }

  recurse(temp, off, halfLen, nBits - 1, data);
  recurse(temp, off + halfLen, halfLen, nBits - 1, data);

  for (int i = 0; i < halfLen; ++i) {
    const float x = temp[off + i];
    const float y = temp[off + i + halfLen] * reciprocalCosTable[i];
    data[off + i] = x + y;
    data[off + n - 1 - i] = x - y;
  }
}

void InverseDct::inverse(float* data) {
  recurse(data, 0, nBase_, nBits_, temp_.data());
}

}  // namespace mabink
