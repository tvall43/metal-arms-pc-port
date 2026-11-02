/*
 * In-tree Bink 1 backend for the Metal Arms PC port.
 *
 * This file implements the 14 stdcall entry points the retail executables
 * import from binkw32.dll, using the portable decoder in this directory
 * instead of RAD's DLL or an FFmpeg DLL.  It is compiled into ma_win.exe when
 * MA_BINK_BACKEND=internal; the external build still links binkw32.lib.
 *
 * The public BINK struct is large and private to RAD, but the engine only
 * reads Width, Height, Frames and FrameNum.  Movie therefore starts with
 * those four fields and the rest is private state, exactly like the FFmpeg
 * shim.  No code outside this file should depend on the private layout.
 */
#ifndef __RADINEXE__
#define __RADINEXE__ /* radbase.h: plain static functions, not dllimport */
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
#include <d3d8.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "bink.h"
#include "bink_audio.h"
#include "bink_decoder.h"

namespace {

char g_error[512] = "no error";

void setError(const char* format, ...) {
  va_list args;
  va_start(args, format);
  std::vsnprintf(g_error, sizeof(g_error), format, args);
  va_end(args);
}

void logLine(const char* format, ...) {
  static std::FILE* file = nullptr;
  static bool initialised = false;
  if (!initialised) {
    initialised = true;
    const char* path = std::getenv("MA_BINK_LOG");
    if (path && *path) file = std::fopen(path, "a");
  }
  if (!file) return;
  va_list args;
  va_start(args, format);
  std::vfprintf(file, format, args);
  va_end(args);
  std::fputc('\n', file);
  std::fflush(file);
}

/* Selected Bink sound track IDs, set by BinkSetSoundTrack before BinkOpen. */
U32 g_trackCount = 0;
U32 g_trackIds[16] = {};

/* MinGW's import library does not always provide this GUID, so instantiate it
 * locally.  Value is the documented IID_IDirectSoundBuffer8. */
const GUID kIID_IDirectSoundBuffer8 = {
    0x6825a449, 0x7524, 0x4d82,
    {0x92, 0x0f, 0x50, 0xe3, 0x6a, 0xb3, 0xab, 0x1e}};

struct AudioRing {
  IDirectSound8* ds = nullptr;
  IDirectSoundBuffer8* buffer = nullptr;
  WAVEFORMATEX format = {};
  U32 bufferBytes = 0;
  U32 writePos = 0;
  U32 lastPlayPos = 0;
  U32 pausePos = 0;
  U64 totalWritten = 0;
  U64 playedBytes = 0;
  bool available = false;
  bool started = false;
  bool paused = false;
  int bytesPerSecond = 0;
  int blockAlign = 0;
  S32 volume = 32768;
};

struct Movie {
  /* Public BINK prefix.  Offsets are fixed by the retail ABI. */
  U32 Width = 0;
  U32 Height = 0;
  U32 Frames = 0;
  U32 FrameNum = 0;

  mabink::Decoder decoder;
  mabink::VideoFrame frame;
  std::unique_ptr<mabink::AudioDecoder> audioDecoder;
  int audioTrackIndex = -1;
  /* Number of leading frames decoded while filling the audio ring.  Playback
   * skips audio for these frames because the ring already holds their PCM; the
   * AudioDecoder's overlap state is deliberately left after the last one. */
  int prefillFrames = 0;

  /* The retail DLL streams; map the file read-only so opening a 106 MB intro
   * does not allocate another 106 MB in the 32-bit process. */
  HANDLE sourceFile = INVALID_HANDLE_VALUE;
  HANDLE sourceMapping = nullptr;
  void* sourceView = nullptr;
  size_t sourceSize = 0;

  AudioRing audio;

  bool decoded = false;
  bool haveFrame = false;
  bool failed = false;
  bool paused = false;

