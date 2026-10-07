#include "AudioFileInfo.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace sdfile {

// ============================================================================
//  small helpers
// ============================================================================
namespace {

constexpr size_t kMaxTextFrame = 2048;      // ID3 / comment values we decode
constexpr size_t kMaxPacket = 64 * 1024;    // Ogg header packets we keep
constexpr uint64_t kMaxOggPrefix = 512 * 1024;
constexpr uint64_t kMaxWavPrefix = 64 * 1024;

inline uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline uint32_t be24(const uint8_t* p) {
  return uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2];
}
inline uint32_t be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 |
         p[3];
}
inline uint64_t be64(const uint8_t* p) {
  return uint64_t(be32(p)) << 32 | be32(p + 4);
}
inline uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
inline uint32_t le32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
         uint32_t(p[3]) << 24;
}
inline uint64_t le64(const uint8_t* p) {
  return uint64_t(le32(p)) | uint64_t(le32(p + 4)) << 32;
}
inline uint32_t syncsafe32(const uint8_t* p) {
  return uint32_t(p[0] & 0x7F) << 21 | uint32_t(p[1] & 0x7F) << 14 |
         uint32_t(p[2] & 0x7F) << 7 | uint32_t(p[3] & 0x7F);
}

bool readAt(FILE* f, uint64_t off, void* buf, size_t n) {
  if (fseek(f, (long)off, SEEK_SET) != 0)
    return false;
  return fread(buf, 1, n, f) == n;
}
size_t readSomeAt(FILE* f, uint64_t off, void* buf, size_t n) {
  if (fseek(f, (long)off, SEEK_SET) != 0)
    return 0;
  return fread(buf, 1, n, f);
}

void appendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += char(cp);
  } else if (cp < 0x800) {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  } else {
    out += char(0xF0 | (cp >> 18));
    out += char(0x80 | ((cp >> 12) & 0x3F));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
}

std::string latin1ToUtf8(const uint8_t* p, size_t n) {
  std::string s;
  for (size_t i = 0; i < n && p[i]; i++)
    appendUtf8(s, p[i]);
  return s;
}

std::string utf16ToUtf8(const uint8_t* p, size_t n, bool bigEndian) {
  std::string s;
  for (size_t i = 0; i + 1 < n; i += 2) {
    uint32_t u = bigEndian ? be16(p + i) : le16(p + i);
    if (u == 0)
      break;
    if (u >= 0xD800 && u < 0xDC00 && i + 3 < n) {  // surrogate pair
      uint32_t lo = bigEndian ? be16(p + i + 2) : le16(p + i + 2);
      if (lo >= 0xDC00 && lo < 0xE000) {
        u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
        i += 2;
      }
    }
    appendUtf8(s, u);
  }
  return s;
}

bool validUtf8(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n && p[i];) {
    uint8_t c = p[i];
    size_t k = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2
             : (c & 0xF8) == 0xF0 ? 3 : 99;
    if (k == 99 || i + k >= n + (k ? 0 : 1))
      return false;
    for (size_t j = 1; j <= k; j++)
      if ((p[i + j] & 0xC0) != 0x80)
        return false;
    i += k + 1;
  }
  return true;
}

// Legacy tags (ID3v1, RIFF INFO) are Latin-1 by spec, but many tools write
// UTF-8.  Keep valid UTF-8 as it is, convert everything else from Latin-1.
std::string legacyToUtf8(const uint8_t* p, size_t n) {
  if (validUtf8(p, n)) {
    size_t k = 0;
    while (k < n && p[k])
      k++;
    return std::string(reinterpret_cast<const char*>(p), k);
  }
  return latin1ToUtf8(p, n);
}

std::string utf8Until0(const uint8_t* p, size_t n) {
  size_t k = 0;
  while (k < n && p[k])
    k++;
  return std::string(reinterpret_cast<const char*>(p), k);
}

void trim(std::string& s) {
  while (!s.empty() && (unsigned char)s.back() <= ' ')
    s.pop_back();
  size_t i = 0;
  while (i < s.size() && (unsigned char)s[i] <= ' ')
    i++;
  s.erase(0, i);
}

std::string lower(std::string s) {
  for (auto& c : s)
    c = (char)std::tolower((unsigned char)c);
  return s;
}

int parseTrackNo(const std::string& s) {
  int v = 0;
  for (char c : s) {
    if (c < '0' || c > '9')
      break;
    v = v * 10 + (c - '0');
    if (v > 9999)
      break;
  }
  return v;
}

void setIfEmpty(std::string& dst, std::string v) {
  trim(v);
  if (dst.empty() && !v.empty())
    dst = std::move(v);
}

// "KEY=value" (Vorbis comment / FLAC / Opus tags)
void applyComment(AudioFileInfo& o, const char* s, size_t n) {
  const char* eq = static_cast<const char*>(memchr(s, '=', n));
  if (!eq)
    return;
  std::string key = lower(std::string(s, eq - s));
  std::string val(eq + 1, n - size_t(eq + 1 - s));
  if (key == "title")
    setIfEmpty(o.title, val);
  else if (key == "artist")
    setIfEmpty(o.artist, val);
  else if (key == "albumartist" || key == "album artist")
    setIfEmpty(o.artist, val);  // only used when ARTIST is missing (first wins)
  else if (key == "album")
    setIfEmpty(o.album, val);
  else if (key == "tracknumber" && o.trackNo == 0)
    o.trackNo = parseTrackNo(val);
}

