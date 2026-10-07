#pragma once
// ============================================================================
//  DlnaRenderer — UPnP AV / DLNA MediaRenderer:1 (the "speaker" side).
//
//  Control points (BubbleUPnP, Hi-Fi Cast, foobar2000, JRiver, Kodi, Plex /
//  Jellyfin clients, ...) find the device and push a media URL to it.  The
//  renderer only speaks the protocol; playing is done by a Player
//  (DlnaStream on the device, a fake one in the PC tests):
//
//    SSDP        UDP 239.255.255.250:1900, M-SEARCH answers, NOTIFY alive /
//                byebye
//    HTTP        own small server (port CONFIG_SC32_DLNA_PORT):
//                  /dlna/description.xml          device description
//                  /dlna/<Service>.xml            service descriptions
//                  /dlna/<Service>/control        SOAP actions
//                  /dlna/<Service>/event          GENA SUBSCRIBE / UNSUBSCRIBE
//    Services    AVTransport:1        SetAVTransportURI, SetNextAVTransportURI
//                                     (gap-less), Play, Pause, Stop, Seek,
//                                     Next, Previous, Get*Info, ...
//                RenderingControl:1   Get/SetVolume, Get/SetMute, presets
//                ConnectionManager:1  GetProtocolInfo, ...
//    Eventing    LastChange (AVTransport, RenderingControl) and the
//                ConnectionManager variables; the player state is polled
//                every 250 ms and only changes are sent.
//
//  Two tasks: "dlna_net" (SSDP + HTTP, select()) and "dlna_evt" (GENA
//  notifications, which may block on a slow subscriber).
//  Portable: POSIX sockets (lwIP on the ESP32), std::thread on a PC.
// ============================================================================
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dlna {

enum class Transport : uint8_t { NoMedia = 0, Stopped, Playing, Paused, Transitioning };
const char* toString(Transport t);

/** A media item as handed in by a control point. */
struct Track {
  std::string uri;
  std::string metadata;  // DIDL-Lite as received (may be empty)
  // parsed from the metadata
  std::string title, artist, album, artUri;
  std::string mime;  // from protocolInfo, "" if unknown
  uint32_t durationMs = 0;
  uint64_t size = 0;
};
std::shared_ptr<const Track> makeTrack(const std::string& uri,
                                       const std::string& metadata);

/** UPnP error codes the player can return (0 = ok). */
enum Error : int {
  kOk = 0,
  kInvalidAction = 401,
  kInvalidArgs = 402,
  kActionFailed = 501,
  kTransitionNotAvailable = 701,
  kNoContents = 702,
  kSeekModeNotSupported = 710,
  kIllegalSeekTarget = 711,
  kPlayModeNotSupported = 712,
  kIllegalMimeType = 714,
  kResourceNotFound = 716,
  kInvalidInstanceId = 718,
};

struct PlayerStatus {
  Transport state = Transport::NoMedia;
  bool error = false;  // last track failed (TransportStatus ERROR_OCCURRED)
  std::shared_ptr<const Track> current, next;
  uint32_t positionMs = 0;
  uint32_t durationMs = 0;
  bool seekable = false;
  uint8_t volume = 0;  // 0..100
  bool mute = false;
};

/** What the renderer needs from the playing side. Thread safe. */
class Player {
 public:
  virtual ~Player() = default;
  /** SetAVTransportURI: becomes the current track (keeps playing if the
   *  renderer was playing). */
  virtual int setUri(std::shared_ptr<const Track> t) = 0;
  /** SetNextAVTransportURI (nullptr = clear): played gap-less after the
   *  current one. */
  virtual int setNextUri(std::shared_ptr<const Track> t) = 0;
  virtual int play() = 0;
  virtual int pause() = 0;
  virtual int stop() = 0;
  virtual int seek(uint32_t ms) = 0;
  virtual int next() = 0;
  virtual int previous() = 0;
  virtual void setVolume(uint8_t v) = 0;
  virtual void setMute(bool on) = 0;
  virtual PlayerStatus status() = 0;
};

class Renderer {
 public:
  struct Config {
    std::string friendlyName = "StreamCore32";
    std::string uuid;  // "xxxxxxxx-xxxx-..." (without "uuid:")
    uint16_t port = 49152;
    std::string modelName = "StreamCore32";
    std::string modelNumber = "1";
    std::string serial;
    std::string presentationUrl;  // "" = http://<ip>/
    /** Current IPv4 address ("" while offline): asked about every second,
     *  a change re-announces the device. */
    std::function<std::string()> localIp;
    bool ssdp = true;  // the PC tests run without
  };

  Renderer(Player& player, Config cfg);
  ~Renderer();

  bool start();
  /** Says goodbye (ssdp:byebye) and ends the tasks. */
  void stop();

  /** Something changed (state, volume, track): send events now instead of
   *  at the next poll. */
  void notify();

  // ---- protocol handling, public for the tests ----------------------------
  struct HttpRequest {
    std::string method, path;
    std::map<std::string, std::string> headers;  // keys lower case
    std::string body;
    std::string header(const std::string& k) const;
  };
  struct HttpResponse {
    int status = 200;
    std::string contentType;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
  };
  HttpResponse handle(const HttpRequest& rq, const std::string& localIp);
  /** Answers to an SSDP datagram (empty if none). */
  std::vector<std::string> ssdpAnswers(const std::string& datagram,
                                       const std::string& localIp);

  const std::string& sinkProtocolInfo() const;

 private:
  struct Sub {
    std::string sid;
    std::string service;  // "AVTransport", ...
    std::string host, path;
    uint16_t port = 80;
    uint32_t seq = 0;
    int64_t expiresMs = 0;
    bool initial = true;  // the first (full) event is still due
    int64_t readyMs = 0;  // ... but not before the SUBSCRIBE answer
    int failures = 0;
  };

  Player& player_;
  Config cfg_;
  std::atomic<bool> running_{false};
  std::atomic<int> tasks_{0};
  std::atomic<bool> wake_{false};
  std::mutex subMu_;
  std::vector<Sub> subs_;
  // state that was evented last (LastChange diffs)
  std::map<std::string, std::string> lastAvt_, lastRcs_;
  std::string scpdAvt_, scpdRcs_, scpdCm_;
  int udp_ = -1, tcp_ = -1;
  std::string joinedIp_;
  uint32_t sidCounter_ = 0;

  // ---- tasks ----
  void netTask();
  void eventTask();
  static void spawn(const char* name, uint32_t stack, std::function<void()> fn);

  // ---- network ----
  bool openSockets();
  void closeSockets();
  void joinMulticast(const std::string& ip);
  void sendNotify(const std::string& ip, bool alive);
  void serveClient(int fd, const std::string& ip);
  bool readRequest(int fd, HttpRequest& rq);
  static void writeResponse(int fd, const HttpResponse& r);

  // ---- content ----
  std::string description(const std::string& ip);
  HttpResponse soap(const std::string& service, const HttpRequest& rq);
  HttpResponse subscribe(const std::string& service, const HttpRequest& rq);
  HttpResponse unsubscribe(const std::string& service, const HttpRequest& rq);
  std::map<std::string, std::string> avtVars(const PlayerStatus& st);
  std::map<std::string, std::string> rcsVars(const PlayerStatus& st);
  static std::string lastChange(const char* ns,
                                const std::map<std::string, std::string>& vars,
                                bool rcs);
  std::string propertySet(const std::string& service, bool full,
                          const std::map<std::string, std::string>& avt,
                          const std::map<std::string, std::string>& rcs,
                          const std::map<std::string, std::string>& avtDiff,
                          const std::map<std::string, std::string>& rcsDiff);
  bool sendEvent(Sub& s, const std::string& body);

  std::string usn(const std::string& st) const;
  std::string location(const std::string& ip) const;
};

}  // namespace dlna
