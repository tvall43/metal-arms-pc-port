/* Bink 1 video decoder, ported from unbikit's src/bik-video-decoder.ts.
 *
 * The structure deliberately follows the reference: a bit reader, a set of
 * per-block-parameter value lists, a small coefficient "VM", and an integer
 * IDCT. Diverging from that structure makes bit-exact comparison against the
 * reference much harder, so keep the shape when editing.
 */

#include "bink_video.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "bink_idct.h"

namespace mabink {
namespace {

inline uint8_t planeRead(const uint8_t* base, size_t size, int64_t index) {
  if (index < 0 || static_cast<uint64_t>(index) >= size) return 0;
  return base[index];
}

inline void planeWrite(uint8_t* base, size_t size, int64_t index,
                       uint8_t value) {
  if (index < 0 || static_cast<uint64_t>(index) >= size) return;
  base[index] = value;
}

inline int signExtendByte(int value) {
  return static_cast<int>(static_cast<int8_t>(value & 0xff));
}

}  // namespace

VideoDecoder::VideoDecoder(int width, int height, int version, bool hasAlpha,
                           bool hasSwappedUVPlanes)
    : width_(width),
      height_(height),
      version_(version),
      hasAlpha_(hasAlpha),
      hasSwappedUVPlanes_(hasSwappedUVPlanes) {
  numPixels_ = static_cast<size_t>(width_) * static_cast<size_t>(height_);

  /* Block writes can reach up to 16 pixels past the visible row (scaled
   * blocks) and one extra column beyond that. A padded stride keeps those
   * writes out of the next visible row; FFmpeg and RAD both do the same.
   * 32-pixel alignment plus 16 bytes of slack covers every block type. */
  auto alignUp = [](int value, int alignment) {
    return (value + alignment - 1) / alignment * alignment;
  };
  yStride_ = alignUp(width_ + 16, 32);
  uvStride_ = alignUp(((width_ + 1) >> 1) + 16, 32);
  uvHeight_ = (height_ + 1) >> 1;
  /* Keep the rows produced by the final block row. Motion vectors near the
   * bottom edge reference them, so dropping them changes the image. */
  yAllocHeight_ = alignUp(height_, 8);
  uvAllocHeight_ = alignUp(uvHeight_, 8);
  ySize_ = static_cast<size_t>(yStride_) * static_cast<size_t>(yAllocHeight_);
  uvSize_ =
      static_cast<size_t>(uvStride_) * static_cast<size_t>(uvAllocHeight_);
  frameSize_ = (ySize_ << (hasAlpha_ ? 1 : 0)) + (uvSize_ << 1);
  prev_.assign(frameSize_, 0);
  cur_.assign(frameSize_, 0);

  const Tables& tables = Tables::get();
  const size_t blocks = (numPixels_ + 63) >> 6;
  const size_t numBlockPixels = blocks << 6;
  for (auto& param : blockParams_) {
    param.len = 0;
    param.tree.table = &tables.huff[0];
    std::memset(param.tree.symbolMap, 0, sizeof(param.tree.symbolMap));
    param.items.assign(numBlockPixels, 0);
    param.curDec = 0;
    param.curPtr = 0;
  }
  for (auto& tree : colHigh_) {
    tree.table = &tables.huff[0];
    std::memset(tree.symbolMap, 0, sizeof(tree.symbolMap));
  }
}

void VideoDecoder::decode(const uint8_t* data, size_t size, VideoFrame& out) {
  /* The current frame starts as a copy of the previous frame. Skip blocks
   * then already contain the right pixels. */
  cur_.assign(prev_.begin(), prev_.end());

  reader_.reset(data, size);

  if (hasAlpha_) {
    if (version_ > 104) reader_.skip(32);
    decodePlane(3);
  }

  if (version_ > 104) reader_.skip(32);

  for (int plane = 0; plane < 3; ++plane) {
    const int planeIndex = (plane && hasSwappedUVPlanes_) ? (plane ^ 3) : plane;
    decodePlane(planeIndex);
    if (reader_.bitsLeft() < 1) break;
  }

  /* Publish a tightly packed Y/U/V/(A) frame. Consumers get the visible
   * width as the line size, not the internal padded stride. */
  const int chromaWidth = (width_ + 1) >> 1;
  out.width = width_;
  out.height = height_;
  out.lineSize[0] = width_;
  out.lineSize[1] = chromaWidth;
  out.lineSize[2] = chromaWidth;
  out.lineSize[3] = width_;

  const size_t yPixels =
      static_cast<size_t>(width_) * static_cast<size_t>(height_);
  const size_t uvPixels =
      static_cast<size_t>(chromaWidth) * static_cast<size_t>(uvHeight_);
  out.yuv.resize((yPixels << (hasAlpha_ ? 1 : 0)) + (uvPixels << 1));

  size_t dest = 0;
  auto packPlane = [&](const uint8_t* src, int srcStride, int visibleWidth,
                       int visibleHeight) {
    for (int y = 0; y < visibleHeight; ++y) {
      std::memcpy(out.yuv.data() + dest, src + static_cast<size_t>(y) * srcStride,
                  static_cast<size_t>(visibleWidth));
      dest += static_cast<size_t>(visibleWidth);
    }
  };

  packPlane(cur_.data(), yStride_, width_, height_);
  packPlane(cur_.data() + ySize_, uvStride_, chromaWidth, uvHeight_);
  packPlane(cur_.data() + ySize_ + uvSize_, uvStride_, chromaWidth,
            uvHeight_);
  if (hasAlpha_) {
    packPlane(cur_.data() + ySize_ + (uvSize_ << 1), yStride_, width_,
              height_);
  }

  prev_.swap(cur_);
}

void VideoDecoder::decodePlane(int planeIndex) {
  const bool isChroma = planeIndex == 1 || planeIndex == 2;
  const int blockWidth =
      isChroma ? (width_ + 15) / 16 : (width_ + 7) / 8;
  const int blockHeight =
      isChroma ? (height_ + 15) / 16 : (height_ + 7) / 8;

  readPlaneTrees(blockWidth);

  stride_ = isChroma ? uvStride_ : yStride_;
  const size_t planeOffset =
      planeIndex == 0 ? 0
      : planeIndex == 1 ? ySize_
      : planeIndex == 2 ? ySize_ + uvSize_
                        : ySize_ + uvSize_ + uvSize_;
  plane_ = cur_.data() + planeOffset;
  prevPlane_ = prev_.data() + planeOffset;
  planeSize_ = isChroma ? uvSize_ : ySize_;
  planePtr_ = 0;

  int64_t rowStart = 0;

  int blockYPos = 0;
  while (blockYPos++ < blockHeight) {
    readBlockTypes(blockParams_[kBikParamBlockTypes]);
    readBlockTypes(blockParams_[kBikParamSubBlockTypes]);
    readColors(blockParams_[kBikParamColors]);
    readPatterns(blockParams_[kBikParamPattern]);
    readMotionValues(blockParams_[kBikParamXOff]);
    readMotionValues(blockParams_[kBikParamYOff]);
    readDCs(blockParams_[kBikParamIntraDC], false);
    readDCs(blockParams_[kBikParamInterDC], true);
    readRuns(blockParams_[kBikParamRun]);

    /* Start each block row at an exact 8-line boundary. The reference adds
     * stride*7 after a row and therefore silently assumes the coded width is
     * a multiple of 8; keeping an explicit row origin fixes odd widths. */
    planePtr_ = rowStart;

    int blockXPos = 0;
    while (blockXPos++ < blockWidth) {
      const int blockType = getValue(kBikParamBlockTypes);

      switch (blockType) {
        case kBikBlockSkip:
          break;
        case kBikBlockScaled:
          if (blockYPos & 1) decodeScaledBlock();
          ++blockXPos;
          planePtr_ += 16;
          continue;
        case kBikBlockMotion:
          decodeMotionBlock();
          break;
        case kBikBlockRun:
          decodeRunBlock(plane_, planeSize_, planePtr_, stride_);
          break;
        case kBikBlockResidue:
          decodeResidueBlock();
          break;
        case kBikBlockIntra:
          decodeIntraBlock(plane_, planeSize_, planePtr_, stride_);
          break;
        case kBikBlockFill:
          decodeFillBlock(8);
          break;
        case kBikBlockInter:
          decodeInterBlock();
          break;
        case kBikBlockPattern:
          decodePatternBlock(plane_, planeSize_, planePtr_, stride_);
          break;
        case kBikBlockRaw:
          decodeRawBlock();
          break;
        default:
          throw std::runtime_error("invalid Bink block type");
      }

      planePtr_ += 8;
    }

    rowStart += static_cast<int64_t>(stride_) * 8;
  }

  reader_.align32();
}

void VideoDecoder::readTree(Tree& tree) {
  const Tables& tables = Tables::get();
  const int tableNum = static_cast<int>(reader_.readBits(4));
  tree.table = &tables.huff[tableNum & 15];

  if (!tableNum) {
    for (int i = 0; i < 16; ++i) tree.symbolMap[i] = static_cast<uint8_t>(i);
    return;
  }

  if (reader_.readBit()) {
    int len = static_cast<int>(reader_.readBits(3));
    uint8_t seen[16] = {};
    for (int i = 0; i <= len; ++i) {
      tree.symbolMap[i] = static_cast<uint8_t>(reader_.readBits(4));
      seen[tree.symbolMap[i]] = 1;
    }
    for (int i = 0; i < 16; ++i) {
      if (!seen[i]) tree.symbolMap[++len] = static_cast<uint8_t>(i);
    }
  } else {
    const int len = static_cast<int>(reader_.readBits(2));
    uint8_t* input = inputTreeBuf_;
    uint8_t* output = outputTreeBuf_;
    for (int i = 0; i < 16; i += 2) {
      const int bit = static_cast<int>(reader_.readBit());
      input[i] = static_cast<uint8_t>(i + bit);
      input[i + 1] = static_cast<uint8_t>(i + (bit ^ 1));
    }
    for (int i = 1; i <= len; ++i) {
      const int size = 1 << i;
      for (int t = 0; t < 16; t += size << 1) {
        mergeTreeData(output, input, t, size);
      }
      std::swap(input, output);
    }
    std::memcpy(tree.symbolMap, input, 16);
  }
}

void VideoDecoder::mergeTreeData(uint8_t* dest, const uint8_t* src,
                                 int src1Index, int size1) {
  int src2Index = src1Index + size1;
  int size2 = size1;
  int destIndex = src1Index;

  while (size1 && size2) {
    if (reader_.readBit()) {
      dest[destIndex++] = src[src2Index++];
      --size2;
    } else {
      dest[destIndex++] = src[src1Index++];
      --size1;
    }
  }
  while (size1--) dest[destIndex++] = src[src1Index++];
  while (size2--) dest[destIndex++] = src[src2Index++];
}

void VideoDecoder::readPlaneTrees(int blockWidth) {
  const int extraLen = 511;
  const int commonLen = blockWidth + extraLen;
  const int lengths[kNumBlockParams] = {
      commonLen,
      (blockWidth >> 1) + extraLen,
      (blockWidth << 6) + extraLen,
      (blockWidth << 3) + extraLen,
      commonLen,
      commonLen,
      commonLen,
      commonLen,
      blockWidth * 48 + extraLen,
  };

  for (int paramNum = 0; paramNum < kNumBlockParams; ++paramNum) {
    BlockParamValues& param = blockParams_[paramNum];
    /* floor(log2(n)) + 1, matching ~~Math.log2(n) + 1. */
    param.len = 32 - __builtin_clz(static_cast<unsigned>(lengths[paramNum]));

    if (paramNum == kBikParamColors) {
      for (auto& tree : colHigh_) readTree(tree);
      colLastValue_ = 0;
    }

    if (paramNum < kBikParamIntraDC || paramNum > kBikParamInterDC) {
      readTree(param.tree);
    }

    param.curDec = 0;
    param.curPtr = 0;
  }
}

int VideoDecoder::readCodedDataCount(BlockParamValues& param) {
  if (param.curDec < 0 || param.curDec > param.curPtr) return 0;
  const int count = static_cast<int>(reader_.readBits(param.len));
  if (count == 0) param.curDec = -1;
  return count;
}

void VideoDecoder::readBlockTypes(BlockParamValues& param) {
  const int count = readCodedDataCount(param);
  if (!count) return;

  if (reader_.readBit()) {
    const int value = static_cast<int>(reader_.readBits(4));
    for (int i = 0; i < count; ++i) param.items[param.curDec++] = value;
    return;
  }

  int prevValue = 0;
  for (int i = 0; i < count; ++i) {
    const int value = getHuff(reader_, param.tree);
    if (value < 12) {
      prevValue = value;
      param.items[param.curDec++] = static_cast<int16_t>(value);
    } else {
      static const int runLengths[4] = {4, 8, 12, 32};
      const int runLength = runLengths[value - 12];
      for (int j = 0; j < runLength; ++j) {
        param.items[param.curDec++] = static_cast<int16_t>(prevValue);
      }
      i += runLength - 1;
    }
  }
}

void VideoDecoder::readColors(BlockParamValues& param) {
  const int count = readCodedDataCount(param);
  if (!count) return;

  const bool isRun = reader_.readBit() != 0;
  int loopCount = isRun ? 1 : count;
  do {
    const int high = getHuff(reader_, colHigh_[colLastValue_]);
    const int low = getHuff(reader_, param.tree);
    int value = low | (high << 4);
    colLastValue_ = high;

    if (version_ < 105) {
      value = (value > 127) ? (256 - value) : (value + 128);
    }

    if (isRun) {
      for (int i = 0; i < count; ++i) {
        param.items[param.curDec++] = static_cast<int16_t>(value);
      }
    } else {
      param.items[param.curDec++] = static_cast<int16_t>(value);
    }
  } while (--loopCount);
}

void VideoDecoder::readPatterns(BlockParamValues& param) {
  const int count = readCodedDataCount(param);
  if (!count) return;

  for (int i = 0; i < count; ++i) {
    const int low = getHuff(reader_, param.tree);
    const int high = getHuff(reader_, param.tree);
    param.items[param.curDec++] =
        static_cast<int16_t>(low | (high << 4));
  }
}

void VideoDecoder::readMotionValues(BlockParamValues& param) {
  const int count = readCodedDataCount(param);
  if (!count) return;

  const int maxDec = param.curDec + count;
  if (reader_.readBit()) {
    int value = static_cast<int>(reader_.readBits(4));
    if (value) value = reader_.applySign(value);
    value = signExtendByte(value);
    for (; param.curDec < maxDec; ++param.curDec) {
      param.items[param.curDec] = static_cast<int16_t>(value);
    }
    return;
  }

  while (param.curDec < maxDec) {
    int value = getHuff(reader_, param.tree);
    if (value) value = reader_.applySign(value);
    value = signExtendByte(value);
    param.items[param.curDec++] = static_cast<int16_t>(value);
  }
}

void VideoDecoder::readDCs(BlockParamValues& param, bool hasSign) {
  const int count = readCodedDataCount(param);
  if (!count) return;

  int value = static_cast<int>(reader_.readBits(hasSign ? 10 : 11));
  if (value && hasSign) value = reader_.applySign(value);
  param.items[param.curDec++] = static_cast<int16_t>(value);

  int i = 1;
  while (i < count) {
    const int len = std::min(count - i, 8);
    const int v2Size = static_cast<int>(reader_.readBits(4));
    if (v2Size) {
      for (int j = 0; j < len; ++j) {
        int v2 = static_cast<int>(reader_.readBits(v2Size));
        if (v2) v2 = reader_.applySign(v2);
        value += v2;
        param.items[param.curDec++] = static_cast<int16_t>(value);
      }
    } else {
      for (int j = 0; j < len; ++j) {
        param.items[param.curDec++] = static_cast<int16_t>(value);
      }
    }
    i += len;
  }
}

void VideoDecoder::readRuns(BlockParamValues& param) {
  const int count = readCodedDataCount(param);
  if (!count) return;

  if (reader_.readBit()) {
    const int value = static_cast<int>(reader_.readBits(4));
    for (int i = 0; i < count; ++i) {
      param.items[param.curDec++] = static_cast<int16_t>(value);
    }
    return;
  }

  for (int i = 0; i < count; ++i) {
    param.items[param.curDec++] =
        static_cast<int16_t>(getHuff(reader_, param.tree));
  }
}

int VideoDecoder::getValue(int paramNum) {
  BlockParamValues& param = blockParams_[paramNum];
  const int value =
      param.curPtr < static_cast<int>(param.items.size())
          ? param.items[param.curPtr]
          : 0;
  ++param.curPtr;
  return value;
}

void VideoDecoder::copyBlock(int64_t srcOffset) {
  const int64_t indexDiff = planePtr_ - srcOffset;
  if (!indexDiff) return;

  const int strideMinusBlock = stride_ - 8;
  int lineCount = 8;
  int64_t src = srcOffset;
  while (lineCount--) {
    const int64_t srcEnd = src + 8;
    while (src < srcEnd) {
      planeWrite(plane_, planeSize_, src + indexDiff,
                 planeRead(prevPlane_, planeSize_, src));
      ++src;
    }
    src += strideMinusBlock;
  }
}

void VideoDecoder::decodeMotionBlock() {
  const int xOffset = getValue(kBikParamXOff);
  const int yOffset = getValue(kBikParamYOff);
  copyBlock(planePtr_ + xOffset + static_cast<int64_t>(yOffset) * stride_);
}

void VideoDecoder::decodeRunBlock(uint8_t* base, size_t baseSize,
                                  int64_t offset, int stride) {
  const Tables& tables = Tables::get();
  int i = 0;
  int scanIndex = static_cast<int>(reader_.readBits(4)) << 6;

  do {
    const int run = getValue(kBikParamRun) + 1;
    i += run;

    if (reader_.readBit()) {
      const uint8_t value = static_cast<uint8_t>(getValue(kBikParamColors));
      for (int j = 0; j < run; ++j) {
        const int pos = scanIndex < static_cast<int>(tables.patterns.size())
                            ? tables.patterns[scanIndex]
                            : 0;
        ++scanIndex;
        planeWrite(base, baseSize,
                   offset + static_cast<int64_t>(pos >> 3) * stride +
                       (pos & 7),
                   value);
      }
    } else {
      for (int j = 0; j < run; ++j) {
        const int pos = scanIndex < static_cast<int>(tables.patterns.size())
                            ? tables.patterns[scanIndex]
                            : 0;
        ++scanIndex;
        planeWrite(base, baseSize,
                   offset + static_cast<int64_t>(pos >> 3) * stride +
                       (pos & 7),
                   static_cast<uint8_t>(getValue(kBikParamColors)));
      }
    }
  } while (i < 63);

  if (i == 63) {
    const int pos = scanIndex < static_cast<int>(tables.patterns.size())
                        ? tables.patterns[scanIndex]
                        : 0;
    planeWrite(base, baseSize,
               offset + static_cast<int64_t>(pos >> 3) * stride + (pos & 7),
               static_cast<uint8_t>(getValue(kBikParamColors)));
  }
}

void VideoDecoder::decodeResidueBlock() {
  const int xOffset = getValue(kBikParamXOff);
  const int yOffset = getValue(kBikParamYOff);
  const int64_t srcPos =
      planePtr_ + xOffset + static_cast<int64_t>(yOffset) * stride_;

  std::memset(tmpDCTBuf_, 0, sizeof(tmpDCTBuf_));
  readCoeffsOrResidue(tmpDCTBuf_, -1);

  copyBlock(srcPos);
  addBlock8x8(tmpDCTBuf_, plane_, planeSize_, planePtr_, stride_);
}

void VideoDecoder::decodeIntraBlock(uint8_t* base, size_t baseSize,
                                    int64_t offset, int stride) {
  tmpDCTBuf_[0] = getValue(kBikParamIntraDC);
  std::memset(tmpDCTBuf_ + 1, 0, sizeof(tmpDCTBuf_) - sizeof(tmpDCTBuf_[0]));
  readCoeffsOrResidue(tmpDCTBuf_, 0);
  idctPut(tmpDCTBuf_, base, baseSize, offset, stride);
}

void VideoDecoder::decodeFillBlock(int size) {
  const uint8_t value = static_cast<uint8_t>(getValue(kBikParamColors));
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      planeWrite(plane_, planeSize_,
                 planePtr_ + static_cast<int64_t>(y) * stride_ + x, value);
    }
  }
}