// Vorbis-comment structure (little endian): vendor, count, [len, "K=V"]...
void parseVorbisComments(AudioFileInfo& o, const uint8_t* p, size_t n) {
  if (n < 8)
    return;
  uint32_t vlen = le32(p);
  size_t pos = 4 + size_t(vlen);
  if (pos + 4 > n)
    return;
  uint32_t count = le32(p + pos);
  pos += 4;
  for (uint32_t i = 0; i < count && pos + 4 <= n; i++) {
    uint32_t len = le32(p + pos);
    pos += 4;
    if (len > n - pos)
      break;
    if (len < kMaxTextFrame)
      applyComment(o, reinterpret_cast<const char*>(p + pos), len);
    pos += len;
  }
}

// ============================================================================
//  ID3
// ============================================================================

// Returns the size of an ID3v2 tag at `off` (0 if there is none).
uint64_t id3v2Size(FILE* f, uint64_t off) {
  uint8_t h[10];
  if (!readAt(f, off, h, 10) || memcmp(h, "ID3", 3) != 0 || h[3] == 0xFF ||
      h[4] == 0xFF)
    return 0;
  uint64_t sz = 10 + uint64_t(syncsafe32(h + 6));
  if (h[5] & 0x10)
    sz += 10;  // footer present
  return sz;
}

std::string id3Text(const uint8_t* d, size_t n) {
  if (n < 1)
    return {};
  uint8_t enc = d[0];
  d++;
  n--;
  switch (enc) {
    case 0:
      return latin1ToUtf8(d, n);
    case 1:  // UTF-16 with BOM
      if (n >= 2 && d[0] == 0xFE && d[1] == 0xFF)
        return utf16ToUtf8(d + 2, n - 2, true);
      if (n >= 2 && d[0] == 0xFF && d[1] == 0xFE)
        return utf16ToUtf8(d + 2, n - 2, false);
      return utf16ToUtf8(d, n, false);
    case 2:
      return utf16ToUtf8(d, n, true);
    case 3:
      return utf8Until0(d, n);
    default:
      return {};
  }
}

void parseId3v2(FILE* f, uint64_t off, uint64_t tagSize, AudioFileInfo& o) {
  uint8_t h[10];
  if (!readAt(f, off, h, 10))
    return;
  const uint8_t ver = h[3];
  const uint8_t flags = h[5];
  if (ver < 2 || ver > 4)
    return;
  uint64_t end = off + 10 + syncsafe32(h + 6);
  uint64_t pos = off + 10;
  if ((flags & 0x40) && ver >= 3) {  // extended header
    uint8_t e[4];
    if (!readAt(f, pos, e, 4))
      return;
    pos += (ver == 4) ? syncsafe32(e) : be32(e) + 4;
  }
  (void)tagSize;
  const size_t hdrLen = (ver == 2) ? 6 : 10;
  std::vector<uint8_t> buf;
  while (pos + hdrLen <= end) {
    uint8_t fh[10];
    if (!readAt(f, pos, fh, hdrLen))
      return;
    if (fh[0] == 0)
      break;  // padding
    char id[5] = {0};
    uint32_t sz;
    if (ver == 2) {
      memcpy(id, fh, 3);
      sz = be24(fh + 3);
    } else {
      memcpy(id, fh, 4);
      sz = (ver == 4) ? syncsafe32(fh + 4) : be32(fh + 4);
    }
    pos += hdrLen;
    if (sz == 0 || pos + sz > end)
      break;
    std::string* dst = nullptr;
    bool isTrack = false;
    if (!strcmp(id, "TIT2") || !strcmp(id, "TT2"))
      dst = &o.title;
    else if (!strcmp(id, "TPE1") || !strcmp(id, "TP1"))
      dst = &o.artist;
    else if (!strcmp(id, "TALB") || !strcmp(id, "TAL"))
      dst = &o.album;
    else if (!strcmp(id, "TRCK") || !strcmp(id, "TRK"))
      isTrack = true;
    if ((dst || isTrack) && sz < kMaxTextFrame) {
      buf.resize(sz);
      if (readAt(f, pos, buf.data(), sz)) {
        std::string v = id3Text(buf.data(), sz);
        trim(v);
        if (isTrack) {
          if (!o.trackNo)
            o.trackNo = parseTrackNo(v);
        } else if (dst->empty()) {
          *dst = v;
        }
      }
    }
    pos += sz;
  }
}

