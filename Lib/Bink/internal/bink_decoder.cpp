/* Bink 1 container parser, ported from unbikit's src/bik-decoder.ts. */

#include "bink_decoder.h"

#include <cstdio>
#include <cstring>
#include <exception>

namespace mabink {
namespace {

inline uint32_t readU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

inline uint16_t readU16(const uint8_t* p) {
  return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                               (static_cast<uint16_t>(p[1]) << 8));
}

}  // namespace

Decoder::Decoder() = default;
Decoder::~Decoder() = default;

bool Decoder::openFile(const char* path, std::string* error) {
  std::FILE* file = nullptr;
  try {
    file = std::fopen(path, "rb");
    if (!file) {
      if (error) *error = std::string("cannot open ") + path;
      return false;
    }
    std::fseek(file, 0, SEEK_END);
    const long length = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (length < 0) {
      std::fclose(file);
      if (error) *error = "cannot determine file size";
      return false;
    }
    owned_.resize(static_cast<size_t>(length));
    const size_t got =
        owned_.empty() ? 0 : std::fread(owned_.data(), 1, owned_.size(), file);
    std::fclose(file);
    file = nullptr;
    if (got != owned_.size()) {
      if (error) *error = "short read";
      return false;
    }
    data_ = owned_.data();
    size_ = owned_.size();
    return parse(error);
  } catch (const std::exception& e) {
    if (file) std::fclose(file);
    if (error) *error = e.what();
    return false;
  }
}

bool Decoder::openMemory(const uint8_t* data, size_t size, std::string* error) {
  data_ = data;
  size_ = size;
  try {
    return parse(error);
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return false;
  }
}

void Decoder::reset() {
  curFrame_ = -1;
  video_.reset();
  if (header_.width > 0 && header_.height > 0) {
    try {
      video_.reset(new VideoDecoder(header_.width, header_.height,
                                    header_.subVersion, header_.hasAlpha,
                                    header_.hasSwappedUVPlanes));
    } catch (const std::exception&) {
      /* A malformed header can request a plane too large to allocate.  Keep
       * the failure contained instead of letting it escape the C ABI. */
      video_.reset();
      supported_ = false;
    }
  }
}

bool Decoder::parse(std::string* error) {
  auto fail = [&](const char* message) {
    if (error) *error = message;
    return false;
  };

  if (size_ < 44) return fail("file is smaller than the 44-byte Bink header");

  uint32_t words[11];
  for (int i = 0; i < 11; ++i) words[i] = readU32(data_ + (i << 2));

  const uint32_t magic = words[0];
  const uint32_t fourcc = magic & 0x00ffffffu;
  if (fourcc == 0x004b4942u) {
    header_.version = 1;
  } else if (fourcc == 0x0032424bu) {
    header_.version = 2;
  } else {
    return fail("not a Bink file (bad FOURCC)");
  }
  header_.subVersion = static_cast<int>(magic >> 24);
  header_.totalSize = words[1] + 8;
  header_.numFrames = static_cast<int>(words[2]);
  header_.largestFrameSize = words[3];
  header_.width = static_cast<int>(words[5]);
  header_.height = static_cast<int>(words[6]);
  header_.fps = words[8] ? static_cast<double>(words[7]) / words[8] : 0.0;

  const uint32_t videoFlags = words[9] >> 16;
  header_.hasAlpha = (videoFlags & 0x10) != 0;
  header_.hasSwappedUVPlanes = header_.version == 1 && header_.subVersion > 103;
  header_.isGrayscale = (videoFlags & 0x2) != 0;
  header_.scaling = static_cast<int>(videoFlags >> 12);
  header_.numAudioTracks = static_cast<int>(words[10]);

  if (header_.numFrames < 0 || header_.numAudioTracks < 0) {
    return fail("negative frame or track count");
  }
  /* Reject implausible dimensions/counts before allocating plane buffers or
   * trusting the frame list.  Retail Bink 1 is 620x340/640x320/640x480; the
   * 2048x2048 / 4M-pixel cap leaves room for HD mod movies without letting a
   * malformed header request an absurd 32-bit allocation. */
  const int64_t pixels =
      static_cast<int64_t>(header_.width) * header_.height;
  if (header_.width <= 0 || header_.height <= 0 ||
      header_.width > 2048 || header_.height > 2048 ||
      pixels > 4 * 1024 * 1024 ||
      header_.numFrames > 1000000 || header_.numAudioTracks > 16) {
    return fail("implausible Bink dimensions or counts");
  }

  const size_t audioHeaderSize =
      static_cast<size_t>(header_.numAudioTracks) * 12;
  const size_t frameListSize =
      (static_cast<size_t>(header_.numFrames) + 1) * 4;
  if (44 + audioHeaderSize + frameListSize > size_) {
    return fail("header lengths exceed file size");
  }

  header_.audioTracks.clear();
  if (header_.numAudioTracks > 0) {
    const uint8_t* audio = data_ + 44;
    header_.audioTracks.resize(static_cast<size_t>(header_.numAudioTracks));
    for (int i = 0; i < header_.numAudioTracks; ++i) {
      AudioTrackHeader& track = header_.audioTracks[static_cast<size_t>(i)];
      const size_t rateOffset =
          static_cast<size_t>(header_.numAudioTracks) * 4 +
          static_cast<size_t>(i) * 4;
      const size_t idOffset =
          static_cast<size_t>(header_.numAudioTracks) * 8 +
          static_cast<size_t>(i) * 4;
      const uint16_t flags = readU16(audio + rateOffset + 2);
      track.trackId = readU32(audio + idOffset);
      track.stereo = (flags & 0x2000) != 0;
      track.numChannels = track.stereo ? 2 : 1;
      track.sampleRate = readU16(audio + rateOffset);
      track.usesDCT = (flags & 0x1000) != 0;
    }
  }

  const uint8_t* frameList = data_ + 44 + audioHeaderSize;
  header_.frames.resize(static_cast<size_t>(header_.numFrames));
  for (int i = 0; i < header_.numFrames; ++i) {
    const uint32_t rawOffset = readU32(frameList + static_cast<size_t>(i) * 4);
    const uint32_t offset = rawOffset & 0xfffffffeu;
    const uint32_t nextOffset =
        readU32(frameList + (static_cast<size_t>(i) + 1) * 4) & 0xfffffffeu;
    FrameHeader& frame = header_.frames[static_cast<size_t>(i)];
    frame.offset = offset;
    frame.size = nextOffset > offset ? nextOffset - offset : 0;
    frame.keyframe = (rawOffset & 1) != 0;
  }

  supported_ = header_.version == 1 && header_.subVersion > 0x63 &&
               header_.subVersion < 0x6a && header_.subVersion != 0x65;

  reset();
  return true;
}