void VideoDecoder::decodeInterBlock() {
  const int xOffset = getValue(kBikParamXOff);
  const int yOffset = getValue(kBikParamYOff);
  const int64_t ref =
      planePtr_ + xOffset + static_cast<int64_t>(yOffset) * stride_;
  copyBlock(ref);

  tmpDCTBuf_[0] = getValue(kBikParamInterDC);
  std::memset(tmpDCTBuf_ + 1, 0, sizeof(tmpDCTBuf_) - sizeof(tmpDCTBuf_[0]));
  readCoeffsOrResidue(tmpDCTBuf_, 1024);
  idctAdd(tmpDCTBuf_, plane_, planeSize_, planePtr_, stride_);
}

void VideoDecoder::decodePatternBlock(uint8_t* base, size_t baseSize,
                                      int64_t offset, int stride) {
  const uint8_t colors[2] = {
      static_cast<uint8_t>(getValue(kBikParamColors)),
      static_cast<uint8_t>(getValue(kBikParamColors)),
  };
  for (int i = 0; i < 8; ++i) {
    int value = getValue(kBikParamPattern);
    for (int j = 0; j < 8; ++j) {
      planeWrite(base, baseSize,
                 offset + static_cast<int64_t>(i) * stride + j,
                 colors[value & 1]);
      value >>= 1;
    }
  }
}

