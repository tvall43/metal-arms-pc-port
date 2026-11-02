/* Bink Audio decoder (DCT variant), ported from unbikit. */

#include "bink_audio.h"

#include <algorithm>
#include <cmath>

#include "bink_tables.h"

namespace mabink {

AudioDecoder::AudioDecoder(int sampleRate, int numChannels, bool useDCT)
    : sampleRate_(sampleRate),
      numChannels_(numChannels),
      numInternalChannels_(numChannels),
      useDCT_(useDCT) {
  if (!useDCT_ || numChannels_ < 1 || numChannels_ > 2) {
    /* Metal Arms only uses stereo DCT audio at 22.05/44.1 kHz. */
    return;
  }

  if (sampleRate_ < 22050) {
    frameLenBits_ = 9;
  } else if (sampleRate_ < 44100) {
    frameLenBits_ = 10;
  } else {
    frameLenBits_ = 11;
  }

  frameLen_ = 1 << frameLenBits_;
  overlapLen_ = frameLen_ >> 4;
  blockSize_ = (frameLen_ - overlapLen_) * numInternalChannels_;
  sampleRateHalf_ = (sampleRate_ + 1) >> 1;
  baseQuant_ = static_cast<float>(static_cast<double>(frameLen_) /
                                  (std::sqrt(static_cast<double>(frameLen_)) *
                                   32768.0));

  const double expMultiplier = 0.0664 / std::log10(2.71828182845904523536);
  quantTable_.resize(96);
  for (int i = 0; i < 96; ++i) {
    quantTable_[static_cast<size_t>(i)] =
        static_cast<float>(std::exp(i * expMultiplier) * baseQuant_);
  }

  const Tables& tables = Tables::get();
  const std::vector<int32_t>& critical = tables.audioCriticalFreqs;
  int bandCount = -1;
  for (size_t i = 0; i < critical.size(); ++i) {
    if (sampleRateHalf_ <= critical[i]) {
      bandCount = static_cast<int>(i);
      break;
    }
  }
  numBands_ = bandCount < 0 ? static_cast<int>(critical.size()) + 1
                            : bandCount + 1;

  bands_.assign(static_cast<size_t>(numBands_) + 1, 0);
  bands_[0] = 2;
  for (int i = 1; i < numBands_; ++i) {
    bands_[static_cast<size_t>(i)] = static_cast<uint32_t>(
        (critical[static_cast<size_t>(i - 1)] * frameLen_) / sampleRateHalf_) &
        ~1u;
  }
  bands_[static_cast<size_t>(numBands_)] = static_cast<uint32_t>(frameLen_);

  output_.assign(static_cast<size_t>(numInternalChannels_),
                 std::vector<float>(static_cast<size_t>(frameLen_), 0.0f));
  overlapWindows_.assign(static_cast<size_t>(numInternalChannels_),
                         std::vector<float>(static_cast<size_t>(overlapLen_),
                                            0.0f));
  idt_.reset(new InverseDct(frameLenBits_));
  supported_ = true;
}

void AudioDecoder::reset() {
  first_ = true;
  for (auto& window : overlapWindows_) {
    std::fill(window.begin(), window.end(), 0.0f);
  }
}

float AudioDecoder::readDequantFloat29(BitReader& reader) {
  const int power = static_cast<int>(reader.readBits(5));
  const int mantissa = static_cast<int>(reader.readBits(23));
  float value = static_cast<float>(mantissa) *
                std::ldexp(1.0f, power - 23);
  if (reader.readBit()) value = -value;
  return value * baseQuant_;
}

bool AudioDecoder::decodePacket(
    const uint8_t* data, size_t size,
    std::vector<std::vector<std::vector<float>>>& blocks, std::string* error) {
  blocks.clear();
  if (!supported_) {
    if (error) *error = "unsupported Bink audio variant";
    return false;
  }

  const Tables& tables = Tables::get();
  BitReader reader;
  reader.reset(data, size);

  while (reader.bitsLeft() > 0) {
    if (useDCT_) reader.skip(2);

    for (int ch = 0; ch < numInternalChannels_; ++ch) {
      std::vector<float>& coeffs = output_[static_cast<size_t>(ch)];

      /* First two coefficients are stored directly. */
      coeffs[0] = readDequantFloat29(reader);
      coeffs[1] = readDequantFloat29(reader);

      std::vector<float> quant(static_cast<size_t>(numBands_), 0.0f);
      for (int i = 0; i < numBands_; ++i) {
        const int index = std::min(static_cast<int>(reader.readBits(8)), 95);
        quant[static_cast<size_t>(i)] = quantTable_[static_cast<size_t>(index)];
      }

      int k = 0;
      float q = quant[0];
      int i = 2;
      while (i < frameLen_) {
        int j;
        if (reader.readBit()) {
          const int v = static_cast<int>(reader.readBits(4));
          j = i + tables.audioRleLengths[static_cast<size_t>(v)];
        } else {
          j = i + 8;
        }
        j = std::min(j, frameLen_);

        const int width = static_cast<int>(reader.readBits(4));
        if (width == 0) {
          std::fill(coeffs.begin() + i, coeffs.begin() + j, 0.0f);
          i = j;
          while (k < numBands_ &&
                 static_cast<int>(bands_[static_cast<size_t>(k)]) < i) {
            q = quant[static_cast<size_t>(k)];
            ++k;
          }
        } else {
          while (i < j) {
            if (k < numBands_ &&
                static_cast<int>(bands_[static_cast<size_t>(k)]) == i) {
              q = quant[static_cast<size_t>(k)];
              ++k;
            }
            const int coeff = static_cast<int>(reader.readBits(width));
            if (coeff) {
              const int sign = static_cast<int>(reader.readBit());
              coeffs[static_cast<size_t>(i)] = sign ? -q * coeff : q * coeff;
            } else {
              coeffs[static_cast<size_t>(i)] = 0.0f;
            }
            ++i;
          }
        }
      }

      idt_->inverse(coeffs.data());
      if (useDCT_) {
        /* unbikit scales with 4*baseQuant, but its IDCT is unnormalised:
         * output grows with frameLen^1.5, so 1024-sample (22.05 kHz) files
         * came out 2x too loud and 2048-sample (44.1 kHz) files 4*sqrt(2)
         * too loud versus FFmpeg/RAD. Anchor at 1024 and remove both the
         * constant and the length-dependent error. */
        const float sizeScale = 1024.0f / static_cast<float>(frameLen_);
        const float modifier =
            2.0f * baseQuant_ * sizeScale * std::sqrt(sizeScale);
        for (float& value : coeffs) value *= modifier;
      }
    }

    /* Cross-fade the end of the previous block into this one. */
    for (int ch = 0; ch < numInternalChannels_; ++ch) {
      std::vector<float>& coeffs = output_[static_cast<size_t>(ch)];
      std::vector<float>& window = overlapWindows_[static_cast<size_t>(ch)];
      if (!first_) {
        const int count = overlapLen_ * numInternalChannels_;
        for (int i = 0, j = ch; i < overlapLen_; ++i, j += numInternalChannels_) {
          coeffs[static_cast<size_t>(i)] =
              (window[static_cast<size_t>(i)] * (count - j) +
               coeffs[static_cast<size_t>(i)] * j) /
              count;
        }
      }
      std::copy(coeffs.end() - overlapLen_, coeffs.end(), window.begin());
    }

    const int stride =
        (numChannels_ + numInternalChannels_ - 1) / numInternalChannels_;
    if (stride > 1) {
      if (error) *error = "interleaved Bink audio is not supported";
      return false;
    }

    const int actualSize = blockSize_ / numInternalChannels_;
    std::vector<std::vector<float>> block(
        static_cast<size_t>(numInternalChannels_));
    for (int ch = 0; ch < numInternalChannels_; ++ch) {
      const std::vector<float>& coeffs = output_[static_cast<size_t>(ch)];
      block[static_cast<size_t>(ch)].assign(coeffs.begin(),
                                            coeffs.begin() + actualSize);
    }
    blocks.push_back(std::move(block));

    first_ = false;
    reader.align32();
  }

  return true;
}

}  // namespace mabink