bool Decoder::nextFrame(VideoFrame& out, std::vector<AudioPacket>* audio,
                        std::string* error) {
  try {
  if (!supported_) {
    if (error) *error = "unsupported Bink version";
    return false;
  }
  if (curFrame_ + 1 >= header_.numFrames) return false;

  ++curFrame_;
  const FrameHeader& frame = header_.frames[static_cast<size_t>(curFrame_)];

  if (static_cast<uint64_t>(frame.offset) + frame.size > size_) {
    if (error) *error = "frame extends past end of file";
    return false;
  }

  const uint8_t* frameData = data_ + frame.offset;
  size_t audioFramePos = 0;
  if (audio) audio->clear();

  /* Each audio track is [u32 packetSize][u32 sampleCount][data...]; the
   * video bitstream follows the last track. */
  for (int i = 0; i < header_.numAudioTracks; ++i) {
    if (audioFramePos + 4 > frame.size) {
      if (error) *error = "truncated audio packet header";
      return false;
    }
    const uint32_t packetSize = readU32(frameData + audioFramePos);
    if (audio) {
      AudioPacket packet;
      packet.numSamples =
          packetSize > 3 ? static_cast<int>(readU32(frameData + audioFramePos + 4))
                         : 0;
      packet.size = packetSize > 3 ? static_cast<size_t>(packetSize) - 4 : 0;
      packet.data = frameData + audioFramePos + 8;
      if (audioFramePos + 8 + packet.size > frame.size) {
        if (error) *error = "audio packet exceeds frame size";
        return false;
      }
      audio->push_back(packet);
    }
    audioFramePos += static_cast<size_t>(packetSize) + 4;
  }

  if (audioFramePos > frame.size) {
    if (error) *error = "audio packets exceed frame size";
    return false;
  }

  const size_t videoSize = frame.size - audioFramePos;
  if (!video_) {
    if (error) *error = "no video decoder";
    return false;
  }
  video_->decode(frameData + audioFramePos, videoSize, out);
  return true;
  } catch (const std::exception& e) {
    /* Malformed/mod movie bitstreams can describe invalid block types.  The
     * decoder API promises a bool/error result, so contain the exception
     * rather than letting it unwind through the game's C ABI. */
    if (error) *error = e.what();
    return false;
  }
}

}  // namespace mabink