void VideoDecoder::decodeRawBlock() {
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      planeWrite(plane_, planeSize_,
                 planePtr_ + static_cast<int64_t>(y) * stride_ + x,
                 static_cast<uint8_t>(getValue(kBikParamColors)));
    }
  }
}

void VideoDecoder::decodeScaledBlock() {
  const int subBlock = getValue(kBikParamSubBlockTypes);

  switch (subBlock) {
    case kBikBlockRaw:
      for (int i = 0; i < 64; ++i) {
        tmpScalingBuf_[i] =
            static_cast<uint8_t>(getValue(kBikParamColors));
      }
      break;
    case kBikBlockIntra:
      decodeIntraBlock(tmpScalingBuf_, sizeof(tmpScalingBuf_), 0, 8);
      break;
    case kBikBlockFill:
      decodeFillBlock(16);
      return;
    case kBikBlockRun:
      decodeRunBlock(tmpScalingBuf_, sizeof(tmpScalingBuf_), 0, 8);
      break;
    case kBikBlockPattern:
      decodePatternBlock(tmpScalingBuf_, sizeof(tmpScalingBuf_), 0, 8);
      break;
    default:
      throw std::runtime_error("invalid Bink sub-block type");
  }

  int64_t destPosLine = planePtr_;
  const int64_t maxDestPosLine =
      destPosLine + (static_cast<int64_t>(stride_) << 4);
  const int lineIncrement = (stride_ << 1) - 15;
  int srcPos = 0;

  while (destPosLine < maxDestPosLine) {
    const uint8_t value = tmpScalingBuf_[srcPos++];
    planeWrite(plane_, planeSize_, destPosLine, value);
    planeWrite(plane_, planeSize_, stride_ + destPosLine, value);
    ++destPosLine;
    planeWrite(plane_, planeSize_, destPosLine, value);
    planeWrite(plane_, planeSize_, stride_ + destPosLine, value);
    destPosLine += (srcPos & 7) ? 1 : lineIncrement;
  }
}

