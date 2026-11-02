/* Bink bit-stream reader.
 *
 * Mirrors the reader in unbikit's src/bik-decoder-utils.ts, including the
 * "reads past the end yield zero" behaviour that malformed/truncated frames
 * rely on. Bits are consumed least-significant-bit first within each byte.
 */
#ifndef MABINK_BITS_H
#define MABINK_BITS_H

#include <cstddef>
#include <cstdint>

namespace mabink {

class BitReader {
 public:
  void reset(const uint8_t* data, size_t size) {
    data_ = data;
    size_ = size;
    bitPos_ = 0;
  }

  void reset() {
    data_ = nullptr;
    size_ = 0;
    bitPos_ = 0;
  }

  uint32_t readBits(int n, bool peek = false) {
    uint32_t result = 0;
    size_t bitPos = bitPos_;
    int remaining = n;
    do {
      const uint32_t mask = (remaining >= 32) ? 0xffffffffu
                                               : ((1u << remaining) - 1u);
      const uint32_t byte = byteAt(bitPos);
      const uint32_t bits = (byte >> (bitPos & 7)) & mask;
      result |= bits << (n - remaining);
      if (static_cast<int>(bitPos & 7) + remaining > 7) {
        const int posChange = 8 - static_cast<int>(bitPos & 7);
        remaining -= posChange;
        bitPos += static_cast<size_t>(posChange);
      } else {
        bitPos += static_cast<size_t>(remaining);
        break;
      }
    } while (remaining);
    if (!peek) bitPos_ = bitPos;
    return result;
  }

  uint32_t readBit() {
    const uint32_t bit = (byteAt(bitPos_) >> (bitPos_ & 7)) & 1u;
    ++bitPos_;
    return bit;
  }

  /* Returns +v when the next bit is 0, -v when it is 1. */
  int applySign(int v) { return readBit() ? -v : v; }

  void skip(size_t n) {
    if (n) bitPos_ += n;
  }

  void align32() {
    const size_t n = (32 - (bitPos_ & 31)) & 31;
    skip(n);
  }

  size_t bitsLeft() const {
    const size_t total = size_ << 3;
    return bitPos_ >= total ? 0 : total - bitPos_;
  }

 private:
  uint32_t byteAt(size_t bitPos) const {
    const size_t index = bitPos >> 3;
    return index < size_ ? data_[index] : 0u;
  }

  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
  size_t bitPos_ = 0;
};

}  // namespace mabink

#endif
