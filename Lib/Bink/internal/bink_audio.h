/* Bink Audio decoder (DCT variant).
 *
 * Ported from unbikit's src/bik-audio-decoder.ts. Only the DCT variant used by
 * Metal Arms' movies is implemented; the older interleaved RDFT variant is
 * reported as unsupported instead of being decoded incorrectly.
 */
#ifndef MABINK_AUDIO_H
#define MABINK_AUDIO_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "bink_bits.h"
#include "bink_dct.h"

namespace mabink {

class AudioDecoder {
 public:
  AudioDecoder(int sampleRate, int numChannels, bool useDCT);

  bool supported() const { return supported_; }
  int sampleRate() const { return sampleRate_; }
  int numChannels() const { return numChannels_; }

  void reset();

  /* Decode one audio packet. Returns blocks of planar float samples:
   * blocks[block][channel][sample]. */
  bool decodePacket(const uint8_t* data, size_t size,
                    std::vector<std::vector<std::vector<float>>>& blocks,
                    std::string* error);

 private:
  float readDequantFloat29(BitReader& reader);

  int sampleRate_;
  int numChannels_;
  int numInternalChannels_;
  bool useDCT_;
  bool supported_ = false;

  int frameLenBits_ = 0;
  int frameLen_ = 0;
  int overlapLen_ = 0;
  int blockSize_ = 0;
  int sampleRateHalf_ = 0;
  float baseQuant_ = 0.0f;
  int numBands_ = 0;

  std::vector<float> quantTable_;
  std::vector<uint32_t> bands_;
  std::vector<std::vector<float>> output_;
  std::vector<std::vector<float>> overlapWindows_;
  std::unique_ptr<InverseDct> idt_;
  bool first_ = true;
};

}  // namespace mabink

#endif
