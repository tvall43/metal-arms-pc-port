/* Inverse DCT used by Bink Audio (DCT variant).
 *
 * Ported from the DCT branch of unbikit's genIDxT() in src/transforms.ts.
 * The recursive Lee algorithm is kept as-is; it is only run on 512-2048
 * sample blocks and is not a hot path compared with video.
 */
#ifndef MABINK_DCT_H
#define MABINK_DCT_H

#include <vector>

namespace mabink {

class InverseDct {
 public:
  explicit InverseDct(int nBits);

  /* Transform `data` in place. `data` must contain at least 1 << nBits
   * samples. */
  void inverse(float* data);

 private:
  void recurse(float* data, int off, int n, int nBits, float* temp);

  int nBase_;
  int nBits_;
  std::vector<std::vector<float>> reciprocalCosTables_;
  std::vector<float> temp_;
};

}  // namespace mabink

#endif
