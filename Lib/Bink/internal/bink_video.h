/* Bink 1 video decoder, ported from unbikit's src/bik-video-decoder.ts. */
#ifndef MABINK_VIDEO_H
#define MABINK_VIDEO_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "bink_bits.h"
#include "bink_tables.h"

namespace mabink {

struct VideoFrame {
  int width = 0;
  int height = 0;
  /* Planes are packed Y, U, V, (A) with the same layout as the reference. */
  std::vector<uint8_t> yuv;
  int lineSize[4] = {0, 0, 0, 0};
};

class VideoDecoder {
 public:
  VideoDecoder(int width, int height, int version, bool hasAlpha,
               bool hasSwappedUVPlanes);

  /* Decode one frame of video bitstream data into `out`. */
  void decode(const uint8_t* data, size_t size, VideoFrame& out);

  int width() const { return width_; }
  int height() const { return height_; }
  size_t frameSize() const { return frameSize_; }

 private:
  struct BlockParamValues {
    int len = 0;
    Tree tree;
    std::vector<int16_t> items;
    int curDec = 0;
    int curPtr = 0;
  };

  void decodePlane(int planeIndex);

  void readTree(Tree& tree);
  void mergeTreeData(uint8_t* dest, const uint8_t* src, int src1Index,
                     int size1);
  void readPlaneTrees(int blockWidth);
  int readCodedDataCount(BlockParamValues& param);
  void readBlockTypes(BlockParamValues& param);
  void readColors(BlockParamValues& param);
  void readPatterns(BlockParamValues& param);
  void readMotionValues(BlockParamValues& param);
  void readDCs(BlockParamValues& param, bool hasSign);
  void readRuns(BlockParamValues& param);
  int getValue(int param);

  void copyBlock(int64_t srcOffset);
  void decodeMotionBlock();
  void decodeRunBlock(uint8_t* base, size_t baseSize, int64_t offset,
                      int stride);
  void decodeResidueBlock();
  void decodeIntraBlock(uint8_t* base, size_t baseSize, int64_t offset,
                        int stride);
  void decodeFillBlock(int size);
  void decodeInterBlock();
  void decodePatternBlock(uint8_t* base, size_t baseSize, int64_t offset,
                          int stride);
  void decodeRawBlock();
  void decodeScaledBlock();
  void readCoeffsOrResidue(int32_t* block, int quantStartIndex);

  int width_;
  int height_;
  int version_;
  bool hasAlpha_;
  bool hasSwappedUVPlanes_;

  size_t numPixels_ = 0;
  size_t uvSize_ = 0;
  size_t ySize_ = 0;
  size_t frameSize_ = 0;
  int yStride_ = 0;
  int uvStride_ = 0;
  int uvHeight_ = 0;
  int yAllocHeight_ = 0;
  int uvAllocHeight_ = 0;

  std::vector<uint8_t> prev_;
  std::vector<uint8_t> cur_;
  uint8_t* plane_ = nullptr;
  const uint8_t* prevPlane_ = nullptr;
  size_t planeSize_ = 0;
  int64_t planePtr_ = 0;
  int stride_ = 0;

  BitReader reader_;

  BlockParamValues blockParams_[kNumBlockParams];
  Tree colHigh_[16];
  int colLastValue_ = 0;

  /* Scratch buffers reused across blocks/frames. */
  uint8_t tmpScalingBuf_[64] = {};
  int32_t tmpDCTBuf_[64] = {};
  int32_t coeffIndex_[64] = {};
  int32_t coeffList_[128] = {};
  uint8_t modeList_[128] = {};
  uint8_t inputTreeBuf_[16] = {};
  uint8_t outputTreeBuf_[16] = {};
};

}  // namespace mabink

#endif
