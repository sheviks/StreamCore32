// PC test of the DLNA renderer: the real protocol code with a fake player.
//   ./build.sh && ./dlna_test [port] [--ssdp]
// then e.g. python3 test_dlna.py  (uses async_upnp_client as control point)
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include "DlnaRenderer.h"
#include "DlnaXml.h"

using namespace dlna;

class FakePlayer : public Player {
 public:
  std::mutex mu;
  PlayerStatus st;
  std::chrono::steady_clock::time_point started;
  uint32_t base = 0;

  uint32_t pos() {
    if (st.state != Transport::Playing)
      return base;
    return base + (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - started)
                      .count();
  }
  int setUri(std::shared_ptr<const Track> t) override {
    std::lock_guard<std::mutex> l(mu);
    printf("player: setUri %s title=%s artist=%s dur=%u mime=%s\n",
           t ? t->uri.c_str() : "(none)", t ? t->title.c_str() : "",
           t ? t->artist.c_str() : "", t ? t->durationMs : 0, t ? t->mime.c_str() : "");
    bool wasPlaying = st.state == Transport::Playing;
    st.current = t;
    st.durationMs = t ? t->durationMs : 0;
    st.seekable = t != nullptr;
    base = 0;
    started = std::chrono::steady_clock::now();
    st.state = !t ? Transport::NoMedia : wasPlaying ? Transport::Playing : Transport::Stopped;
    return kOk;
  }
  int setNextUri(std::shared_ptr<const Track> t) override {
    std::lock_guard<std::mutex> l(mu);
    printf("player: setNextUri %s\n", t ? t->uri.c_str() : "(none)");
    st.next = t;
    return kOk;
  }
  int play() override {
    std::lock_guard<std::mutex> l(mu);
    if (!st.current)
      return kTransitionNotAvailable;
    printf("player: play\n");
    if (st.state != Transport::Playing) {
      started = std::chrono::steady_clock::now();
      st.state = Transport::Playing;
    }
    return kOk;
  }
  int pause() override {
    std::lock_guard<std::mutex> l(mu);
    if (st.state != Transport::Playing)
      return kTransitionNotAvailable;
    base = pos();
    st.state = Transport::Paused;
    printf("player: pause at %u\n", base);
    return kOk;
  }
  int stop() override {
    std::lock_guard<std::mutex> l(mu);
    printf("player: stop\n");
    if (st.current)
      st.state = Transport::Stopped;
    base = 0;
    return kOk;
  }
  int seek(uint32_t ms) override {
    std::lock_guard<std::mutex> l(mu);
    printf("player: seek %u\n", ms);
    if (!st.seekable)
      return kTransitionNotAvailable;
    if (st.durationMs && ms > st.durationMs)
      return kIllegalSeekTarget;
    base = ms;
    started = std::chrono::steady_clock::now();
    return kOk;
  }
  int next() override {
    std::lock_guard<std::mutex> l(mu);
    if (!st.next)
      return kTransitionNotAvailable;
    printf("player: next -> %s\n", st.next->uri.c_str());
    st.current = st.next;
    st.next = nullptr;
    st.durationMs = st.current->durationMs;
    base = 0;
    started = std::chrono::steady_clock::now();
    return kOk;
  }
  int previous() override {
    std::lock_guard<std::mutex> l(mu);
    base = 0;
    started = std::chrono::steady_clock::now();
    return kOk;
  }
  void setVolume(uint8_t v) override {
    std::lock_guard<std::mutex> l(mu);
    printf("player: volume %u\n", v);
    st.volume = v;
  }
  void setMute(bool m) override {
    std::lock_guard<std::mutex> l(mu);
    printf("player: mute %d\n", m);
    st.mute = m;
  }
  PlayerStatus status() override {
    std::lock_guard<std::mutex> l(mu);
    PlayerStatus s = st;
    s.positionMs = pos();
    return s;
  }
};

static int unitTests() {
  int fails = 0;
  auto check = [&](bool ok, const char* what) {
    printf("%-50s %s\n", what, ok ? "OK" : "FAIL");
    if (!ok)
      fails++;
  };
  uint32_t ms = 0;
  check(parseTime("1:02:03", ms) && ms == 3723000, "parseTime H:MM:SS");
  check(parseTime("0:00:05.500", ms) && ms == 5500, "parseTime fraction");
  check(parseTime("00:01:00.1/2", ms) && ms == 60500, "parseTime F0/F1");
  check(parseTime("90", ms) && ms == 90000, "parseTime seconds");
  check(!parseTime("1:x:00", ms), "parseTime rubbish");
  check(formatTime(3723999) == "1:02:03", "formatTime");
  check(xmlUnescape("a&amp;b&lt;&#65;&#x42;&quot;") == "a&b<AB\"", "xmlUnescape");
  check(xmlEscape("<a&\"'>") == "&lt;a&amp;&quot;&apos;&gt;", "xmlEscape");
  const char* didl =
      "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" "
      "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
      "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\">"
      "<item id=\"1\" parentID=\"0\" restricted=\"1\"><dc:title>Song &amp; "
      "Dance</dc:title><dc:creator>Someone</dc:creator><upnp:artist "
      "role=\"Performer\">The Band</upnp:artist><upnp:album>Best Of</upnp:album>"
      "<upnp:albumArtURI dlna:profileID=\"JPEG_TN\">http://x/a.jpg?x=1&amp;y=2"
      "</upnp:albumArtURI><upnp:class>object.item.audioItem.musicTrack</upnp:class>"
      "<res protocolInfo=\"http-get:*:audio/flac:DLNA.ORG_OP=01\" "
      "duration=\"0:03:25.000\" size=\"12345\">http://srv/1.flac</res></item></DIDL-Lite>";
  auto d = parseDidl(didl);
  check(d.title == "Song & Dance", "didl title");
  check(d.artist == "The Band", "didl artist (upnp:artist first)");
  check(d.album == "Best Of", "didl album");
  check(d.artUri == "http://x/a.jpg?x=1&y=2", "didl art");
  check(d.mime == "audio/flac", "didl mime");
  check(d.durationMs == 205000, "didl duration");
  check(d.size == 12345, "didl size");
  auto d2 = parseDidl(makeDidl(d, "http://srv/1.flac"));
  check(d2.title == d.title && d2.mime == d.mime && d2.durationMs == d.durationMs,
        "makeDidl round trip");
  std::string v;
  check(xmlElement("<a><u:Play xmlns:u=\"x\"><InstanceID>0</InstanceID><Speed/></u:Play></a>",
                   "Speed", v) && v.empty(),
        "empty element");
  return fails;
}

int main(int argc, char** argv) {
  if (argc > 1 && !strcmp(argv[1], "--unit"))
    return unitTests() ? 1 : 0;
  FakePlayer player;
  player.st.volume = 40;
  Renderer::Config cfg;
  cfg.friendlyName = "StreamCore32 Test";
  cfg.uuid = "5c333200-0000-1000-8000-0123456789ab";
  cfg.port = argc > 1 ? (uint16_t)atoi(argv[1]) : 49152;
  cfg.ssdp = argc > 2 && !strcmp(argv[2], "--ssdp");
  cfg.localIp = [] { return std::string("127.0.0.1"); };
  Renderer r(player, cfg);
  if (!r.start())
    return 1;
  printf("running on port %u\n", cfg.port);
  fflush(stdout);
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    fflush(stdout);
  }
}