// ID3v1 + APEv2 at the end of the file: shrink audioEnd, take tags if missing.
void parseTailTags(FILE* f, AudioFileInfo& o) {
  if (o.audioEnd >= 128 + o.audioStart) {
    uint8_t t[128];
    if (readAt(f, o.audioEnd - 128, t, 128) && memcmp(t, "TAG", 3) == 0) {
      o.audioEnd -= 128;
      setIfEmpty(o.title, legacyToUtf8(t + 3, 30));
      setIfEmpty(o.artist, legacyToUtf8(t + 33, 30));
      setIfEmpty(o.album, legacyToUtf8(t + 63, 30));
      if (!o.trackNo && t[125] == 0 && t[126] != 0)
        o.trackNo = t[126];
    }
  }
  if (o.audioEnd >= 32 + o.audioStart) {
    uint8_t a[32];
    if (readAt(f, o.audioEnd - 32, a, 32) && memcmp(a, "APETAGEX", 8) == 0) {
      uint64_t sz = le32(a + 12);
      if (le32(a + 20) & 0x80000000u)
        sz += 32;  // header present
      if (sz <= o.audioEnd - o.audioStart)
        o.audioEnd -= sz;
    }
  }
}

// ============================================================================
//  MP3
// ============================================================================
struct Mp3Hdr {
  int version = 0;  // 1 = MPEG1, 2 = MPEG2, 25 = MPEG2.5
  int layer = 0;
  uint32_t bitrate = 0;  // kbps
  uint32_t sampleRate = 0;
  uint32_t frameLen = 0;
  uint32_t samplesPerFrame = 0;
  bool mono = false;
};

