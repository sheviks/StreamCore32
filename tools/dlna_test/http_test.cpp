// PC test of DlnaHttp (HttpConn / HttpFile) + the probe over HTTP.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "AudioFileInfo.h"
#include "DlnaHttp.h"

using namespace dlna;
static int fails = 0;
static void check(bool ok, const std::string& what) {
  printf("%-60s %s\n", what.c_str(), ok ? "OK" : "FAIL");
  if (!ok) fails++;
}
static std::vector<uint8_t> load(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}
static std::vector<uint8_t> readAll(HttpFile& f) {
  std::vector<uint8_t> out;
  uint8_t b[7000];
  for (;;) {
    int r = f.readAt(out.size(), b, sizeof b);
    if (r <= 0) break;
    out.insert(out.end(), b, b + r);
  }
  return out;
}
int main(int argc, char** argv) {
  std::string dir = argv[1], base = "http://127.0.0.1:" + std::string(argv[2]);
  auto flac = load(dir + "/t.flac");
  for (const char* mode : {"f", "norange", "redir", "chunked", "live", "drop"}) {
    HttpFile f;
    bool ok = f.open(base + "/" + mode + "/t.flac");
    check(ok, std::string(mode) + ": open");
    auto all = readAll(f);
    check(all == flac, std::string(mode) + ": whole file (" + std::to_string(all.size()) + ")");
    bool seekExpected = !strcmp(mode, "f") || !strcmp(mode, "redir") || !strcmp(mode, "drop");
    check(f.seekable() == seekExpected, std::string(mode) + ": seekable=" + std::to_string(f.seekable()));
  }
  {  // random access: tail, middle, back into the head cache
    HttpFile f;
    f.open(base + "/f/t.flac");
    uint8_t b[100];
    bool ok = f.readFull(flac.size() - 100, b, 100) == 100 && !memcmp(b, &flac[flac.size() - 100], 100);
    ok = ok && f.readFull(200000, b, 100) == 100 && !memcmp(b, &flac[200000], 100);
    ok = ok && f.readFull(10, b, 100) == 100 && !memcmp(b, &flac[10], 100);
    check(ok, "random reads (tail, middle, head)");
    check(f.readAt(flac.size(), b, 10) == 0, "read at the end -> 0");
  }
  {
    HttpFile f;
    f.open(base + "/norange/t.flac");
    uint8_t b[16];
    check(f.readAt(300000, b, 16) < 0, "norange: jump far ahead fails");
  }
  // probe every format through HTTP (FILE* cookie)
  struct { const char* file; sdfile::Codec codec; } fmts[] = {
      {"t.flac", sdfile::Codec::FLAC}, {"t.mp3", sdfile::Codec::MP3},
      {"t.ogg", sdfile::Codec::OggVorbis}, {"t.m4a", sdfile::Codec::M4A},
      {"t.wav", sdfile::Codec::WAV}};
  for (auto& x : fmts) {
    HttpFile f;
    f.open(base + "/f/" + x.file);
    FILE* fp = f.asFile();
    sdfile::AudioFileInfo net, local;
    bool ok = fp && sdfile::probe(fp, "stream", net);
    if (fp) fclose(fp);
    sdfile::probeFile(dir + "/" + x.file, local);
    check(ok && net.codec == x.codec, std::string("probe ") + x.file + " codec " + sdfile::codecName(net.codec));
    check(net.durationMs == local.durationMs && net.durationMs > 29000 && net.durationMs < 31000,
          std::string("probe ") + x.file + " duration " + std::to_string(net.durationMs));
    check(net.seekable == local.seekable && net.offsetForMs(15000) == local.offsetForMs(15000),
          std::string("probe ") + x.file + " seek offset same as local (" + std::to_string(net.seekable) + ")");
    check(net.title == "Test Tone" || x.codec == sdfile::Codec::WAV, std::string("probe ") + x.file + " title '" + net.title + "'");
  }
  printf("%s (%d failures)\n", fails ? "FAILED" : "ALL PASSED", fails);
  return fails ? 1 : 0;
}