void VideoDecoder::readCoeffsOrResidue(int32_t* block, int quantStartIndex) {
  const Tables& tables = Tables::get();
  const bool isResidue = quantStartIndex < 0;
  int listStart = 64;
  int listEnd = 70;
  int masksCount = 0;
  int coeffCount = 0;

  /* The reference uses 128-entry typed arrays and relies on out-of-range
   * accesses reading as 0 / writes being ignored. Keep that behaviour. */
  auto coeffGet = [&](int index) -> int {
    return (index >= 0 && index < 128) ? coeffList_[index] : 0;
  };
  auto coeffSet = [&](int index, int value) {
    if (index >= 0 && index < 128) coeffList_[index] = value;
  };
  auto modeGet = [&](int index) -> int {
    return (index >= 0 && index < 128) ? modeList_[index] : 0;
  };
  auto modeSet = [&](int index, int value) {
    if (index >= 0 && index < 128) modeList_[index] = static_cast<uint8_t>(value);
  };
  auto indexGet = [&](int index) -> int {
    return (index >= 0 && index < 64) ? coeffIndex_[index] : 0;
  };
  auto indexSet = [&](int index, int value) {
    if (index >= 0 && index < 64) coeffIndex_[index] = value;
  };

  coeffList_[64] = 4;
  coeffList_[65] = 24;
  coeffList_[66] = 44;
  modeList_[64] = 0;
  modeList_[65] = 0;
  modeList_[66] = 0;
  if (isResidue) {
    listEnd = 68;
    masksCount = static_cast<int>(reader_.readBits(7));
    coeffList_[67] = 0;
    modeList_[67] = 2;
  } else {
    coeffList_[67] = 1;
    coeffList_[68] = 2;
    coeffList_[69] = 3;
    modeList_[67] = 3;
    modeList_[68] = 3;
    modeList_[69] = 3;
  }

  int bits = isResidue ? (1 << reader_.readBits(3))
                       : static_cast<int>(reader_.readBits(4)) - 1;

  while (isResidue ? (bits != 0) : (bits >= 0)) {
    if (isResidue) {
      for (int i = 0; i < coeffCount; ++i) {
        if (reader_.readBit()) {
          const int curNzCoeff = indexGet(i);
          const int value = block[curNzCoeff];
          block[curNzCoeff] = value < 0 ? value - bits : value + bits;
          if (!masksCount--) return;
        }
      }
    }

    int listPos = listStart;
    while (listPos < listEnd) {
      const int ccoeff = coeffGet(listPos);
      const int mode = modeGet(listPos);
      if (!(mode | ccoeff) || !reader_.readBit()) {
        ++listPos;
        continue;
      }

      switch (mode) {
        case 0:
        case 2: {
          if (mode == 0) {
            coeffSet(listPos, ccoeff + 4);
            modeSet(listPos, 1);
          } else {
            coeffSet(listPos, 0);
            modeSet(listPos, 0);
            ++listPos;
          }

          for (int i = ccoeff; i < ccoeff + 4; ++i) {
            if (reader_.readBit()) {
              --listStart;
              coeffSet(listStart, i);
              modeSet(listStart, 3);
            } else {
              if (isResidue) {
                const int offset = tables.scan[i];
                indexSet(coeffCount, offset);
                ++coeffCount;
                block[offset] = reader_.applySign(bits);
                if (!masksCount--) return;
              } else {
                const int offset =
                    bits ? reader_.applySign(
                               static_cast<int>(reader_.readBits(bits)) |
                               (1 << bits))
                         : 1 - (static_cast<int>(reader_.readBit()) << 1);
                block[tables.scan[i]] = offset;
                indexSet(coeffCount, i);
                ++coeffCount;
              }
            }
          }
          break;
        }

        case 1: {
          modeSet(listPos, 2);
          for (int i = ccoeff + 4; i < ccoeff + 16; i += 4) {
            coeffSet(listEnd, i);
            modeSet(listEnd, 2);
            ++listEnd;
          }
          break;
        }

        case 3: {
          coeffSet(listPos, 0);
          modeSet(listPos, 0);
          ++listPos;
          if (isResidue) {
            const int offset = tables.scan[ccoeff];
            indexSet(coeffCount, offset);
            ++coeffCount;
            block[offset] = reader_.applySign(bits);
            if (!masksCount--) return;
          } else {
            const int offset =
                bits ? reader_.applySign(
                           static_cast<int>(reader_.readBits(bits)) |
                           (1 << bits))
                     : 1 - (static_cast<int>(reader_.readBit()) << 1);
            block[tables.scan[ccoeff]] = offset;
            indexSet(coeffCount, ccoeff);
            ++coeffCount;
          }
          break;
        }
      }
    }

    bits = isResidue ? (bits >> 1) : (bits - 1);
  }

  if (!isResidue) {
    const int quantOffset =
        (static_cast<int>(reader_.readBits(4)) << 6) + quantStartIndex;
    block[0] = (block[0] * tables.quant[quantOffset]) >> 11;
    while (coeffCount--) {
      const int index = indexGet(coeffCount);
      const int blockIndex = tables.scan[index];
      block[blockIndex] =
          (block[blockIndex] * tables.quant[quantOffset + index]) >> 11;
    }
  }
}

}  // namespace mabink