bool parseMp3Header(uint32_t h, Mp3Hdr& o) {
  if ((h >> 21) != 0x7FF)
    return false;
  const int verBits = (h >> 19) & 3, layerBits = (h >> 17) & 3;
  const int brIdx = (h >> 12) & 0xF, srIdx = (h >> 10) & 3, pad = (h >> 9) & 1;
  if (verBits == 1 || layerBits == 0 || brIdx == 0 || brIdx == 15 || srIdx == 3)
    return false;
  static const uint16_t br[2][3][15] = {
      {// MPEG1: L1, L2, L3
       {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
       {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
       {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}},
      {// MPEG2/2.5: L1, L2, L3
       {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
       {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
       {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}}};
  static const uint32_t sr[3] = {44100, 48000, 32000};
  o.version = verBits == 3 ? 1 : (verBits == 2 ? 2 : 25);
  o.layer = 4 - layerBits;
  o.bitrate = br[o.version == 1 ? 0 : 1][o.layer - 1][brIdx];
  o.sampleRate = sr[srIdx] >> (o.version == 1 ? 0 : (o.version == 2 ? 1 : 2));
  o.mono = ((h >> 6) & 3) == 3;
  if (o.layer == 1) {
    o.samplesPerFrame = 384;
    o.frameLen = (12 * o.bitrate * 1000 / o.sampleRate + pad) * 4;
  } else {
    o.samplesPerFrame = (o.layer == 3 && o.version != 1) ? 576 : 1152;
    o.frameLen = o.samplesPerFrame / 8 * o.bitrate * 1000 / o.sampleRate + pad;
  }
  return o.frameLen >= 21;
}

bool probeMp3(FILE* f, AudioFileInfo& o) {
  // find the first frame whose successor is also a valid frame
  const size_t kScan = 64 * 1024;
  std::vector<uint8_t> b(kScan + 4);
  size_t got = readSomeAt(f, o.audioStart, b.data(), b.size());
  if (got < 4)
    return false;
  for (size_t i = 0; i + 4 <= got; i++) {
    if (b[i] != 0xFF || (b[i + 1] & 0xE0) != 0xE0)
      continue;
    Mp3Hdr h;
    if (!parseMp3Header(be32(&b[i]), h))
      continue;
    uint8_t n4[4];
    uint64_t next = o.audioStart + i + h.frameLen;
    Mp3Hdr h2;
    if (next + 4 <= o.audioEnd) {
      if (!readAt(f, next, n4, 4) || !parseMp3Header(be32(n4), h2) ||
          h2.version != h.version || h2.layer != h.layer ||
          h2.sampleRate != h.sampleRate)
        continue;
    }
    // found the first frame
    o.audioStart += i;
    o.codec = Codec::MP3;
    o.sampleRate = h.sampleRate;
    o.channels = h.mono ? 1 : 2;
    o.seekable = true;
    // Xing / Info / VBRI
    uint8_t fr[200];
    size_t fl = readSomeAt(f, o.audioStart, fr, sizeof(fr));
    size_t side = h.version == 1 ? (h.mono ? 17 : 32) : (h.mono ? 9 : 17);
    uint32_t frames = 0;
    size_t xo = 4 + side;
    if (xo + 120 <= fl &&
        (!memcmp(fr + xo, "Xing", 4) || !memcmp(fr + xo, "Info", 4))) {
      uint32_t flags = be32(fr + xo + 4);
      size_t p = xo + 8;
      if (flags & 1) {
        frames = be32(fr + p);
        p += 4;
      }
      if (flags & 2)
        p += 4;
      if ((flags & 4) && p + 100 <= fl)
        o.toc.assign(fr + p, fr + p + 100);
    } else if (36 + 18 <= fl && !memcmp(fr + 36, "VBRI", 4)) {
      frames = be32(fr + 36 + 14);
    }
    uint64_t bytes = o.audioEnd - o.audioStart;
    if (frames) {
      o.durationMs =
          uint32_t(uint64_t(frames) * h.samplesPerFrame * 1000 / h.sampleRate);
      if (o.durationMs)
        o.bitrateKbps = uint32_t(bytes * 8 / o.durationMs);
    } else {
      o.bitrateKbps = h.bitrate;
      o.durationMs = uint32_t(bytes * 8 / h.bitrate);
    }
    return true;
  }
  return false;
}

// ============================================================================
//  FLAC
// ============================================================================
bool probeFlac(FILE* f, AudioFileInfo& o) {
  uint8_t m[4];
  if (!readAt(f, o.audioStart, m, 4) || memcmp(m, "fLaC", 4) != 0)
    return false;
  uint64_t pos = o.audioStart + 4;
  bool haveInfo = false;
  uint64_t totalSamples = 0;
  uint8_t si[34];
  for (int guard = 0; guard < 256; guard++) {
    uint8_t bh[4];
    if (!readAt(f, pos, bh, 4))
      return false;
    const bool last = bh[0] & 0x80;
    const uint8_t type = bh[0] & 0x7F;
    const uint32_t len = be24(bh + 1);
    pos += 4;
    if (type == 0 && len >= 34) {  // STREAMINFO
      if (!readAt(f, pos, si, 34))
        return false;
      o.sampleRate = (uint32_t(si[10]) << 12) | (uint32_t(si[11]) << 4) |
                     (si[12] >> 4);
      o.channels = ((si[12] >> 1) & 7) + 1;
      o.bitsPerSample = (((si[12] & 1) << 4) | (si[13] >> 4)) + 1;
      totalSamples = (uint64_t(si[13] & 0x0F) << 32) | be32(si + 14);
      haveInfo = true;
    } else if (type == 4 && len < 256 * 1024) {  // VORBIS_COMMENT
      std::vector<uint8_t> c(len);
      if (readAt(f, pos, c.data(), len))
        parseVorbisComments(o, c.data(), len);
    }
    pos += len;
    if (last)
      break;
  }
  if (!haveInfo || !o.sampleRate)
    return false;
  o.codec = Codec::FLAC;
  o.audioStart = pos;  // first frame
  if (totalSamples)
    o.durationMs = uint32_t(totalSamples * 1000 / o.sampleRate);
  if (o.durationMs)
    o.bitrateKbps = uint32_t((o.audioEnd - o.audioStart) * 8 / o.durationMs);
  // decoder needs "fLaC" + STREAMINFO (flagged as last block) before frames
  o.prefixBytes = {'f', 'L', 'a', 'C', 0x80, 0x00, 0x00, 34};
  o.prefixBytes.insert(o.prefixBytes.end(), si, si + 34);
  o.seekable = true;
  return true;
}

// ============================================================================
//  WAV
// ============================================================================
bool probeWav(FILE* f, AudioFileInfo& o) {
  uint8_t h[12];
  if (!readAt(f, 0, h, 12) || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
    return false;
  uint64_t pos = 12;
  uint16_t fmtTag = 0, blockAlign = 0;
  uint32_t byteRate = 0;
  bool haveFmt = false;
  for (int guard = 0; guard < 64 && pos + 8 <= o.fileSize; guard++) {
    uint8_t ch[8];
    if (!readAt(f, pos, ch, 8))
      break;
    uint32_t len = le32(ch + 4);
    if (!memcmp(ch, "fmt ", 4) && len >= 16) {
      uint8_t fm[40] = {0};
      if (!readAt(f, pos + 8, fm, std::min<uint32_t>(len, 40)))
        return false;
      fmtTag = le16(fm);
      o.channels = (uint8_t)le16(fm + 2);
      o.sampleRate = le32(fm + 4);
      byteRate = le32(fm + 8);
      blockAlign = le16(fm + 12);
      o.bitsPerSample = (uint8_t)le16(fm + 14);
      if (fmtTag == 0xFFFE && len >= 40)
        fmtTag = le16(fm + 24);  // WAVE_FORMAT_EXTENSIBLE sub format
      haveFmt = true;
    } else if (!memcmp(ch, "LIST", 4) && len >= 4 && len < 64 * 1024) {
      std::vector<uint8_t> l(len);
      if (readAt(f, pos + 8, l.data(), len) && !memcmp(l.data(), "INFO", 4)) {
        size_t p = 4;
        while (p + 8 <= len) {
          uint32_t sl = le32(&l[p + 4]);
          if (sl > len - p - 8)
            break;
          std::string v = legacyToUtf8(&l[p + 8], sl);
          if (!memcmp(&l[p], "INAM", 4))
            setIfEmpty(o.title, v);
          else if (!memcmp(&l[p], "IART", 4))
            setIfEmpty(o.artist, v);
          else if (!memcmp(&l[p], "IPRD", 4))
            setIfEmpty(o.album, v);
          else if (!memcmp(&l[p], "ITRK", 4) && !o.trackNo)
            o.trackNo = parseTrackNo(v);
          p += 8 + sl + (sl & 1);
        }
      }
    } else if (!memcmp(ch, "data", 4)) {
      if (!haveFmt)
        return false;
      o.codec = Codec::WAV;
      o.audioStart = pos + 8;
      uint64_t end = o.audioStart + len;
      if (len == 0 || len == 0xFFFFFFFFu || end > o.fileSize)
        end = o.fileSize;  // streaming writers / truncated files
      o.audioEnd = end;
      if (byteRate) {
        o.durationMs =
            uint32_t((o.audioEnd - o.audioStart) * 1000 / byteRate);
        o.bitrateKbps = byteRate * 8 / 1000;
      }
      o.blockAlign = blockAlign ? blockAlign : 1;
      o.prefixFileLen = o.audioStart;
      o.seekable = o.audioStart <= kMaxWavPrefix && byteRate;
      (void)fmtTag;
      return true;
    }
    pos += 8 + uint64_t(len) + (len & 1);
  }
  return false;
}

// ============================================================================
//  Ogg (Vorbis / Opus)
// ============================================================================
struct OggPage {
  uint64_t offset = 0;
  uint64_t granule = 0;
  uint32_t serial = 0;
  uint8_t type = 0;
  uint8_t nseg = 0;
  uint8_t seg[255];
  uint32_t headerLen = 0;
  uint32_t bodyLen = 0;
};

bool readOggPage(FILE* f, uint64_t off, OggPage& p) {
  uint8_t h[27];
  if (!readAt(f, off, h, 27) || memcmp(h, "OggS", 4) != 0 || h[4] != 0)
    return false;
  p.offset = off;
  p.type = h[5];
  p.granule = le64(h + 6);
  p.serial = le32(h + 14);
  p.nseg = h[26];
  if (p.nseg && fread(p.seg, 1, p.nseg, f) != p.nseg)
    return false;
  p.headerLen = 27 + p.nseg;
  p.bodyLen = 0;
  for (int i = 0; i < p.nseg; i++)
    p.bodyLen += p.seg[i];
  return true;
}

bool probeOgg(FILE* f, AudioFileInfo& o) {
  OggPage pg;
  if (!readOggPage(f, 0, pg))
    return false;
  const uint32_t serial = pg.serial;
  uint64_t off = 0;
  std::vector<uint8_t> pkt[2];
  size_t pktLen[2] = {0, 0};
  int pktIdx = 0;
  uint32_t preSkip = 0;
  uint64_t audioStart = 0;
  for (int guard = 0; guard < 256; guard++) {
    if (!readOggPage(f, off, pg))
      return false;
    if (pg.serial == serial && pg.granule != 0 && pg.granule != ~0ull &&
        pktIdx >= 2) {
      audioStart = off;  // first audio page
      break;
    }
    if (pg.serial == serial && pktIdx < 2) {
      std::vector<uint8_t> body(pg.bodyLen);
      if (pg.bodyLen &&
          !readAt(f, off + pg.headerLen, body.data(), pg.bodyLen))
        return false;
      size_t bp = 0;
      for (int s = 0; s < pg.nseg && pktIdx < 2; s++) {
        size_t l = pg.seg[s];
        if (pkt[pktIdx].size() + l <= kMaxPacket)
          pkt[pktIdx].insert(pkt[pktIdx].end(), body.begin() + bp,
                             body.begin() + bp + l);
        pktLen[pktIdx] += l;
        bp += l;
        if (l < 255)
          pktIdx++;  // packet complete
      }
    }
    off += pg.headerLen + pg.bodyLen;
    if (off > kMaxOggPrefix * 4 || off >= o.fileSize)
      return false;
  }
  if (!audioStart || pkt[0].size() < 8)
    return false;

  const auto& id = pkt[0];
  uint32_t granuleRate = 0;
  if (id.size() >= 30 && id[0] == 1 && !memcmp(&id[1], "vorbis", 6)) {
    o.codec = Codec::OggVorbis;
    o.channels = id[11];
    o.sampleRate = le32(&id[12]);
    o.bitrateKbps = le32(&id[20]) / 1000;  // nominal
    granuleRate = o.sampleRate;
    if (pkt[1].size() > 7 && pkt[1][0] == 3 && !memcmp(&pkt[1][1], "vorbis", 6))
      parseVorbisComments(o, pkt[1].data() + 7, pkt[1].size() - 7);
  } else if (id.size() >= 19 && !memcmp(id.data(), "OpusHead", 8)) {
    o.codec = Codec::Opus;
    o.channels = id[9];
    preSkip = le16(&id[10]);
    o.sampleRate = le32(&id[12]) ? le32(&id[12]) : 48000;
    granuleRate = 48000;
    if (pkt[1].size() > 8 && !memcmp(pkt[1].data(), "OpusTags", 8))
      parseVorbisComments(o, pkt[1].data() + 8, pkt[1].size() - 8);
  } else {
    return false;
  }
  o.audioStart = audioStart;
  o.prefixFileLen = audioStart;  // header pages are re-sent before a seek

  // duration: granule position of the last page of this stream
  const uint64_t tail = std::min<uint64_t>(o.fileSize, 64 * 1024);
  std::vector<uint8_t> t(tail);
  if (granuleRate && tail >= 27 &&
      readAt(f, o.fileSize - tail, t.data(), tail)) {
    for (size_t i = tail - 27;; i--) {
      if (t[i] == 'O' && !memcmp(&t[i], "OggS", 4) && t[i + 4] == 0 &&
          le32(&t[i + 14]) == serial) {
        uint64_t g = le64(&t[i + 6]);
        if (g != ~0ull && g > preSkip) {
          o.durationMs = uint32_t((g - preSkip) * 1000 / granuleRate);
          break;
        }
      }
      if (i == 0)
        break;
    }
  }
  if (o.durationMs)
    o.bitrateKbps = uint32_t((o.audioEnd - o.audioStart) * 8 / o.durationMs);
  o.seekable = o.durationMs && o.prefixFileLen <= kMaxOggPrefix;
  return true;
}

// ============================================================================
//  AAC (ADTS)
// ============================================================================
bool probeAdts(FILE* f, AudioFileInfo& o) {
  static const uint32_t sr[13] = {96000, 88200, 64000, 48000, 44100,
                                  32000, 24000, 22050, 16000, 12000,
                                  11025, 8000,  7350};
  std::vector<uint8_t> b(64 * 1024);
  size_t got = readSomeAt(f, o.audioStart, b.data(), b.size());
  size_t i = 0;
  while (i + 7 <= got && !(b[i] == 0xFF && (b[i + 1] & 0xF6) == 0xF0))
    i++;
  if (i + 7 > got)
    return false;
  const uint64_t first = o.audioStart + i;
  uint64_t pos = first;
  uint64_t sum = 0;
  int frames = 0;
  uint32_t rate = 0;
  for (; frames < 256 && pos + 7 <= o.audioEnd; frames++) {
    uint8_t h[7];
    if (!readAt(f, pos, h, 7) || h[0] != 0xFF || (h[1] & 0xF6) != 0xF0)
      break;
    uint8_t srIdx = (h[2] >> 2) & 0xF;
    if (srIdx >= 13)
      break;
    rate = sr[srIdx];
    o.channels = uint8_t(((h[2] & 1) << 2) | (h[3] >> 6));
    uint32_t len = ((h[3] & 3) << 11) | (h[4] << 3) | (h[5] >> 5);
    if (len < 7)
      break;
    sum += len;
    pos += len;
  }
  if (frames < 2 || !rate)
    return false;
  o.codec = Codec::AAC;
  o.audioStart = first;
  o.sampleRate = rate;
  const double avg = double(sum) / frames;
  o.durationMs = uint32_t(double(o.audioEnd - first) / avg * 1024.0 * 1000.0 /
                          rate);
  if (o.durationMs)
    o.bitrateKbps = uint32_t((o.audioEnd - first) * 8 / o.durationMs);
  o.seekable = true;
  return true;
}

// ============================================================================
//  MP4 / M4A
// ============================================================================
struct Box {
  uint64_t start = 0, size = 0;
  uint32_t hdr = 8;
  char type[5] = {0};
  uint64_t body() const { return start + hdr; }
  uint64_t end() const { return start + size; }
};

bool readBox(FILE* f, uint64_t off, uint64_t limit, Box& b) {
  uint8_t h[16];
  if (off + 8 > limit || !readAt(f, off, h, 8))
    return false;
  b.start = off;
  b.size = be32(h);
  memcpy(b.type, h + 4, 4);
  b.hdr = 8;
  if (b.size == 1) {
    if (!readAt(f, off + 8, h + 8, 8))
      return false;
    b.size = be64(h + 8);
    b.hdr = 16;
  } else if (b.size == 0) {
    b.size = limit - off;
  }
  return b.size >= b.hdr && off + b.size <= limit;
}

bool findChild(FILE* f, const Box& parent, uint64_t skip, const char* type,
               Box& out) {
  uint64_t p = parent.body() + skip;
  Box b;
  while (readBox(f, p, parent.end(), b)) {
    if (!memcmp(b.type, type, 4)) {
      out = b;
      return true;
    }
    p = b.end();
  }
  return false;
}

void parseIlst(FILE* f, const Box& ilst, AudioFileInfo& o) {
  uint64_t p = ilst.body();
  Box item;
  while (readBox(f, p, ilst.end(), item)) {
    Box data;
    if (findChild(f, item, 0, "data", data) && data.size > data.hdr + 8) {
      uint64_t vlen = data.size - data.hdr - 8;
      if (vlen < kMaxTextFrame) {
        std::vector<uint8_t> v(vlen);
        if (readAt(f, data.body() + 8, v.data(), vlen)) {
          std::string s(v.begin(), v.end());
          const uint8_t* t = reinterpret_cast<const uint8_t*>(item.type);
          if (t[0] == 0xA9 && !memcmp(item.type + 1, "nam", 3))
            setIfEmpty(o.title, s);
          else if (t[0] == 0xA9 && !memcmp(item.type + 1, "ART", 3))
            setIfEmpty(o.artist, s);
          else if (!memcmp(item.type, "aART", 4))
            setIfEmpty(o.artist, s);
          else if (t[0] == 0xA9 && !memcmp(item.type + 1, "alb", 3))
            setIfEmpty(o.album, s);
          else if (!memcmp(item.type, "trkn", 4) && vlen >= 4 && !o.trackNo)
            o.trackNo = be16(&v[2]);
        }
      }
    }
    p = item.end();
  }
}

bool probeMp4(FILE* f, AudioFileInfo& o) {
  Box b, moov, mdat;
  bool haveMoov = false, haveMdat = false, sawFtyp = false;
  uint64_t p = 0;
  for (int guard = 0; guard < 64 && readBox(f, p, o.fileSize, b); guard++) {
    if (!memcmp(b.type, "ftyp", 4))
      sawFtyp = true;
    else if (!memcmp(b.type, "moov", 4) && !haveMoov) {
      moov = b;
      haveMoov = true;
    } else if (!memcmp(b.type, "mdat", 4) && !haveMdat) {
      mdat = b;
      haveMdat = true;
    }
    p = b.end();
  }
  if (!sawFtyp || !haveMoov)
    return false;
  o.codec = Codec::M4A;
  Box mvhd;
  if (findChild(f, moov, 0, "mvhd", mvhd)) {
    uint8_t m[32];
    if (readAt(f, mvhd.body(), m, 32)) {
      uint64_t scale, dur;
      if (m[0] == 1) {
        scale = be32(m + 20);
        dur = be64(m + 24);
      } else {
        scale = be32(m + 12);
        dur = be32(m + 16);
      }
      if (scale)
        o.durationMs = uint32_t(dur * 1000 / scale);
    }
  }
  // first audio sample entry: moov/trak/mdia/minf/stbl/stsd
  Box trak, mdia, minf, stbl, stsd;
  uint64_t tp = moov.body();
  while (readBox(f, tp, moov.end(), trak)) {
    tp = trak.end();
    if (memcmp(trak.type, "trak", 4))
      continue;
    if (findChild(f, trak, 0, "mdia", mdia) &&
        findChild(f, mdia, 0, "minf", minf) &&
        findChild(f, minf, 0, "stbl", stbl) &&
        findChild(f, stbl, 0, "stsd", stsd)) {
      uint8_t e[36];
      if (readAt(f, stsd.body() + 8, e, 36) &&
          (!memcmp(e + 4, "mp4a", 4) || !memcmp(e + 4, "alac", 4))) {
        o.channels = (uint8_t)be16(e + 24);
        o.bitsPerSample = !memcmp(e + 4, "alac", 4) ? (uint8_t)be16(e + 26) : 0;
        o.sampleRate = be32(e + 32) >> 16;
        break;
      }
    }
  }
  Box udta, meta, ilst;
  if (findChild(f, moov, 0, "udta", udta) &&
      findChild(f, udta, 0, "meta", meta) &&
      findChild(f, meta, 4, "ilst", ilst))  // meta is a full box (+4)
    parseIlst(f, ilst, o);
  if (haveMdat && o.durationMs)
    o.bitrateKbps = uint32_t(mdat.size * 8 / o.durationMs);
  // the decoder streams MP4 front to back; jumping needs the sample tables
  o.audioStart = 0;
  o.seekable = false;
  return true;
}

}  // namespace

// ============================================================================
//  public API
// ============================================================================
const char* codecName(Codec c) {
  switch (c) {
    case Codec::MP3:
      return "MP3";
    case Codec::FLAC:
      return "FLAC";
    case Codec::WAV:
      return "WAV";
    case Codec::OggVorbis:
      return "Ogg Vorbis";
    case Codec::Opus:
      return "Opus";
    case Codec::AAC:
      return "AAC";
    case Codec::M4A:
      return "M4A";
    case Codec::WMA:
      return "WMA";
    case Codec::MIDI:
      return "MIDI";
    default:
      return "?";
  }
}

Codec codecFromExtension(const std::string& name) {
  auto dot = name.find_last_of('.');
  if (dot == std::string::npos)
    return Codec::Unknown;
  std::string e = lower(name.substr(dot + 1));
  if (e == "mp3" || e == "mp2" || e == "mpga")
    return Codec::MP3;
  if (e == "flac" || e == "fla")
    return Codec::FLAC;
  if (e == "wav" || e == "wave")
    return Codec::WAV;
  if (e == "ogg" || e == "oga")
    return Codec::OggVorbis;
  if (e == "opus")
    return Codec::Opus;
  if (e == "aac" || e == "adts")
    return Codec::AAC;
  if (e == "m4a" || e == "mp4" || e == "m4b")
    return Codec::M4A;
  if (e == "wma")
    return Codec::WMA;
  if (e == "mid" || e == "midi")
    return Codec::MIDI;
  return Codec::Unknown;
}

bool isPlayableFile(const std::string& name) {
  // macOS resource forks ("._Song.mp3") look like audio files but are not
  auto slash = name.find_last_of('/');
  const char* base = name.c_str() + (slash == std::string::npos ? 0 : slash + 1);
  if (base[0] == '.')
    return false;
  // Opus is not supported by the VS10xx decoders
  Codec c = codecFromExtension(name);
  return c != Codec::Unknown && c != Codec::Opus;
}

std::string titleFromFileName(const std::string& path) {
  auto slash = path.find_last_of('/');
  std::string s = path.substr(slash == std::string::npos ? 0 : slash + 1);
  auto dot = s.find_last_of('.');
  if (dot != std::string::npos && dot > 0)
    s.erase(dot);
  // "01 - Title", "01. Title", "1-03 Title", "01_Title"
  size_t i = 0;
  while (i < s.size() && i < 4 && std::isdigit((unsigned char)s[i]))
    i++;
  if (i > 0 && i < s.size()) {
    size_t j = i;
    if (j < s.size() && s[j] == '-' && j + 1 < s.size() &&
        std::isdigit((unsigned char)s[j + 1])) {  // disc-track "1-03"
      j++;
      while (j < s.size() && std::isdigit((unsigned char)s[j]))
        j++;
    }
    size_t k = j;
    while (k < s.size() && (s[k] == ' ' || s[k] == '-' || s[k] == '.' ||
                            s[k] == '_'))
      k++;
    if (k > j && k < s.size())
      s.erase(0, k);
  }
  for (auto& c : s)
    if (c == '_')
      c = ' ';
  trim(s);
  return s;
}

uint64_t AudioFileInfo::offsetForMs(uint32_t ms) const {
  if (!ms || !seekable || !durationMs || audioEnd <= audioStart)
    return audioStart;
  if (ms >= durationMs)
    ms = durationMs - 1;
  const uint64_t span = audioEnd - audioStart;
  uint64_t rel;
  if (toc.size() == 100) {
    // VBR MP3: Xing table of contents, 100 points of (percent -> 1/256 file)
    double pct = 100.0 * double(ms) / double(durationMs);
    int a = std::min(99, int(pct));
    double fa = toc[a], fb = a < 99 ? toc[a + 1] : 256.0;
    double fx = fa + (fb - fa) * (pct - a);
    rel = uint64_t(fx / 256.0 * double(span));
  } else {
    rel = uint64_t(double(span) * double(ms) / double(durationMs));
  }
  if (blockAlign > 1)
    rel = rel / blockAlign * blockAlign;
  if (rel >= span)
    rel = span ? span - 1 : 0;
  return audioStart + rel;
}

std::string AudioFileInfo::qualityString() const {
  char buf[64];
  std::string s = codecName(codec);
  if (codec == Codec::M4A && bitsPerSample)
    s = "ALAC";
  const bool lossless = codec == Codec::FLAC || codec == Codec::WAV ||
                        (codec == Codec::M4A && bitsPerSample);
  if (lossless && bitsPerSample && sampleRate) {
    const unsigned r = (unsigned)((sampleRate + 50) / 100);  // 22050 -> 22.1
    snprintf(buf, sizeof(buf), " - %u-Bit / %u.%u kHz", (unsigned)bitsPerSample,
             r / 10, r % 10);
    s += buf;
  } else {
    if (bitrateKbps) {
      snprintf(buf, sizeof(buf), " - %u kbps", (unsigned)bitrateKbps);
      s += buf;
    }
    if (sampleRate) {
      const unsigned r = (unsigned)((sampleRate + 50) / 100);
      snprintf(buf, sizeof(buf), " / %u.%u kHz", r / 10, r % 10);
      s += buf;
    }
  }
  return s;
}

bool probe(FILE* f, const std::string& path, AudioFileInfo& o) {
  o = AudioFileInfo();
  if (!f)
    return false;
  if (fseek(f, 0, SEEK_END) != 0)
    return false;
  long sz = ftell(f);
  if (sz <= 0)
    return false;
  o.fileSize = uint64_t(sz);
  o.audioEnd = o.fileSize;

  // leading ID3v2 (MP3, AAC, sometimes FLAC)
  uint64_t id3 = id3v2Size(f, 0);
  if (id3 && id3 < o.fileSize) {
    parseId3v2(f, 0, id3, o);
    o.audioStart = id3;
  }

  uint8_t m[12] = {0};
  readSomeAt(f, o.audioStart, m, sizeof(m));
  const Codec ext = codecFromExtension(path);
  bool ok = false;

  if (!memcmp(m, "fLaC", 4))
    ok = probeFlac(f, o);
  else if (!memcmp(m, "OggS", 4))
    ok = probeOgg(f, o);
  else if (!memcmp(m, "RIFF", 4) && !memcmp(m + 8, "WAVE", 4))
    ok = probeWav(f, o);
  else if (!memcmp(m + 4, "ftyp", 4))
    ok = probeMp4(f, o);
  else if (!memcmp(m, "MThd", 4)) {
    o.codec = Codec::MIDI;
    ok = true;
  } else if (m[0] == 0x30 && m[1] == 0x26 && m[2] == 0xB2 && m[3] == 0x75) {
    o.codec = Codec::WMA;  // ASF header GUID
    ok = true;
  } else {
    // MP3 / ADTS: strip ID3v1 / APE tags at the end first
    parseTailTags(f, o);
    if (ext != Codec::MP3)
      ok = probeAdts(f, o);  // .aac first, an MP3 sync can look alike
    if (!ok)
      ok = probeMp3(f, o);
    if (!ok && ext == Codec::AAC)
      ok = probeAdts(f, o);
  }
  if (!ok) {
    // unknown content: still let the decoder try if the extension says so
    if (ext == Codec::Unknown || ext == Codec::Opus)
      return false;
    o = AudioFileInfo();
    o.fileSize = uint64_t(sz);
    o.audioEnd = o.fileSize;
    o.codec = ext;
    o.seekable = false;
  }
  if (o.title.empty())
    o.title = titleFromFileName(path);
  return true;
}

bool probeFile(const std::string& path, AudioFileInfo& out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f)
    return false;
  bool ok = probe(f, path, out);
  fclose(f);
  return ok;
}

}  // namespace sdfile
