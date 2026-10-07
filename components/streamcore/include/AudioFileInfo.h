#pragma once
// ============================================================================
//  AudioFileInfo — lightweight audio file probe for the SD file player.
//
//  Reads only headers / small parts of a file (no decoding) and returns
//    • codec, sample rate, channels, bit depth, bitrate
//    • duration (exact for FLAC / WAV / Ogg / MP3-Xing / M4A, estimated for
//      CBR MP3 and ADTS AAC)
//    • title / artist / album / track number (ID3v1/v2, FLAC + Ogg Vorbis
//      comments, Opus tags, RIFF INFO, MP4 ilst)
//    • where the audio frames start/end and what has to be sent to the
//      decoder before it can start in the middle of the file (seeking)
//
//  The decoder (VS1053/VS1063) does the real work; this only tells the
//  player how to feed it.  Pure C++ / stdio — no ESP-IDF dependency, so it
//  can be unit tested on a PC.
// ============================================================================
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace sdfile {

enum class Codec : uint8_t {
  Unknown = 0,
  MP3,
  FLAC,
  WAV,
  OggVorbis,
  Opus,
  AAC,   // raw ADTS stream (.aac)
  M4A,   // MP4 container (AAC / ALAC)
  WMA,
  MIDI,
};

const char* codecName(Codec c);

struct AudioFileInfo {
  Codec codec = Codec::Unknown;

  std::string title;
  std::string artist;
  std::string album;
  int trackNo = 0;

  uint64_t fileSize = 0;
  uint64_t audioStart = 0;  // first byte of the audio frames
  uint64_t audioEnd = 0;    // one past the last audio byte (tags excluded)

  uint32_t durationMs = 0;  // 0 = unknown
  uint32_t sampleRate = 0;
  uint8_t channels = 0;
  uint8_t bitsPerSample = 0;  // lossless formats only
  uint32_t bitrateKbps = 0;   // average

  // ---- what to send before starting at an offset > 0 -------------------------
  // Start of playback (also for offset 0):
  //   1. prefixBytes            (synthesised header, e.g. FLAC STREAMINFO)
  //   2. file[0 .. prefixFileLen)   (header pages that must be re-sent, Ogg/WAV)
  //   3. file[offset .. audioEnd)
  std::vector<uint8_t> prefixBytes;
  uint64_t prefixFileLen = 0;
  uint32_t blockAlign = 1;  // seek offsets are aligned to this (WAV)
  std::vector<uint8_t> toc; // MP3 Xing TOC (100 entries) for VBR seeking
  bool seekable = false;

  bool valid() const { return codec != Codec::Unknown; }

  /** Byte offset in the file to start playback at `ms` (audioStart for 0). */
  uint64_t offsetForMs(uint32_t ms) const;

  /** e.g. "FLAC · 16-bit / 44.1 kHz" or "MP3 · 320 kbps · 44.1 kHz" */
  std::string qualityString() const;
};

/** Codec guess from the file name extension only (cheap, for lists). */
Codec codecFromExtension(const std::string& name);

/** True for files the player can hand to the decoder. */
bool isPlayableFile(const std::string& name);

/** "01 - Song.mp3" -> "Song" (file name without extension / track prefix). */
std::string titleFromFileName(const std::string& path);

/**
 * Probe an open file.  `path` is only used for the extension fallback and the
 * title fallback (file name).  The file position is undefined afterwards.
 */
bool probe(FILE* f, const std::string& path, AudioFileInfo& out);

/** Convenience: fopen + probe + fclose. */
bool probeFile(const std::string& path, AudioFileInfo& out);

}  // namespace sdfile
