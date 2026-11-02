/* Bink 1 container parser and frame decoder (portable, no Windows ABI). */
#ifndef MABINK_DECODER_H
#define MABINK_DECODER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "bink_video.h"

namespace mabink {

struct AudioTrackHeader {
  uint32_t trackId = 0;
  int numChannels = 1;
  int sampleRate = 0;
  bool stereo = false;
  bool usesDCT = false;
};

struct FrameHeader {
  uint32_t offset = 0;
  uint32_t size = 0;
  bool keyframe = false;
};

struct AudioPacket {
  const uint8_t* data = nullptr;
  size_t size = 0;
  int numSamples = 0;
};

struct Header {
  int version = 0;
  int subVersion = 0;
  uint32_t totalSize = 0;
  uint32_t largestFrameSize = 0;
  int numFrames = 0;
  int width = 0;
  int height = 0;
  double fps = 0.0;
  bool hasAlpha = false;
  bool hasSwappedUVPlanes = false;
  bool isGrayscale = false;
  int scaling = 0;
  int numAudioTracks = 0;
  std::vector<AudioTrackHeader> audioTracks;
  std::vector<FrameHeader> frames;
};

class Decoder {
 public:
  Decoder();
  ~Decoder();

  bool openFile(const char* path, std::string* error);
  bool openMemory(const uint8_t* data, size_t size, std::string* error);

  const Header& header() const { return header_; }
  bool isSupported() const { return supported_; }
  int currentFrame() const { return curFrame_; }

  /* Decode the next frame in sequence. Returns false at end of stream, or on
   * error (in which case `error` is set). `audio`, when non-null, receives one
   * packet per audio track in the frame. */
  bool nextFrame(VideoFrame& video, std::vector<AudioPacket>* audio,
                 std::string* error);
  bool nextFrame(VideoFrame& video, std::string* error) {
    return nextFrame(video, nullptr, error);
  }

  void reset();

 private:
  bool parse(std::string* error);

  std::vector<uint8_t> owned_;
  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
  Header header_;
  bool supported_ = false;
  int curFrame_ = -1;
  std::unique_ptr<VideoDecoder> video_;
};

}  // namespace mabink

#endif