  bool t0Set = false;
  LARGE_INTEGER t0 = {};
  LARGE_INTEGER pauseStart = {};
  LARGE_INTEGER qpcFreq = {};
  double frameDuration = 1.0 / 30.0;
};

void audioUpdatePlayed(Movie* movie) {
  AudioRing& audio = movie->audio;
  DWORD play = 0;
  DWORD write = 0;

  if (!audio.available || !audio.started || !audio.buffer) return;
  if (FAILED(IDirectSoundBuffer8_GetCurrentPosition(audio.buffer, &play, &write)))
    return;

  if (play >= audio.lastPlayPos) {
    audio.playedBytes += static_cast<U64>(play - audio.lastPlayPos);
  } else {
    audio.playedBytes += static_cast<U64>(audio.bufferBytes - audio.lastPlayPos) +
                         play;
  }
  audio.lastPlayPos = play;
  if (audio.playedBytes > audio.totalWritten)
    audio.playedBytes = audio.totalWritten;
}

U32 audioBuffered(const Movie* movie) {
  const AudioRing& audio = movie->audio;
  if (!audio.available) return 0;
  U64 buffered = audio.totalWritten - audio.playedBytes;
  if (buffered > audio.bufferBytes) buffered = audio.bufferBytes;
  return static_cast<U32>(buffered);
}

void audioWrite(Movie* movie, const U8* data, U32 bytes) {
  AudioRing& audio = movie->audio;
  void* part1 = nullptr;
  void* part2 = nullptr;
  DWORD bytes1 = 0;
  DWORD bytes2 = 0;

  if (!audio.available || !audio.buffer || bytes == 0) return;

  const U32 buffered = audioBuffered(movie);
  if (buffered >= audio.bufferBytes) return;
  U32 freeBytes = audio.bufferBytes - buffered;
  if (bytes > freeBytes) bytes = freeBytes;
  if (bytes == 0) return;

  HRESULT hr = IDirectSoundBuffer8_Lock(
      audio.buffer, audio.writePos, bytes, &part1, &bytes1, &part2, &bytes2, 0);
  if (hr == DSERR_BUFFERLOST) {
    IDirectSoundBuffer8_Restore(audio.buffer);
    hr = IDirectSoundBuffer8_Lock(audio.buffer, audio.writePos, bytes, &part1,
                                  &bytes1, &part2, &bytes2, 0);
  }
  if (FAILED(hr)) return;

  std::memcpy(part1, data, bytes1);
  if (bytes2) std::memcpy(part2, data + bytes1, bytes2);
  IDirectSoundBuffer8_Unlock(audio.buffer, part1, bytes1, part2, bytes2);

  audio.writePos = (audio.writePos + bytes) % audio.bufferBytes;
  audio.totalWritten += bytes;
}

void audioSetVolume(Movie* movie, S32 volume) {
  AudioRing& audio = movie->audio;
  if (volume < 0) volume = 0;
  if (volume > 32768) volume = 32768;
  audio.volume = volume;
  if (!audio.available || !audio.buffer) return;

  LONG dsVolume;
  if (volume <= 0) {
    dsVolume = DSBVOLUME_MIN;
  } else if (volume >= 32768) {
    dsVolume = DSBVOLUME_MAX;
  } else {
    double db = 2000.0 * std::log10(static_cast<double>(volume) / 32768.0);
    if (db < DSBVOLUME_MIN) db = DSBVOLUME_MIN;
    if (db > DSBVOLUME_MAX) db = DSBVOLUME_MAX;
    dsVolume = static_cast<LONG>(db);
  }
  IDirectSoundBuffer8_SetVolume(audio.buffer, dsVolume);
}

void audioZeroBuffer(Movie* movie) {
  AudioRing& audio = movie->audio;
  void* part1 = nullptr;
  void* part2 = nullptr;
  DWORD bytes1 = 0;
  DWORD bytes2 = 0;
  if (!audio.available || !audio.buffer) return;
  if (FAILED(IDirectSoundBuffer8_Lock(audio.buffer, 0, audio.bufferBytes, &part1,
                                     &bytes1, &part2, &bytes2, 0)))
    return;
  if (part1) std::memset(part1, 0, bytes1);
  if (part2) std::memset(part2, 0, bytes2);
  IDirectSoundBuffer8_Unlock(audio.buffer, part1, bytes1, part2, bytes2);
}

bool audioCreate(Movie* movie, int sampleRate, int channels) {
  AudioRing& audio = movie->audio;
  if (sampleRate <= 0) sampleRate = 44100;
  if (channels <= 0) channels = 2;

  audio = AudioRing();
  audio.format.wFormatTag = WAVE_FORMAT_PCM;
  audio.format.nChannels = static_cast<WORD>(channels);
  audio.format.nSamplesPerSec = static_cast<DWORD>(sampleRate);
  audio.format.wBitsPerSample = 16;
  audio.format.nBlockAlign = static_cast<WORD>(channels * 2);
  audio.format.nAvgBytesPerSec =
      static_cast<DWORD>(sampleRate * audio.format.nBlockAlign);
  audio.format.cbSize = 0;
  audio.blockAlign = audio.format.nBlockAlign;
  audio.bytesPerSecond = static_cast<int>(audio.format.nAvgBytesPerSec);

  audio.bufferBytes = audio.format.nAvgBytesPerSec * 2;
  audio.bufferBytes -= audio.bufferBytes % audio.blockAlign;
  if (audio.bufferBytes < static_cast<U32>(audio.format.nAvgBytesPerSec / 4))
    audio.bufferBytes = audio.format.nAvgBytesPerSec / 4;
  audio.volume = 32768;

  HWND window = FindWindowA("Fang D3D Window", nullptr);
  if (!window) window = GetForegroundWindow();
  if (!window) window = GetDesktopWindow();

  HRESULT hr = DirectSoundCreate8(nullptr, &audio.ds, nullptr);
  if (FAILED(hr)) {
    logLine("[bink-internal] DirectSoundCreate8 failed: 0x%08lx",
            static_cast<unsigned long>(hr));
    return false;
  }
  hr = IDirectSound8_SetCooperativeLevel(audio.ds, window, DSSCL_PRIORITY);
  if (FAILED(hr))
    hr = IDirectSound8_SetCooperativeLevel(audio.ds, window, DSSCL_NORMAL);
  if (FAILED(hr)) {
    logLine("[bink-internal] SetCooperativeLevel failed: 0x%08lx",
            static_cast<unsigned long>(hr));
    IDirectSound8_Release(audio.ds);
    audio.ds = nullptr;
    return false;
  }

  DSBUFFERDESC desc = {};
  desc.dwSize = sizeof(desc);
  desc.dwFlags = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS |
                 DSBCAPS_CTRLVOLUME;
  desc.dwBufferBytes = audio.bufferBytes;
  desc.lpwfxFormat = &audio.format;

  IDirectSoundBuffer* temporary = nullptr;
  hr = IDirectSound8_CreateSoundBuffer(audio.ds, &desc, &temporary, nullptr);
  if (FAILED(hr)) {
    logLine("[bink-internal] CreateSoundBuffer failed: 0x%08lx",
            static_cast<unsigned long>(hr));
    IDirectSound8_Release(audio.ds);
    audio.ds = nullptr;
    return false;
  }
  hr = IDirectSoundBuffer_QueryInterface(
      temporary, kIID_IDirectSoundBuffer8,
      reinterpret_cast<void**>(&audio.buffer));
  IDirectSoundBuffer_Release(temporary);
  if (FAILED(hr) || !audio.buffer) {
    logLine("[bink-internal] IDirectSoundBuffer8 unavailable: 0x%08lx",
            static_cast<unsigned long>(hr));
    if (audio.buffer) {
      IDirectSoundBuffer8_Release(audio.buffer);
      audio.buffer = nullptr;
    }
    IDirectSound8_Release(audio.ds);
    audio.ds = nullptr;
    return false;
  }

  audio.available = true;
  audioZeroBuffer(movie);
  audioSetVolume(movie, 32768);
  logLine("[bink-internal] audio: %d Hz, %d ch, %u-byte ring", sampleRate,
          channels, audio.bufferBytes);
  return true;
}

void audioDestroy(Movie* movie) {
  AudioRing& audio = movie->audio;
  if (audio.buffer) {
    IDirectSoundBuffer8_Stop(audio.buffer);
    IDirectSoundBuffer8_Release(audio.buffer);
    audio.buffer = nullptr;
  }
  if (audio.ds) {
    IDirectSound8_Release(audio.ds);
    audio.ds = nullptr;
  }
  audio.available = false;
  audio.started = false;
  audio.paused = false;
}

void audioStart(Movie* movie) {
  AudioRing& audio = movie->audio;
  if (!audio.available || !audio.buffer || audio.started) return;

  IDirectSoundBuffer8_SetCurrentPosition(audio.buffer, 0);
  const HRESULT hr =
      IDirectSoundBuffer8_Play(audio.buffer, 0, 0, DSBPLAY_LOOPING);
  if (FAILED(hr)) {
    logLine("[bink-internal] Play failed: 0x%08lx",
            static_cast<unsigned long>(hr));
    return;
  }
  audio.started = true;
  audio.paused = false;
  audio.lastPlayPos = 0;
  audio.playedBytes = 0;
  audioSetVolume(movie, audio.volume);
}

void audioPause(Movie* movie, bool pause) {
  AudioRing& audio = movie->audio;
  DWORD play = 0;
  DWORD write = 0;
  if (!audio.available || !audio.buffer || !audio.started) return;

  if (pause && !audio.paused) {
    IDirectSoundBuffer8_GetCurrentPosition(audio.buffer, &play, &write);
    audio.pausePos = play;
    IDirectSoundBuffer8_Stop(audio.buffer);
    audio.paused = true;
  } else if (!pause && audio.paused) {
    IDirectSoundBuffer8_SetCurrentPosition(audio.buffer, audio.pausePos);
    audio.lastPlayPos = audio.pausePos;
    IDirectSoundBuffer8_Play(audio.buffer, 0, 0, DSBPLAY_LOOPING);
    audio.paused = false;
  }
}

void audioReset(Movie* movie) {
  AudioRing& audio = movie->audio;
  if (!audio.available) return;
  if (audio.buffer) {
    IDirectSoundBuffer8_Stop(audio.buffer);
    IDirectSoundBuffer8_SetCurrentPosition(audio.buffer, 0);
  }
  audio.started = false;
  audio.paused = false;
  audio.writePos = 0;
  audio.lastPlayPos = 0;
  audio.pausePos = 0;
  audio.totalWritten = 0;
  audio.playedBytes = 0;
  audioZeroBuffer(movie);
}

double qpcSecondsSince(const LARGE_INTEGER& base, const LARGE_INTEGER& freq) {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return static_cast<double>(now.QuadPart - base.QuadPart) /
         static_cast<double>(freq.QuadPart);
}

double elapsedSeconds(Movie* movie) {
  if (!movie->t0Set) return 0.0;
  const double wall = qpcSecondsSince(movie->t0, movie->qpcFreq);
  if (movie->audio.available && movie->audio.started && !movie->audio.paused) {
    audioUpdatePlayed(movie);
    const double audioTime =
        static_cast<double>(movie->audio.playedBytes) /
        static_cast<double>(movie->audio.bytesPerSecond);
    /* Follow the sound card while it tracks the wall clock; if the device
     * stalls, fall back so the game cannot hang on a dead audio buffer. */
    if (audioTime > 0.0 && std::fabs(audioTime - wall) < 0.25)
      return audioTime;
  }
  return wall;
}

void closeSource(Movie* movie) {
  if (movie->sourceView) {
    UnmapViewOfFile(movie->sourceView);
    movie->sourceView = nullptr;
  }
  if (movie->sourceMapping) {
    CloseHandle(movie->sourceMapping);
    movie->sourceMapping = nullptr;
  }
  if (movie->sourceFile != INVALID_HANDLE_VALUE) {
    CloseHandle(movie->sourceFile);
    movie->sourceFile = INVALID_HANDLE_VALUE;
  }
  movie->sourceSize = 0;
}

/* Map the movie and hand the view to the portable decoder.  Returns false if
 * the platform cannot map it, in which case the caller falls back to a normal
 * read (still correct, just more memory). */
bool openMapped(Movie* movie, const char* name, std::string* error) {
  HANDLE file = CreateFileA(name, GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    if (error) *error = "CreateFileA failed";
    return false;
  }

  LARGE_INTEGER length;
  if (!GetFileSizeEx(file, &length) || length.QuadPart < 44 ||
      length.QuadPart > 0xffffffffll) {
    if (error) *error = "unsupported file size";
    CloseHandle(file);
    return false;
  }

  HANDLE mapping =
      CreateFileMappingA(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (!mapping) {
    if (error) *error = "CreateFileMappingA failed";
    CloseHandle(file);
    return false;
  }

  void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
  if (!view) {
    if (error) *error = "MapViewOfFile failed";
    CloseHandle(mapping);
    CloseHandle(file);
    return false;
  }

  movie->sourceFile = file;
  movie->sourceMapping = mapping;
  movie->sourceView = view;
  movie->sourceSize = static_cast<size_t>(length.QuadPart);

  if (!movie->decoder.openMemory(static_cast<const uint8_t*>(view),
                                 movie->sourceSize, error)) {
    closeSource(movie);
    return false;
  }
  return true;
}

/* Convert planar YUV 4:2:0 (limited-range BT.601, as the movies are encoded)
 * into the surface formats the engine actually asks for. */
inline U8 clamp8(int value) {
  if (value < 0) return 0;
  if (value > 255) return 255;
  return static_cast<U8>(value);
}

void convertFrame(const mabink::VideoFrame& frame, void* dest, S32 destPitch,
                  U32 destHeight, U32 destX, U32 destY, int surface) {
  const int width = frame.width;
  const int height = frame.height;
  const int chromaWidth = (width + 1) / 2;
  const int chromaHeight = (height + 1) / 2;
  const U8* yPlane = frame.yuv.data();
  const U8* uPlane = yPlane + static_cast<size_t>(width) * height;
  const U8* vPlane = uPlane + static_cast<size_t>(chromaWidth) * chromaHeight;

  int rows = height;
  if (destHeight && static_cast<U32>(rows) > destHeight)
    rows = static_cast<int>(destHeight);

  int bytesPerPixel = 4;
  switch (surface) {
    case BINKSURFACE24:
    case BINKSURFACE24R:
      bytesPerPixel = 3;
      break;
    case BINKSURFACE565:
    case BINKSURFACE555:
      bytesPerPixel = 2;
      break;
    default:
      bytesPerPixel = 4;
      break;
  }

  U8* dst = static_cast<U8*>(dest) + static_cast<size_t>(destY) * destPitch +
            static_cast<size_t>(destX) * bytesPerPixel;

  for (int y = 0; y < rows; ++y) {
    U8* row = dst + static_cast<size_t>(y) * destPitch;
    const U8* yRow = yPlane + static_cast<size_t>(y) * width;
    const U8* uRow = uPlane + static_cast<size_t>(y / 2) * chromaWidth;
    const U8* vRow = vPlane + static_cast<size_t>(y / 2) * chromaWidth;
    for (int x = 0; x < width; ++x) {
      const int c = static_cast<int>(yRow[x]) - 16;
      const int d = static_cast<int>(uRow[x / 2]) - 128;
      const int e = static_cast<int>(vRow[x / 2]) - 128;
      const U8 r = clamp8((298 * c + 409 * e + 128) >> 8);
      const U8 g = clamp8((298 * c - 100 * d - 208 * e + 128) >> 8);
      const U8 b = clamp8((298 * c + 516 * d + 128) >> 8);
      U8* pixel = row + static_cast<size_t>(x) * bytesPerPixel;
      switch (surface) {
        case BINKSURFACE32:
        case BINKSURFACE32A:
          pixel[0] = b;
          pixel[1] = g;
          pixel[2] = r;
          pixel[3] = 255;
          break;
        case BINKSURFACE32R:
        case BINKSURFACE32RA:
          pixel[0] = r;
          pixel[1] = g;
          pixel[2] = b;
          pixel[3] = 255;
          break;
        case BINKSURFACE24:
          pixel[0] = b;
          pixel[1] = g;
          pixel[2] = r;
          break;
        case BINKSURFACE24R:
          pixel[0] = r;
          pixel[1] = g;
          pixel[2] = b;
          break;
        case BINKSURFACE565: {
          const U16 value = static_cast<U16>(((r >> 3) << 11) |
                                             ((g >> 2) << 5) | (b >> 3));
          pixel[0] = static_cast<U8>(value & 0xff);
          pixel[1] = static_cast<U8>(value >> 8);
          break;
        }
        case BINKSURFACE555: {
          const U16 value = static_cast<U16>(((r >> 3) << 10) |
                                             ((g >> 3) << 5) | (b >> 3));
          pixel[0] = static_cast<U8>(value & 0xff);
          pixel[1] = static_cast<U8>(value >> 8);
          break;
        }
        default:
          break;
      }
    }
  }
}

void writeAudioPacket(Movie* movie, const mabink::AudioPacket& packet) {
  if (!movie->audioDecoder || !movie->audio.available) return;
  std::vector<std::vector<std::vector<float>>> blocks;
  std::string error;
  if (!movie->audioDecoder->decodePacket(packet.data, packet.size, blocks,
                                         &error)) {
    logLine("[bink-internal] audio decode failed: %s", error.c_str());
    movie->audio.available = false;
    return;
  }
  if (blocks.empty()) return;

  std::vector<S16> pcm;
  const int channels = movie->audioDecoder->numChannels();
  /* blocks is [block][channel][sample]; blocks[0].size() is the channel
   * count, not the sample count.  Using it here wrote only two samples per
   * block and left the ring almost silent. */
  const size_t samplesPerBlock =
      blocks[0].empty() ? 0 : blocks[0][0].size();
  if (channels < 1 || samplesPerBlock < 1) return;
  pcm.reserve(blocks.size() * samplesPerBlock * static_cast<size_t>(channels));

  for (const auto& block : blocks) {
    for (size_t i = 0; i < samplesPerBlock; ++i) {
      for (int ch = 0; ch < channels; ++ch) {
        float value = ch < static_cast<int>(block.size()) &&
                              i < block[static_cast<size_t>(ch)].size()
                          ? block[static_cast<size_t>(ch)][i]
                          : 0.0f;
        value *= 32768.0f;
        value = std::max(-32768.0f, std::min(32767.0f, value));
        pcm.push_back(static_cast<S16>(std::lround(value)));
      }
    }
  }
  if (!pcm.empty())
    audioWrite(movie, reinterpret_cast<const U8*>(pcm.data()),
               static_cast<U32>(pcm.size() * sizeof(S16)));
}

/* The retail DLL and the FFmpeg shim keep ~1.5 s of audio queued before the
 * movie starts.  Bink interleaves audio packets with video frames, so fill the
 * ring by decoding the leading frames once, feed their PCM, then rewind the
 * video decoder.  The AudioDecoder is left advanced past those frames: during
 * playback BinkDoFrame skips audio for the same leading frames, so the first
 * sample it decodes again is exactly the one after the prefilled range. */
void prefillAudio(Movie* movie) {
  movie->prefillFrames = 0;
  if (!movie->audioDecoder || !movie->audio.available) return;

  const U32 target = static_cast<U32>(
      static_cast<U64>(movie->audio.bytesPerSecond) * 3 / 2);
  int frames = 0;
  while (frames < static_cast<int>(movie->Frames) &&
         audioBuffered(movie) < target && movie->audio.available) {
    std::vector<mabink::AudioPacket> packets;
    std::string error;
    if (!movie->decoder.nextFrame(movie->frame, &packets, &error)) {
      if (!error.empty())
        logLine("[bink-internal] audio prefill failed at frame %d: %s", frames,
                error.c_str());
      break;
    }
    if (movie->audioTrackIndex >= 0 &&
        static_cast<size_t>(movie->audioTrackIndex) < packets.size()) {
      writeAudioPacket(movie,
                       packets[static_cast<size_t>(movie->audioTrackIndex)]);
    }
    ++frames;
  }

  movie->prefillFrames = frames;
  movie->decoder.reset();
  movie->decoded = false;
  movie->haveFrame = false;
  logLine("[bink-internal] audio prefill: %d frames, %u bytes buffered", frames,
          audioBuffered(movie));
}

}  // namespace

extern "C" {

HBINK RADEXPLINK BinkOpen(const char* name, U32 flags) {
  Movie* movie = new (std::nothrow) Movie();
  if (!movie) {
    setError("out of memory");
    return nullptr;
  }
  QueryPerformanceFrequency(&movie->qpcFreq);
  movie->FrameNum = 1;

  std::string error;
  if (!openMapped(movie, name, &error)) {
    /* Mapping is an optimisation; a shared/network path can fail.  Fall back
     * to reading the file rather than refusing to play the movie. */
    error.clear();
    if (!movie->decoder.openFile(name, &error)) {
      setError("cannot open %s: %s", name, error.c_str());
      closeSource(movie);
      delete movie;
      return nullptr;
    }
  }
  if (!movie->decoder.isSupported()) {
    setError("unsupported Bink file: %s", name);
    closeSource(movie);
    delete movie;
    return nullptr;
  }

  const mabink::Header& header = movie->decoder.header();
  movie->Width = static_cast<U32>(header.width);
  movie->Height = static_cast<U32>(header.height);
  movie->Frames = static_cast<U32>(header.numFrames > 0 ? header.numFrames : 1);
  movie->frameDuration = header.fps > 0.0 ? 1.0 / header.fps : 1.0 / 30.0;

  int track = -1;
  if (!header.audioTracks.empty()) {
    track = 0;
    if (g_trackCount > 0) {
      track = -1;
      for (size_t i = 0; i < header.audioTracks.size() && track < 0; ++i) {
        for (U32 t = 0; t < g_trackCount; ++t) {
          if (header.audioTracks[i].trackId == g_trackIds[t]) {
            track = static_cast<int>(i);
            break;
          }
        }
      }
      if (track < 0) track = 0;
    }
  }

  if (track >= 0) {
    const mabink::AudioTrackHeader& at = header.audioTracks[track];
    movie->audioDecoder.reset(new (std::nothrow)
                                  mabink::AudioDecoder(at.sampleRate,
                                                       at.numChannels,
                                                       at.usesDCT));
    if (movie->audioDecoder && movie->audioDecoder->supported()) {
      movie->audioTrackIndex = track;
      if (!audioCreate(movie, at.sampleRate, at.numChannels)) {
        movie->audioDecoder.reset();
        movie->audioTrackIndex = -1;
      }
    } else {
      movie->audioDecoder.reset();
    }
  }

  prefillAudio(movie);

  logLine("[bink-internal] open %s: %ux%u, %u frames, %.3f fps, audio=%s",
          name, movie->Width, movie->Height, movie->Frames,
          movie->frameDuration > 0.0 ? 1.0 / movie->frameDuration : 0.0,
          movie->audio.available ? "yes" : "no");
  return reinterpret_cast<HBINK>(movie);
}

void RADEXPLINK BinkClose(HBINK bink) {
  Movie* movie = reinterpret_cast<Movie*>(bink);
  if (!movie) return;
  audioDestroy(movie);
  closeSource(movie);
  delete movie;
}

S32 RADEXPLINK BinkDoFrame(HBINK bink) {
  Movie* movie = reinterpret_cast<Movie*>(bink);
  if (!movie || movie->failed) return 0;
  if (movie->decoded) return 0;

  std::vector<mabink::AudioPacket> packets;
  std::string error;
  if (!movie->decoder.nextFrame(movie->frame, &packets, &error)) {
    if (!error.empty())
      logLine("[bink-internal] decode failed at frame %u: %s", movie->FrameNum,
              error.c_str());
    movie->failed = true;
    movie->FrameNum = movie->Frames;
    return 0;
  }

  const int frameIndex = movie->decoder.currentFrame();
  if (movie->audioDecoder && movie->audioTrackIndex >= 0 &&
      frameIndex >= movie->prefillFrames &&
      static_cast<size_t>(movie->audioTrackIndex) < packets.size()) {
    writeAudioPacket(movie,
                     packets[static_cast<size_t>(movie->audioTrackIndex)]);
  }

  movie->decoded = true;
  movie->haveFrame = true;

  if (!movie->t0Set) {
    QueryPerformanceCounter(&movie->t0);
    movie->t0Set = true;
    if (movie->audio.available) audioStart(movie);
  }
  return 0;
}

void RADEXPLINK BinkNextFrame(HBINK bink) {
  Movie* movie = reinterpret_cast<Movie*>(bink);
  if (!movie) return;
  movie->decoded = false;
  if (movie->FrameNum != 0xffffffffu) ++movie->FrameNum;

  if (movie->FrameNum > movie->Frames) {
    logLine("[bink-internal] loop reset after frame %u/%u", movie->FrameNum - 1,
            movie->Frames);
    movie->decoder.reset();
    if (movie->audioDecoder) movie->audioDecoder->reset();
    audioReset(movie);
    movie->prefillFrames = 0;
    prefillAudio(movie);
    movie->t0Set = false;
    movie->haveFrame = false;
    movie->failed = false;
    movie->FrameNum = 1;
  }
}

S32 RADEXPLINK BinkWait(HBINK bink) {
  Movie* movie = reinterpret_cast<Movie*>(bink);
  if (!movie) return 0;
  if (movie->failed) return 0;
  if (movie->paused) return 1;
  if (movie->FrameNum <= 1 || !movie->t0Set) return 0;
  if (movie->FrameNum > movie->Frames) return 0;

  const double target =
      static_cast<double>(movie->FrameNum - 1) * movie->frameDuration;
  const double wall = qpcSecondsSince(movie->t0, movie->qpcFreq);
  if (wall + 0.050 < target) return 1;

  const double now = elapsedSeconds(movie);
  if (now + 0.0005 >= target) {
    /* Same late-clock rebase as the FFmpeg shim and the retail DLL: do not
     * draw a burst of overdue frames after the loader blocked the renderer. */
    if (!movie->audio.available && (wall - target) > 2.0) {
      LARGE_INTEGER current;
      QueryPerformanceCounter(&current);
      const double offset =
          static_cast<double>(movie->FrameNum - 1) * movie->frameDuration;
      movie->t0.QuadPart =
          current.QuadPart -
          static_cast<LONGLONG>(offset *
                                static_cast<double>(movie->qpcFreq.QuadPart));
      logLine("[bink-internal] rebase clock at frame %u/%u (late %.3f s)",
              movie->FrameNum, movie->Frames, wall - target);
    }
    return 0;
  }
  return 1;
}

S32 RADEXPLINK BinkCopyToBuffer(HBINK bink, void* dest, S32 destPitch,
                                U32 destHeight, U32 destX, U32 destY,
                                U32 flags) {
  Movie* movie = reinterpret_cast<Movie*>(bink);
  if (!movie || !movie->haveFrame || !dest || movie->failed) return 1;

  const int surface = static_cast<int>(flags & BINKSURFACEMASK);
  switch (surface) {
    case BINKSURFACE32:
    case BINKSURFACE32R:
    case BINKSURFACE32A:
    case BINKSURFACE32RA:
    case BINKSURFACE24:
    case BINKSURFACE24R:
    case BINKSURFACE565:
    case BINKSURFACE555:
      break;
    default:
      logLine("[bink-internal] unsupported surface type 0x%x", surface);
      return 1;
  }

  convertFrame(movie->frame, dest, destPitch, destHeight, destX, destY, surface);
  return 0;
}

S32 RADEXPLINK BinkPause(HBINK bink, S32 pause) {
  Movie* movie = reinterpret_cast<Movie*>(bink);
  if (!movie) return 0;
  const S32 old = movie->paused ? 1 : 0;

  if (pause && !movie->paused) {
    movie->paused = true;
    if (movie->t0Set) QueryPerformanceCounter(&movie->pauseStart);
    audioPause(movie, true);
  } else if (!pause && movie->paused) {
    movie->paused = false;
    if (movie->t0Set) {
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      movie->t0.QuadPart += now.QuadPart - movie->pauseStart.QuadPart;
    }
    audioPause(movie, false);
  }
  return old;
}

void RADEXPLINK BinkSetVolume(HBINK bink, U32 trackId, S32 volume) {
  (void)trackId;
  Movie* movie = reinterpret_cast<Movie*>(bink);
  if (movie) audioSetVolume(movie, volume);
}

void RADEXPLINK BinkSetSoundTrack(U32 totalTracks, U32* tracks) {
  if (!tracks || totalTracks == 0) {
    g_trackCount = 0;
    return;
  }
  if (totalTracks > 16) totalTracks = 16;
  g_trackCount = totalTracks;
  for (U32 i = 0; i < totalTracks; ++i) g_trackIds[i] = tracks[i];
}

S32 RADEXPLINK BinkSetSoundSystem(BINKSNDSYSOPEN open, U32 param) {
  (void)open;
  (void)param;
  return 1;
}

S32 RADLINK dummySoundOpen(BINKSND* sound, U32 freq, S32 bits, S32 channels,
                           U32 flags, HBINK bink) {
  (void)sound;
  (void)freq;
  (void)bits;
  (void)channels;
  (void)flags;
  (void)bink;
  return 0;
}

BINKSNDOPEN RADEXPLINK BinkOpenDirectSound(U32 param) {
  (void)param;
  return dummySoundOpen;
}

S32 RADEXPLINK BinkDX8SurfaceType(void* surface) {
  IDirect3DSurface8* d3dSurface = static_cast<IDirect3DSurface8*>(surface);
  D3DSURFACE_DESC desc;
  if (!d3dSurface || FAILED(IDirect3DSurface8_GetDesc(d3dSurface, &desc)))
    return BINKSURFACE32;

  switch (desc.Format) {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
      return BINKSURFACE32;
    case D3DFMT_R5G6B5:
      return BINKSURFACE565;
    case D3DFMT_X1R5G5B5:
      return BINKSURFACE555;
    case D3DFMT_R8G8B8:
      return BINKSURFACE24;
    default:
      return BINKSURFACE32;
  }
}

char* RADEXPLINK BinkGetError(void) { return g_error; }

void RADEXPLINK RADSetMemory(RADMEMALLOC alloc, RADMEMFREE freeFunction) {
  (void)alloc;
  (void)freeFunction;
}

}  // extern "C"
