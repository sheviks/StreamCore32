#include "DlnaRenderer.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "DlnaXml.h"

#ifdef ESP_PLATFORM
#include "Logger.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#define DLOG(level, fmt, ...) SC32_LOG(level, "DLNA: " fmt, ##__VA_ARGS__)
#else
#define DLOG(level, fmt, ...) \
  fprintf(stderr, "[dlna " #level "] " fmt "\n", ##__VA_ARGS__)
#endif

namespace dlna {

namespace {

constexpr const char* kDeviceType = "urn:schemas-upnp-org:device:MediaRenderer:1";
constexpr const char* kSsdpAddr = "239.255.255.250";
constexpr uint16_t kSsdpPort = 1900;
constexpr int kMaxAge = 1800;
constexpr size_t kMaxHeader = 8 * 1024;
constexpr size_t kMaxBody = 64 * 1024;
constexpr size_t kMaxSubsPerService = 8;
#ifdef ESP_PLATFORM
constexpr const char* kServer = "FreeRTOS/10 UPnP/1.0 StreamCore32/1.0";
#else
constexpr const char* kServer = "Linux UPnP/1.0 StreamCore32/1.0";
#endif

const char* const kServices[] = {"AVTransport", "RenderingControl",
                                 "ConnectionManager"};

std::string serviceType(const std::string& s) {
  return "urn:schemas-upnp-org:service:" + s + ":1";
}

int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void sleepMs(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

uint32_t random32() {
#ifdef ESP_PLATFORM
  return esp_random();
#else
  static std::mt19937 rng(std::random_device{}());
  return rng();
#endif
}

std::string lower(std::string s) {
  for (auto& c : s)
    c = (char)std::tolower((unsigned char)c);
  return s;
}

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace((unsigned char)s[a]))
    a++;
  while (b > a && std::isspace((unsigned char)s[b - 1]))
    b--;
  return s.substr(a, b - a);
}

void setTimeouts(int fd, int ms) {
  timeval tv{};
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

bool sendAll(int fd, const std::string& s) {
  size_t off = 0;
  while (off < s.size()) {
    ssize_t n = ::send(fd, s.data() + off, s.size() - off, 0);
    if (n <= 0)
      return false;
    off += size_t(n);
  }
  return true;
}

// TCP connect with a timeout (non-blocking connect + select)
int connectTimeout(const std::string& host, uint16_t port, int ms) {
  addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  char ps[8];
  snprintf(ps, sizeof ps, "%u", port);
  if (getaddrinfo(host.c_str(), ps, &hints, &res) != 0 || !res)
    return -1;
  int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (fd < 0) {
    freeaddrinfo(res);
    return -1;
  }
  int fl = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  int r = ::connect(fd, res->ai_addr, res->ai_addrlen);
  freeaddrinfo(res);
  if (r != 0 && errno != EINPROGRESS) {
    ::close(fd);
    return -1;
  }
  if (r != 0) {
    fd_set w;
    FD_ZERO(&w);
    FD_SET(fd, &w);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    if (select(fd + 1, nullptr, &w, nullptr, &tv) <= 0) {
      ::close(fd);
      return -1;
    }
    int err = 0;
    socklen_t len = sizeof err;
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err) {
      ::close(fd);
      return -1;
    }
  }
  fcntl(fd, F_SETFL, fl);
  setTimeouts(fd, ms);
  return fd;
}

// ------------------------------------------------------------------- SCPD --
struct Arg {
  const char* name;
  bool out;
  const char* var;
};
struct Action {
  const char* name;
  std::vector<Arg> args;
};
struct Var {
  const char* name;
  const char* type;
  bool evented = false;
  std::vector<const char*> allowed = {};
  const char* min = nullptr;
  const char* max = nullptr;
  const char* step = nullptr;
};

std::string makeScpd(const std::vector<Action>& actions,
                     const std::vector<Var>& vars) {
  std::string s =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">"
      "<specVersion><major>1</major><minor>0</minor></specVersion><actionList>";
  for (auto& a : actions) {
    s += "<action><name>";
    s += a.name;
    s += "</name><argumentList>";
    for (auto& g : a.args) {
      s += "<argument><name>";
      s += g.name;
      s += "</name><direction>";
      s += g.out ? "out" : "in";
      s += "</direction><relatedStateVariable>";
      s += g.var;
      s += "</relatedStateVariable></argument>";
    }
    s += "</argumentList></action>";
  }
  s += "</actionList><serviceStateTable>";
  for (auto& v : vars) {
    s += "<stateVariable sendEvents=\"";
    s += v.evented ? "yes" : "no";
    s += "\"><name>";
    s += v.name;
    s += "</name><dataType>";
    s += v.type;
    s += "</dataType>";
    if (!v.allowed.empty()) {
      s += "<allowedValueList>";
      for (auto* a : v.allowed) {
        s += "<allowedValue>";
        s += a;
        s += "</allowedValue>";
      }
      s += "</allowedValueList>";
    }
    if (v.min) {
      s += "<allowedValueRange><minimum>";
      s += v.min;
      s += "</minimum><maximum>";
      s += v.max;
      s += "</maximum>";
      if (v.step) {
        s += "<step>";
        s += v.step;
        s += "</step>";
      }
      s += "</allowedValueRange>";
    }
    s += "</stateVariable>";
  }
  s += "</serviceStateTable></scpd>";
  return s;
}

std::string scpdAvTransport() {
  const char* I = "A_ARG_TYPE_InstanceID";
  return makeScpd(
      {
          {"SetAVTransportURI",
           {{"InstanceID", false, I},
            {"CurrentURI", false, "AVTransportURI"},
            {"CurrentURIMetaData", false, "AVTransportURIMetaData"}}},
          {"SetNextAVTransportURI",
           {{"InstanceID", false, I},
            {"NextURI", false, "NextAVTransportURI"},
            {"NextURIMetaData", false, "NextAVTransportURIMetaData"}}},
          {"GetMediaInfo",
           {{"InstanceID", false, I},
            {"NrTracks", true, "NumberOfTracks"},
            {"MediaDuration", true, "CurrentMediaDuration"},
            {"CurrentURI", true, "AVTransportURI"},
            {"CurrentURIMetaData", true, "AVTransportURIMetaData"},
            {"NextURI", true, "NextAVTransportURI"},
            {"NextURIMetaData", true, "NextAVTransportURIMetaData"},
            {"PlayMedium", true, "PlaybackStorageMedium"},
            {"RecordMedium", true, "RecordStorageMedium"},
            {"WriteStatus", true, "RecordMediumWriteStatus"}}},
          {"GetTransportInfo",
           {{"InstanceID", false, I},
            {"CurrentTransportState", true, "TransportState"},
            {"CurrentTransportStatus", true, "TransportStatus"},
            {"CurrentSpeed", true, "TransportPlaySpeed"}}},
          {"GetPositionInfo",
           {{"InstanceID", false, I},
            {"Track", true, "CurrentTrack"},
            {"TrackDuration", true, "CurrentTrackDuration"},
            {"TrackMetaData", true, "CurrentTrackMetaData"},
            {"TrackURI", true, "CurrentTrackURI"},
            {"RelTime", true, "RelativeTimePosition"},
            {"AbsTime", true, "AbsoluteTimePosition"},
            {"RelCount", true, "RelativeCounterPosition"},
            {"AbsCount", true, "AbsoluteCounterPosition"}}},
          {"GetDeviceCapabilities",
           {{"InstanceID", false, I},
            {"PlayMedia", true, "PossiblePlaybackStorageMedia"},
            {"RecMedia", true, "PossibleRecordStorageMedia"},
            {"RecQualityModes", true, "PossibleRecordQualityModes"}}},
          {"GetTransportSettings",
           {{"InstanceID", false, I},
            {"PlayMode", true, "CurrentPlayMode"},
            {"RecQualityMode", true, "CurrentRecordQualityMode"}}},
          {"Stop", {{"InstanceID", false, I}}},
          {"Play",
           {{"InstanceID", false, I}, {"Speed", false, "TransportPlaySpeed"}}},
          {"Pause", {{"InstanceID", false, I}}},
          {"Seek",
           {{"InstanceID", false, I},
            {"Unit", false, "A_ARG_TYPE_SeekMode"},
            {"Target", false, "A_ARG_TYPE_SeekTarget"}}},
          {"Next", {{"InstanceID", false, I}}},
          {"Previous", {{"InstanceID", false, I}}},
          {"SetPlayMode",
           {{"InstanceID", false, I}, {"NewPlayMode", false, "CurrentPlayMode"}}},
          {"GetCurrentTransportActions",
           {{"InstanceID", false, I},
            {"Actions", true, "CurrentTransportActions"}}},
      },
      {
          {"TransportState", "string", false,
           {"STOPPED", "PLAYING", "TRANSITIONING", "PAUSED_PLAYBACK",
            "NO_MEDIA_PRESENT"}},
          {"TransportStatus", "string", false, {"OK", "ERROR_OCCURRED"}},
          {"PlaybackStorageMedium", "string", false, {"NONE", "NETWORK"}},
          {"RecordStorageMedium", "string", false, {"NOT_IMPLEMENTED"}},
          {"PossiblePlaybackStorageMedia", "string"},
          {"PossibleRecordStorageMedia", "string"},
          {"CurrentPlayMode", "string", false, {"NORMAL"}},
          {"TransportPlaySpeed", "string", false, {"1"}},
          {"RecordMediumWriteStatus", "string", false, {"NOT_IMPLEMENTED"}},
          {"CurrentRecordQualityMode", "string", false, {"NOT_IMPLEMENTED"}},
          {"PossibleRecordQualityModes", "string"},
          {"NumberOfTracks", "ui4", false, {}, "0", "1"},
          {"CurrentTrack", "ui4", false, {}, "0", "1", "1"},
          {"CurrentTrackDuration", "string"},
          {"CurrentMediaDuration", "string"},
          {"CurrentTrackMetaData", "string"},
          {"CurrentTrackURI", "string"},
          {"AVTransportURI", "string"},
          {"AVTransportURIMetaData", "string"},
          {"NextAVTransportURI", "string"},
          {"NextAVTransportURIMetaData", "string"},
          {"RelativeTimePosition", "string"},
          {"AbsoluteTimePosition", "string"},
          {"RelativeCounterPosition", "i4"},
          {"AbsoluteCounterPosition", "i4"},
          {"CurrentTransportActions", "string"},
          {"LastChange", "string", true},
          {"A_ARG_TYPE_SeekMode", "string", false,
           {"REL_TIME", "ABS_TIME", "TRACK_NR"}},
          {"A_ARG_TYPE_SeekTarget", "string"},
          {"A_ARG_TYPE_InstanceID", "ui4"},
      });
}

std::string scpdRenderingControl() {
  const char* I = "A_ARG_TYPE_InstanceID";
  const char* C = "A_ARG_TYPE_Channel";
  return makeScpd(
      {
          {"ListPresets",
           {{"InstanceID", false, I},
            {"CurrentPresetNameList", true, "PresetNameList"}}},
          {"SelectPreset",
           {{"InstanceID", false, I},
            {"PresetName", false, "A_ARG_TYPE_PresetName"}}},
          {"GetMute",
           {{"InstanceID", false, I},
            {"Channel", false, C},
            {"CurrentMute", true, "Mute"}}},
          {"SetMute",
           {{"InstanceID", false, I},
            {"Channel", false, C},
            {"DesiredMute", false, "Mute"}}},
          {"GetVolume",
           {{"InstanceID", false, I},
            {"Channel", false, C},
            {"CurrentVolume", true, "Volume"}}},
          {"SetVolume",
           {{"InstanceID", false, I},
            {"Channel", false, C},
            {"DesiredVolume", false, "Volume"}}},
      },
      {
          {"PresetNameList", "string"},
          {"LastChange", "string", true},
          {"Mute", "boolean"},
          {"Volume", "ui2", false, {}, "0", "100", "1"},
          {"A_ARG_TYPE_Channel", "string", false, {"Master"}},
          {"A_ARG_TYPE_InstanceID", "ui4"},
          {"A_ARG_TYPE_PresetName", "string", false, {"FactoryDefaults"}},
      });
}

std::string scpdConnectionManager() {
  return makeScpd(
      {
          {"GetProtocolInfo",
           {{"Source", true, "SourceProtocolInfo"},
            {"Sink", true, "SinkProtocolInfo"}}},
          {"GetCurrentConnectionIDs",
           {{"ConnectionIDs", true, "CurrentConnectionIDs"}}},
          {"GetCurrentConnectionInfo",
           {{"ConnectionID", false, "A_ARG_TYPE_ConnectionID"},
            {"RcsID", true, "A_ARG_TYPE_RcsID"},
            {"AVTransportID", true, "A_ARG_TYPE_AVTransportID"},
            {"ProtocolInfo", true, "A_ARG_TYPE_ProtocolInfo"},
            {"PeerConnectionManager", true, "A_ARG_TYPE_ConnectionManager"},
            {"PeerConnectionID", true, "A_ARG_TYPE_ConnectionID"},
            {"Direction", true, "A_ARG_TYPE_Direction"},
            {"Status", true, "A_ARG_TYPE_ConnectionStatus"}}},
      },
      {
          {"SourceProtocolInfo", "string", true},
          {"SinkProtocolInfo", "string", true},
          {"CurrentConnectionIDs", "string", true},
          {"A_ARG_TYPE_ConnectionStatus", "string", false,
           {"OK", "ContentFormatMismatch", "InsufficientBandwidth",
            "UnreliableChannel", "Unknown"}},
          {"A_ARG_TYPE_ConnectionManager", "string"},
          {"A_ARG_TYPE_Direction", "string", false, {"Input", "Output"}},
          {"A_ARG_TYPE_ProtocolInfo", "string"},
          {"A_ARG_TYPE_ConnectionID", "i4"},
          {"A_ARG_TYPE_AVTransportID", "i4"},
          {"A_ARG_TYPE_RcsID", "i4"},
      });
}

// what the VS1053 decodes (audio/L16 is converted to WAV by DlnaStream)
const std::string& sinkProtocols() {
  static const std::string s = [] {
    const char* mimes[] = {
        "audio/mpeg",  "audio/mp3",       "audio/x-mpeg",  "audio/flac",
        "audio/x-flac", "audio/wav",      "audio/x-wav",   "audio/wave",
        "audio/aac",   "audio/x-aac",     "audio/aacp",    "audio/mp4",
        "audio/x-m4a", "audio/m4a",       "audio/ogg",     "audio/x-ogg",
        "application/ogg", "audio/x-ms-wma", "audio/wma",  "audio/midi",
        "audio/x-midi", "audio/L16;rate=44100;channels=2",
        "audio/L16;rate=48000;channels=2", "audio/L16;rate=44100;channels=1",
        "audio/L16;rate=48000;channels=1"};
    std::string r =
        "http-get:*:audio/mpeg:DLNA.ORG_PN=MP3,"
        "http-get:*:audio/L16;rate=44100;channels=2:DLNA.ORG_PN=LPCM,"
        "http-get:*:audio/vnd.dlna.adts:DLNA.ORG_PN=AAC_ADTS_320,"
        "http-get:*:audio/mp4:DLNA.ORG_PN=AAC_ISO_320,"
        "http-get:*:audio/x-ms-wma:DLNA.ORG_PN=WMABASE,"
        "http-get:*:audio/x-ms-wma:DLNA.ORG_PN=WMAFULL";
    for (auto* m : mimes) {
      r += ",http-get:*:";
      r += m;
      r += ":*";
    }
    return r;
  }();
  return s;
}

std::string soapEnvelope(const std::string& inner) {
  return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
         "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
         "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
         "<s:Body>" +
         inner + "</s:Body></s:Envelope>";
}

const char* errorText(int code) {
  switch (code) {
    case kInvalidAction:
      return "Invalid Action";
    case kInvalidArgs:
      return "Invalid Args";
    case kTransitionNotAvailable:
      return "Transition not available";
    case kNoContents:
      return "No contents";
    case kSeekModeNotSupported:
      return "Seek mode not supported";
    case kIllegalSeekTarget:
      return "Illegal seek target";
    case kPlayModeNotSupported:
      return "Play mode not supported";
    case kIllegalMimeType:
      return "Illegal MIME-type";
    case kResourceNotFound:
      return "Resource not found";
    case kInvalidInstanceId:
      return "Invalid InstanceID";
    default:
      return "Action Failed";
  }
}

const char* statusText(int s) {
  switch (s) {
    case 200:
      return "OK";
    case 400:
      return "Bad Request";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 412:
      return "Precondition Failed";
    case 413:
      return "Payload Too Large";
    case 500:
      return "Internal Server Error";
    default:
      return "Error";
  }
}

std::string transportActions(const PlayerStatus& st) {
  std::string a;
  auto add = [&](const char* x) {
    if (!a.empty())
      a += ',';
    a += x;
  };
  switch (st.state) {
    case Transport::NoMedia:
      break;
    case Transport::Stopped:
      add("Play");
      break;
    case Transport::Playing:
      add("Pause");
      add("Stop");
      break;
    case Transport::Paused:
      add("Play");
      add("Stop");
      break;
    case Transport::Transitioning:
      add("Stop");
      break;
  }
  if (st.seekable && (st.state == Transport::Playing || st.state == Transport::Paused)) {
    add("Seek");
    add("Previous");
  }
  if (st.next && st.state != Transport::NoMedia)
    add("Next");
  return a;
}

}  // namespace

// ============================================================================
const char* toString(Transport t) {
  switch (t) {
    case Transport::Stopped:
      return "STOPPED";
    case Transport::Playing:
      return "PLAYING";
    case Transport::Paused:
      return "PAUSED_PLAYBACK";
    case Transport::Transitioning:
      return "TRANSITIONING";
    default:
      return "NO_MEDIA_PRESENT";
  }
}

std::shared_ptr<const Track> makeTrack(const std::string& uri,
                                       const std::string& metadata) {
  auto t = std::make_shared<Track>();
  t->uri = trim(uri);
  t->metadata = metadata;
  DidlInfo d = parseDidl(metadata);
  t->title = d.title;
  t->artist = d.artist;
  t->album = d.album;
  t->artUri = d.artUri;
  t->mime = d.mime;
  t->durationMs = d.durationMs;
  t->size = d.size;
  return t;
}

std::string Renderer::HttpRequest::header(const std::string& k) const {
  auto it = headers.find(k);
  return it == headers.end() ? std::string() : it->second;
}

Renderer::Renderer(Player& player, Config cfg)
    : player_(player), cfg_(std::move(cfg)) {
  if (cfg_.uuid.empty()) {
    char b[40];
    snprintf(b, sizeof b, "5c333200-0000-1000-8000-%08x%04x",
             (unsigned)random32(), (unsigned)(random32() & 0xFFFF));
    cfg_.uuid = b;
  }
  scpdAvt_ = scpdAvTransport();
  scpdRcs_ = scpdRenderingControl();
  scpdCm_ = scpdConnectionManager();
}

Renderer::~Renderer() {
  stop();
}

const std::string& Renderer::sinkProtocolInfo() const {
  return sinkProtocols();
}

// ================================================================== tasks ==
void Renderer::spawn(const char* name, uint32_t stack, std::function<void()> fn) {
#ifdef ESP_PLATFORM
  auto* f = new std::function<void()>(std::move(fn));
  auto tramp = [](void* arg) {
    auto* fp = static_cast<std::function<void()>*>(arg);
    (*fp)();
    delete fp;
    vTaskDeleteWithCaps(nullptr);
  };
  // stacks in PSRAM: sockets only, no flash / NVS access
  if (xTaskCreatePinnedToCoreWithCaps(tramp, name, stack, f, 2, nullptr,
                                      tskNO_AFFINITY,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    DLOG(error, "cannot start task %s", name);
    delete f;
  }
#else
  (void)name;
  (void)stack;
  std::thread(std::move(fn)).detach();
#endif
}

bool Renderer::start() {
  if (running_.exchange(true))
    return true;
  if (!openSockets()) {
    running_.store(false);
    return false;
  }
  tasks_.store(2);
  spawn("dlna_net", 6144, [this] {
    netTask();
    tasks_--;
  });
  spawn("dlna_evt", 6144, [this] {
    eventTask();
    tasks_--;
  });
  DLOG(info, "renderer \"%s\" on port %u (uuid:%s)", cfg_.friendlyName.c_str(),
       (unsigned)cfg_.port, cfg_.uuid.c_str());
  return true;
}

void Renderer::stop() {
  if (!running_.exchange(false))
    return;
  for (int i = 0; i < 300 && tasks_.load() > 0; i++)
    sleepMs(10);
}

void Renderer::notify() {
  wake_.store(true);
}

// ================================================================ sockets ==
bool Renderer::openSockets() {
  tcp_ = socket(AF_INET, SOCK_STREAM, 0);
  if (tcp_ < 0) {
    DLOG(error, "socket() failed");
    return false;
  }
  int one = 1;
  setsockopt(tcp_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(cfg_.port);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(tcp_, (sockaddr*)&a, sizeof a) != 0 || listen(tcp_, 4) != 0) {
    DLOG(error, "cannot listen on port %u", (unsigned)cfg_.port);
    closeSockets();
    return false;
  }
  if (!cfg_.ssdp)
    return true;
  udp_ = socket(AF_INET, SOCK_DGRAM, 0);
  if (udp_ < 0) {
    closeSockets();
    return false;
  }
  setsockopt(udp_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
  setsockopt(udp_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
  sockaddr_in u{};
  u.sin_family = AF_INET;
  u.sin_port = htons(kSsdpPort);
  u.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(udp_, (sockaddr*)&u, sizeof u) != 0) {
    DLOG(error, "cannot bind SSDP port 1900");
    closeSockets();
    return false;
  }
  unsigned char ttl = 2;
  setsockopt(udp_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
  return true;
}

void Renderer::closeSockets() {
  if (tcp_ >= 0)
    ::close(tcp_);
  if (udp_ >= 0)
    ::close(udp_);
  tcp_ = udp_ = -1;
  joinedIp_.clear();
}

void Renderer::joinMulticast(const std::string& ip) {
  if (udp_ < 0)
    return;
  ip_mreq m{};
  m.imr_multiaddr.s_addr = inet_addr(kSsdpAddr);
  if (!joinedIp_.empty()) {
    m.imr_interface.s_addr = inet_addr(joinedIp_.c_str());
    setsockopt(udp_, IPPROTO_IP, IP_DROP_MEMBERSHIP, &m, sizeof m);
  }
  m.imr_interface.s_addr = inet_addr(ip.c_str());
  if (setsockopt(udp_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m) != 0)
    DLOG(error, "IP_ADD_MEMBERSHIP failed (%d)", errno);
  in_addr ifa{};
  ifa.s_addr = inet_addr(ip.c_str());
  setsockopt(udp_, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof ifa);
  joinedIp_ = ip;
}

std::string Renderer::usn(const std::string& st) const {
  std::string u = "uuid:" + cfg_.uuid;
  if (st.rfind("uuid:", 0) == 0)
    return u;
  return u + "::" + st;
}

std::string Renderer::location(const std::string& ip) const {
  return "http://" + ip + ":" + std::to_string(cfg_.port) + "/dlna/description.xml";
}

void Renderer::sendNotify(const std::string& ip, bool alive) {
  if (udp_ < 0)
    return;
  std::vector<std::string> nts = {"upnp:rootdevice", "uuid:" + cfg_.uuid, kDeviceType};
  for (auto* s : kServices)
    nts.push_back(serviceType(s));
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(kSsdpPort);
  to.sin_addr.s_addr = inet_addr(kSsdpAddr);
  for (auto& nt : nts) {
    std::string m = "NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n";
    if (alive) {
      m += "CACHE-CONTROL: max-age=" + std::to_string(kMaxAge) + "\r\n";
      m += "LOCATION: " + location(ip) + "\r\n";
      m += std::string("SERVER: ") + kServer + "\r\n";
    }
    m += "NT: " + nt + "\r\n";
    m += std::string("NTS: ") + (alive ? "ssdp:alive" : "ssdp:byebye") + "\r\n";
    m += "USN: " + usn(nt) + "\r\n\r\n";
    sendto(udp_, m.data(), m.size(), 0, (sockaddr*)&to, sizeof to);
  }
}

std::vector<std::string> Renderer::ssdpAnswers(const std::string& d,
                                               const std::string& ip) {
  std::vector<std::string> out;
  if (d.compare(0, 8, "M-SEARCH") != 0)
    return out;
  std::string st, man;
  size_t p = d.find("\r\n");
  while (p != std::string::npos && p + 2 < d.size()) {
    size_t e = d.find("\r\n", p + 2);
    std::string line = d.substr(p + 2, (e == std::string::npos ? d.size() : e) - p - 2);
    p = e;
    auto c = line.find(':');
    if (c == std::string::npos)
      continue;
    std::string k = lower(trim(line.substr(0, c)));
    std::string v = trim(line.substr(c + 1));
    if (k == "st")
      st = v;
    else if (k == "man")
      man = v;
  }
  if (man.find("ssdp:discover") == std::string::npos || st.empty())
    return out;

  std::vector<std::string> targets;
  std::vector<std::string> all = {"upnp:rootdevice", "uuid:" + cfg_.uuid, kDeviceType};
  for (auto* s : kServices)
    all.push_back(serviceType(s));
  if (st == "ssdp:all") {
    targets = all;
  } else {
    for (auto& t : all) {
      // device / service types: answer searches for version 1
      if (t == st)
        targets.push_back(t);
    }
  }
  for (auto& t : targets) {
    std::string r = "HTTP/1.1 200 OK\r\n";
    r += "CACHE-CONTROL: max-age=" + std::to_string(kMaxAge) + "\r\n";
    r += "EXT:\r\n";
    r += "LOCATION: " + location(ip) + "\r\n";
    r += std::string("SERVER: ") + kServer + "\r\n";
    r += "ST: " + t + "\r\n";
    r += "USN: " + usn(t) + "\r\n";
    r += "Content-Length: 0\r\n\r\n";
    out.push_back(std::move(r));
  }
  return out;
}

void Renderer::netTask() {
  std::string ip;
  int64_t lastIpCheck = -100000, lastAlive = 0, nextBurst = 0;
  int burst = 0;
  std::vector<char> buf(1536);
  while (running_.load()) {
    const int64_t now = nowMs();
    if (now - lastIpCheck >= 1000) {
      lastIpCheck = now;
      std::string cur = cfg_.localIp ? cfg_.localIp() : "127.0.0.1";
      if (cur != ip) {
        ip = cur;
        if (!ip.empty()) {
          DLOG(info, "announcing on %s", location(ip).c_str());
          joinMulticast(ip);
          burst = 3;
          nextBurst = now;
        }
      }
    }
    if (ip.empty()) {
      sleepMs(500);
      continue;
    }
    if (udp_ >= 0) {
      if (burst > 0 && now >= nextBurst) {
        sendNotify(ip, true);
        burst--;
        nextBurst = now + 2000;
        lastAlive = now;
      } else if (burst == 0 && now - lastAlive > kMaxAge * 1000 / 3) {
        burst = 2;
        nextBurst = now;
      }
    }

    fd_set r;
    FD_ZERO(&r);
    int maxFd = tcp_;
    FD_SET(tcp_, &r);
    if (udp_ >= 0) {
      FD_SET(udp_, &r);
      maxFd = std::max(maxFd, udp_);
    }
    timeval tv{0, 250 * 1000};
    int n = select(maxFd + 1, &r, nullptr, nullptr, &tv);
    if (n <= 0)
      continue;
    if (udp_ >= 0 && FD_ISSET(udp_, &r)) {
      sockaddr_in from{};
      socklen_t fl = sizeof from;
      ssize_t len = recvfrom(udp_, buf.data(), buf.size() - 1, 0, (sockaddr*)&from, &fl);
      if (len > 0) {
        for (auto& a : ssdpAnswers(std::string(buf.data(), size_t(len)), ip))
          sendto(udp_, a.data(), a.size(), 0, (sockaddr*)&from, fl);
      }
    }
    if (FD_ISSET(tcp_, &r)) {
      sockaddr_in from{};
      socklen_t fl = sizeof from;
      int fd = accept(tcp_, (sockaddr*)&from, &fl);
      if (fd >= 0) {
        serveClient(fd, ip);
        ::close(fd);
      }
    }
  }
  if (!ip.empty() && udp_ >= 0)
    sendNotify(ip, false);
  closeSockets();
}

// ============================================================ HTTP server ==
bool Renderer::readRequest(int fd, HttpRequest& rq) {
  std::string data;
  char b[1024];
  size_t hdrEnd = std::string::npos;
  while (hdrEnd == std::string::npos) {
    ssize_t n = recv(fd, b, sizeof b, 0);
    if (n <= 0)
      return false;
    data.append(b, size_t(n));
    hdrEnd = data.find("\r\n\r\n");
    if (hdrEnd == std::string::npos && data.size() > kMaxHeader)
      return false;
  }
  size_t le = data.find("\r\n");
  std::string first = data.substr(0, le);
  size_t s1 = first.find(' ');
  size_t s2 = first.find(' ', s1 + 1);
  if (s1 == std::string::npos || s2 == std::string::npos)
    return false;
  rq.method = first.substr(0, s1);
  rq.path = first.substr(s1 + 1, s2 - s1 - 1);
  size_t p = le;
  while (p < hdrEnd) {
    size_t e = data.find("\r\n", p + 2);
    if (e == std::string::npos || e > hdrEnd)
      e = hdrEnd;
    std::string line = data.substr(p + 2, e - p - 2);
    p = e;
    auto c = line.find(':');
    if (c == std::string::npos)
      continue;
    rq.headers[lower(trim(line.substr(0, c)))] = trim(line.substr(c + 1));
  }
  size_t len = (size_t)strtoul(rq.header("content-length").c_str(), nullptr, 10);
  if (len > kMaxBody)
    return false;
  rq.body = data.substr(hdrEnd + 4);
  while (rq.body.size() < len) {
    ssize_t n = recv(fd, b, std::min(sizeof b, len - rq.body.size()), 0);
    if (n <= 0)
      return false;
    rq.body.append(b, size_t(n));
  }
  if (rq.body.size() > len)
    rq.body.resize(len);
  return true;
}

void Renderer::writeResponse(int fd, const HttpResponse& r) {
  std::string h = "HTTP/1.1 " + std::to_string(r.status) + " " + statusText(r.status) + "\r\n";
  if (!r.contentType.empty())
    h += "Content-Type: " + r.contentType + "\r\n";
  h += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
  h += std::string("Server: ") + kServer + "\r\n";
  for (auto& kv : r.headers)
    h += kv.first + ": " + kv.second + "\r\n";
  h += "Connection: close\r\n\r\n";
  if (sendAll(fd, h) && !r.body.empty())
    sendAll(fd, r.body);
}

void Renderer::serveClient(int fd, const std::string& ip) {
  setTimeouts(fd, 3000);
  HttpRequest rq;
  if (!readRequest(fd, rq)) {
    HttpResponse bad;
    bad.status = 400;
    writeResponse(fd, bad);
    return;
  }
  // the address this client reached us on (LOCATION / presentationURL)
  std::string local = ip;
  sockaddr_in la{};
  socklen_t ll = sizeof la;
  if (getsockname(fd, (sockaddr*)&la, &ll) == 0) {
    char s[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &la.sin_addr, s, sizeof s))
      local = s;
  }
  HttpResponse r = handle(rq, local);
  if (rq.method == "HEAD")
    r.body.clear();  // (Content-Length is computed from the body: keep it)
  writeResponse(fd, r);
  if (rq.method == "SUBSCRIBE")
    notify();  // initial event after the answer
}

Renderer::HttpResponse Renderer::handle(const HttpRequest& rq,
                                        const std::string& ip) {
  HttpResponse r;
  std::string path = rq.path;
  // absolute URI form ("http://host:port/path")
  if (path.rfind("http://", 0) == 0) {
    size_t s = path.find('/', 7);
    path = s == std::string::npos ? "/" : path.substr(s);
  }
  auto q = path.find('?');
  if (q != std::string::npos)
    path.resize(q);

  const bool get = rq.method == "GET" || rq.method == "HEAD";
  if (path == "/dlna/description.xml" || path == "/description.xml") {
    if (!get) {
      r.status = 405;
      return r;
    }
    r.contentType = "text/xml; charset=\"utf-8\"";
    r.body = description(ip);
    return r;
  }
  for (auto* s : kServices) {
    std::string base = std::string("/dlna/") + s;
    if (path == base + ".xml") {
      if (!get) {
        r.status = 405;
        return r;
      }
      r.contentType = "text/xml; charset=\"utf-8\"";
      r.body = s[0] == 'A' ? scpdAvt_ : s[0] == 'R' ? scpdRcs_ : scpdCm_;
      return r;
    }
    if (path == base + "/control") {
      if (rq.method != "POST") {
        r.status = 405;
        return r;
      }
      return soap(s, rq);
    }
    if (path == base + "/event") {
      if (rq.method == "SUBSCRIBE")
        return subscribe(s, rq);
      if (rq.method == "UNSUBSCRIBE")
        return unsubscribe(s, rq);
      r.status = 405;
      return r;
    }
  }
  r.status = 404;
  return r;
}

std::string Renderer::description(const std::string& ip) {
  std::string pres = cfg_.presentationUrl.empty() ? "http://" + ip + "/" : cfg_.presentationUrl;
  std::string s =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<root xmlns=\"urn:schemas-upnp-org:device-1-0\" "
      "xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\">"
      "<specVersion><major>1</major><minor>0</minor></specVersion>"
      "<device>"
      "<deviceType>" +
      std::string(kDeviceType) +
      "</deviceType>"
      "<friendlyName>" +
      xmlEscape(cfg_.friendlyName) +
      "</friendlyName>"
      "<manufacturer>StreamCore32</manufacturer>"
      "<manufacturerURL>https://github.com/</manufacturerURL>"
      "<modelDescription>StreamCore32 network audio player</modelDescription>"
      "<modelName>" +
      xmlEscape(cfg_.modelName) + "</modelName><modelNumber>" +
      xmlEscape(cfg_.modelNumber) + "</modelNumber><serialNumber>" +
      xmlEscape(cfg_.serial.empty() ? cfg_.uuid : cfg_.serial) +
      "</serialNumber><UDN>uuid:" + cfg_.uuid +
      "</UDN>"
      "<dlna:X_DLNADOC>DMR-1.50</dlna:X_DLNADOC>"
      "<presentationURL>" +
      xmlEscape(pres) + "</presentationURL><serviceList>";
  for (auto* sv : kServices) {
    std::string n(sv);
    s += "<service><serviceType>" + serviceType(n) +
         "</serviceType><serviceId>urn:upnp-org:serviceId:" + n +
         "</serviceId><SCPDURL>/dlna/" + n + ".xml</SCPDURL><controlURL>/dlna/" + n +
         "/control</controlURL><eventSubURL>/dlna/" + n +
         "/event</eventSubURL></service>";
  }
  s += "</serviceList></device></root>";
  return s;
}

// ================================================================== SOAP ==
Renderer::HttpResponse Renderer::soap(const std::string& service,
                                      const HttpRequest& rq) {
  HttpResponse r;
  r.contentType = "text/xml; charset=\"utf-8\"";
  r.headers.push_back({"EXT", ""});

  // action name: SOAPACTION "urn:...:AVTransport:1#Play", else the first
  // element inside <Body>
  std::string action = rq.header("soapaction");
  action.erase(std::remove(action.begin(), action.end(), '"'), action.end());
  auto hash = action.find('#');
  action = hash == std::string::npos ? std::string() : action.substr(hash + 1);
  std::string body;
  xmlElement(rq.body, "Body", body);
  if (action.empty()) {
    size_t lt = body.find('<');
    while (lt != std::string::npos && lt + 1 < body.size() &&
           (body[lt + 1] == '?' || body[lt + 1] == '!'))
      lt = body.find('<', lt + 1);
    if (lt != std::string::npos) {
      size_t e = lt + 1;
      while (e < body.size() && !std::isspace((unsigned char)body[e]) &&
             body[e] != '>' && body[e] != '/')
        e++;
      action = body.substr(lt + 1, e - lt - 1);
      auto c = action.rfind(':');
      if (c != std::string::npos)
        action = action.substr(c + 1);
    }
  }
  if (body.empty())
    body = rq.body;

  std::vector<std::pair<std::string, std::string>> out;
  int err = kOk;
  auto arg = [&](const char* n) { return xmlText(body, n); };
  auto has = [&](const char* n) {
    std::string v;
    return xmlElement(body, n, v);
  };
  bool changed = false;

  if (service != "ConnectionManager") {
    std::string inst = trim(arg("InstanceID"));
    if (!has("InstanceID"))
      err = kInvalidArgs;
    else if (inst != "0")
      err = kInvalidInstanceId;
  }

  if (err == kOk && service == "AVTransport") {
    if (action == "SetAVTransportURI") {
      std::string uri = trim(arg("CurrentURI"));
      if (uri.empty()) {
        err = player_.setUri(nullptr);
      } else {
        auto t = makeTrack(uri, arg("CurrentURIMetaData"));
        DLOG(info, "SetAVTransportURI %s (%s)", t->uri.c_str(), t->title.c_str());
        err = player_.setUri(t);
      }
      changed = true;
    } else if (action == "SetNextAVTransportURI") {
      std::string uri = trim(arg("NextURI"));
      if (uri.empty()) {
        err = player_.setNextUri(nullptr);
      } else {
        auto t = makeTrack(uri, arg("NextURIMetaData"));
        DLOG(info, "SetNextAVTransportURI %s (%s)", t->uri.c_str(), t->title.c_str());
        err = player_.setNextUri(t);
      }
      changed = true;
    } else if (action == "GetMediaInfo") {
      auto st = player_.status();
      out = {{"NrTracks", st.current ? "1" : "0"},
             {"MediaDuration", formatTime(st.durationMs)},
             {"CurrentURI", st.current ? st.current->uri : ""},
             {"CurrentURIMetaData", st.current ? st.current->metadata : ""},
             {"NextURI", st.next ? st.next->uri : ""},
             {"NextURIMetaData", st.next ? st.next->metadata : ""},
             {"PlayMedium", st.current ? "NETWORK" : "NONE"},
             {"RecordMedium", "NOT_IMPLEMENTED"},
             {"WriteStatus", "NOT_IMPLEMENTED"}};
    } else if (action == "GetTransportInfo") {
      auto st = player_.status();
      out = {{"CurrentTransportState", toString(st.state)},
             {"CurrentTransportStatus", st.error ? "ERROR_OCCURRED" : "OK"},
             {"CurrentSpeed", "1"}};
    } else if (action == "GetPositionInfo") {
      auto st = player_.status();
      std::string pos = formatTime(st.positionMs);
      out = {{"Track", st.current ? "1" : "0"},
             {"TrackDuration", formatTime(st.durationMs)},
             {"TrackMetaData", st.current ? st.current->metadata : ""},
             {"TrackURI", st.current ? st.current->uri : ""},
             {"RelTime", pos},
             {"AbsTime", pos},
             {"RelCount", "2147483647"},
             {"AbsCount", "2147483647"}};
    } else if (action == "GetDeviceCapabilities") {
      out = {{"PlayMedia", "NETWORK"},
             {"RecMedia", "NOT_IMPLEMENTED"},
             {"RecQualityModes", "NOT_IMPLEMENTED"}};
    } else if (action == "GetTransportSettings") {
      out = {{"PlayMode", "NORMAL"}, {"RecQualityMode", "NOT_IMPLEMENTED"}};
    } else if (action == "GetCurrentTransportActions") {
      out = {{"Actions", transportActions(player_.status())}};
    } else if (action == "Play") {
      err = player_.play();
      changed = true;
    } else if (action == "Pause") {
      err = player_.pause();
      changed = true;
    } else if (action == "Stop") {
      err = player_.stop();
      changed = true;
    } else if (action == "Next") {
      err = player_.next();
      changed = true;
    } else if (action == "Previous") {
      err = player_.previous();
      changed = true;
    } else if (action == "Seek") {
      std::string unit = trim(arg("Unit"));
      std::string target = trim(arg("Target"));
      uint32_t ms = 0;
      if (unit == "REL_TIME" || unit == "ABS_TIME") {
        if (!parseTime(target, ms))
          err = kIllegalSeekTarget;
        else
          err = player_.seek(ms);
      } else if (unit == "TRACK_NR") {
        if (target == "1" || target == "0")
          err = player_.seek(0);
        else
          err = kIllegalSeekTarget;
      } else {
        err = kSeekModeNotSupported;
      }
      changed = true;
    } else if (action == "SetPlayMode") {
      if (trim(arg("NewPlayMode")) != "NORMAL")
        err = kPlayModeNotSupported;
    } else {
      err = kInvalidAction;
    }
  } else if (err == kOk && service == "RenderingControl") {
    const bool channelOk = !has("Channel") || trim(arg("Channel")) == "Master";
    if (action == "ListPresets") {
      out = {{"CurrentPresetNameList", "FactoryDefaults"}};
    } else if (action == "SelectPreset") {
      if (trim(arg("PresetName")) != "FactoryDefaults")
        err = kInvalidArgs;
    } else if (action == "GetVolume") {
      if (!channelOk)
        err = kInvalidArgs;
      else
        out = {{"CurrentVolume", std::to_string(player_.status().volume)}};
    } else if (action == "SetVolume") {
      std::string v = trim(arg("DesiredVolume"));
      char* end = nullptr;
      long n = strtol(v.c_str(), &end, 10);
      if (!channelOk || v.empty() || *end || n < 0 || n > 100) {
        err = kInvalidArgs;
      } else {
        player_.setVolume(uint8_t(n));
        changed = true;
      }
    } else if (action == "GetMute") {
      if (!channelOk)
        err = kInvalidArgs;
      else
        out = {{"CurrentMute", player_.status().mute ? "1" : "0"}};
    } else if (action == "SetMute") {
      std::string v = lower(trim(arg("DesiredMute")));
      if (!channelOk || (v != "1" && v != "0" && v != "true" && v != "false" &&
                         v != "yes" && v != "no")) {
        err = kInvalidArgs;
      } else {
        player_.setMute(v == "1" || v == "true" || v == "yes");
        changed = true;
      }
    } else {
      err = kInvalidAction;
    }
  } else if (err == kOk && service == "ConnectionManager") {
    if (action == "GetProtocolInfo") {
      out = {{"Source", ""}, {"Sink", sinkProtocols()}};
    } else if (action == "GetCurrentConnectionIDs") {
      out = {{"ConnectionIDs", "0"}};
    } else if (action == "GetCurrentConnectionInfo") {
      if (trim(arg("ConnectionID")) != "0") {
        err = 706;  // invalid connection reference
      } else {
        auto st = player_.status();
        std::string pi;
        if (st.current && !st.current->metadata.empty())
          pi = xmlAttr(st.current->metadata, "res", "protocolInfo");
        out = {{"RcsID", "0"},
               {"AVTransportID", "0"},
               {"ProtocolInfo", pi},
               {"PeerConnectionManager", ""},
               {"PeerConnectionID", "-1"},
               {"Direction", "Input"},
               {"Status", "OK"}};
      }
    } else {
      err = kInvalidAction;
    }
  }

  if (changed)
    notify();
  if (err != kOk) {
    DLOG(info, "%s#%s -> error %d", service.c_str(), action.c_str(), err);
    r.status = 500;
    r.body = soapEnvelope(
        "<s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</"
        "faultstring><detail><UPnPError "
        "xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>" +
        std::to_string(err) + "</errorCode><errorDescription>" + errorText(err) +
        "</errorDescription></UPnPError></detail></s:Fault>");
    return r;
  }
  std::string inner = "<u:" + action + "Response xmlns:u=\"" + serviceType(service) + "\">";
  for (auto& kv : out)
    inner += "<" + kv.first + ">" + xmlEscape(kv.second) + "</" + kv.first + ">";
  inner += "</u:" + action + "Response>";
  r.body = soapEnvelope(inner);
  return r;
}

// ================================================================== GENA ==
Renderer::HttpResponse Renderer::subscribe(const std::string& service,
                                           const HttpRequest& rq) {
  HttpResponse r;
  const std::string sid = rq.header("sid");
  const std::string cb = rq.header("callback");
  const std::string nt = rq.header("nt");
  int timeout = kMaxAge;
  std::string to = lower(rq.header("timeout"));
  if (to.rfind("second-", 0) == 0 && to != "second-infinite")
    timeout = std::max(60, std::min(kMaxAge, atoi(to.c_str() + 7)));

  std::lock_guard<std::mutex> lk(subMu_);
  const int64_t now = nowMs();
  if (!sid.empty()) {
    if (!cb.empty() || !nt.empty()) {
      r.status = 400;
      return r;
    }
    for (auto& s : subs_)
      if (s.sid == sid && s.service == service) {
        s.expiresMs = now + int64_t(timeout) * 1000;
        r.headers = {{"SID", sid}, {"TIMEOUT", "Second-" + std::to_string(timeout)}};
        return r;
      }
    r.status = 412;
    return r;
  }
  if (nt != "upnp:event") {
    r.status = 412;
    return r;
  }
  // CALLBACK: <http://192.168.1.5:49200/evt/1><http://...>  (first one)
  size_t a = cb.find("<http://"), b = a == std::string::npos ? a : cb.find('>', a);
  if (b == std::string::npos) {
    r.status = 412;
    return r;
  }
  std::string url = cb.substr(a + 8, b - a - 8);
  Sub s;
  size_t slash = url.find('/');
  std::string hostport = url.substr(0, slash);
  s.path = slash == std::string::npos ? "/" : url.substr(slash);
  size_t colon = hostport.rfind(':');
  if (colon != std::string::npos) {
    s.host = hostport.substr(0, colon);
    s.port = (uint16_t)atoi(hostport.c_str() + colon + 1);
  } else {
    s.host = hostport;
  }
  if (s.host.empty() || !s.port) {
    r.status = 412;
    return r;
  }
  char id[64];
  snprintf(id, sizeof id, "uuid:%08x-%04x-4%03x-a%03x-%08x%04x", (unsigned)random32(),
           (unsigned)(random32() & 0xFFFF), (unsigned)(random32() & 0xFFF),
           (unsigned)(random32() & 0xFFF), (unsigned)random32(),
           (unsigned)(++sidCounter_ & 0xFFFF));
  s.sid = id;
  s.service = service;
  s.expiresMs = now + int64_t(timeout) * 1000;
  s.readyMs = now + 150;
  // drop expired ones, keep at most kMaxSubsPerService per service
  subs_.erase(std::remove_if(subs_.begin(), subs_.end(),
                             [&](const Sub& x) { return x.expiresMs < now; }),
              subs_.end());
  size_t count = 0;
  auto oldest = subs_.end();
  for (auto it = subs_.begin(); it != subs_.end(); ++it)
    if (it->service == service) {
      count++;
      if (oldest == subs_.end() || it->expiresMs < oldest->expiresMs)
        oldest = it;
    }
  if (count >= kMaxSubsPerService && oldest != subs_.end())
    subs_.erase(oldest);
  DLOG(info, "%s: subscription from %s:%u", service.c_str(), s.host.c_str(),
       (unsigned)s.port);
  subs_.push_back(s);
  r.headers = {{"SID", s.sid}, {"TIMEOUT", "Second-" + std::to_string(timeout)}};
  return r;
}

Renderer::HttpResponse Renderer::unsubscribe(const std::string& service,
                                             const HttpRequest& rq) {
  HttpResponse r;
  const std::string sid = rq.header("sid");
  std::lock_guard<std::mutex> lk(subMu_);
  for (auto it = subs_.begin(); it != subs_.end(); ++it)
    if (it->sid == sid && it->service == service) {
      subs_.erase(it);
      return r;
    }
  r.status = 412;
  return r;
}

std::map<std::string, std::string> Renderer::avtVars(const PlayerStatus& st) {
  std::map<std::string, std::string> v;
  const bool media = st.current != nullptr;
  v["TransportState"] = toString(st.state);
  v["TransportStatus"] = st.error ? "ERROR_OCCURRED" : "OK";
  v["PlaybackStorageMedium"] = media ? "NETWORK" : "NONE";
  v["PossiblePlaybackStorageMedia"] = "NETWORK";
  v["PossibleRecordStorageMedia"] = "NOT_IMPLEMENTED";
  v["RecordStorageMedium"] = "NOT_IMPLEMENTED";
  v["RecordMediumWriteStatus"] = "NOT_IMPLEMENTED";
  v["PossibleRecordQualityModes"] = "NOT_IMPLEMENTED";
  v["CurrentRecordQualityMode"] = "NOT_IMPLEMENTED";
  v["CurrentPlayMode"] = "NORMAL";
  v["TransportPlaySpeed"] = "1";
  v["NumberOfTracks"] = media ? "1" : "0";
  v["CurrentTrack"] = media ? "1" : "0";
  v["CurrentTrackDuration"] = formatTime(st.durationMs);
  v["CurrentMediaDuration"] = formatTime(st.durationMs);
  v["CurrentTrackURI"] = media ? st.current->uri : "";
  v["CurrentTrackMetaData"] = media ? st.current->metadata : "";
  v["AVTransportURI"] = media ? st.current->uri : "";
  v["AVTransportURIMetaData"] = media ? st.current->metadata : "";
  v["NextAVTransportURI"] = st.next ? st.next->uri : "";
  v["NextAVTransportURIMetaData"] = st.next ? st.next->metadata : "";
  v["CurrentTransportActions"] = transportActions(st);
  return v;
}

std::map<std::string, std::string> Renderer::rcsVars(const PlayerStatus& st) {
  return {{"Volume", std::to_string(st.volume)},
          {"Mute", st.mute ? "1" : "0"},
          {"PresetNameList", "FactoryDefaults"}};
}

std::string Renderer::lastChange(const char* ns,
                                 const std::map<std::string, std::string>& vars,
                                 bool rcs) {
  std::string e = std::string("<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/") +
                   ns + "/\"><InstanceID val=\"0\">";
  for (auto& kv : vars) {
    e += "<" + kv.first;
    if (rcs && (kv.first == "Volume" || kv.first == "Mute"))
      e += " channel=\"Master\"";
    e += " val=\"" + xmlEscape(kv.second) + "\"/>";
  }
  e += "</InstanceID></Event>";
  return e;
}

std::string Renderer::propertySet(
    const std::string& service, bool full,
    const std::map<std::string, std::string>& avt,
    const std::map<std::string, std::string>& rcs,
    const std::map<std::string, std::string>& avtDiff,
    const std::map<std::string, std::string>& rcsDiff) {
  std::string s = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                  "<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\">";
  if (service == "AVTransport") {
    s += "<e:property><LastChange>" +
         xmlEscape(lastChange("AVT", full ? avt : avtDiff, false)) +
         "</LastChange></e:property>";
  } else if (service == "RenderingControl") {
    s += "<e:property><LastChange>" +
         xmlEscape(lastChange("RCS", full ? rcs : rcsDiff, true)) +
         "</LastChange></e:property>";
  } else {
    s += "<e:property><SourceProtocolInfo></SourceProtocolInfo></e:property>"
         "<e:property><SinkProtocolInfo>" +
         xmlEscape(sinkProtocols()) +
         "</SinkProtocolInfo></e:property>"
         "<e:property><CurrentConnectionIDs>0</CurrentConnectionIDs></e:property>";
  }
  s += "</e:propertyset>";
  return s;
}

bool Renderer::sendEvent(Sub& s, const std::string& body) {
  int fd = connectTimeout(s.host, s.port, 1500);
  if (fd < 0)
    return false;
  std::string m = "NOTIFY " + s.path + " HTTP/1.1\r\n";
  m += "HOST: " + s.host + ":" + std::to_string(s.port) + "\r\n";
  m += "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n";
  m += "NT: upnp:event\r\nNTS: upnp:propchange\r\n";
  m += "SID: " + s.sid + "\r\n";
  m += "SEQ: " + std::to_string(s.seq) + "\r\n";
  m += "CONTENT-LENGTH: " + std::to_string(body.size()) + "\r\n";
  m += "CONNECTION: close\r\n\r\n";
  m += body;
  bool ok = sendAll(fd, m);
  if (ok) {
    char b[64];
    ssize_t n = recv(fd, b, sizeof b - 1, 0);
    ok = n >= 12;
    if (ok) {
      b[n] = 0;
      ok = strstr(b, " 200") != nullptr;
    }
  }
  ::close(fd);
  return ok;
}

void Renderer::eventTask() {
  int64_t lastSend = 0;
  while (running_.load()) {
    for (int i = 0; i < 25 && !wake_.load() && running_.load(); i++)
      sleepMs(10);
    wake_.store(false);
    if (!running_.load())
      break;
    // moderation: LastChange at most 5 times per second
    int64_t since = nowMs() - lastSend;
    if (since < 200)
      sleepMs(int(200 - since));

    PlayerStatus st = player_.status();
    auto avt = avtVars(st);
    auto rcs = rcsVars(st);
    std::map<std::string, std::string> avtDiff, rcsDiff;
    for (auto& kv : avt) {
      auto it = lastAvt_.find(kv.first);
      if (it == lastAvt_.end() || it->second != kv.second)
        avtDiff.insert(kv);
    }
    for (auto& kv : rcs) {
      auto it = lastRcs_.find(kv.first);
      if (it == lastRcs_.end() || it->second != kv.second)
        rcsDiff.insert(kv);
    }
    lastAvt_ = avt;
    lastRcs_ = rcs;

    // what to send (copied: sending may block, SUBSCRIBE must not wait)
    struct Job {
      Sub sub;
      bool full;
    };
    std::vector<Job> jobs;
    {
      std::lock_guard<std::mutex> lk(subMu_);
      const int64_t now = nowMs();
      subs_.erase(std::remove_if(subs_.begin(), subs_.end(),
                                 [&](const Sub& x) {
                                   return x.expiresMs < now || x.failures >= 5;
                                 }),
                  subs_.end());
      for (auto& s : subs_) {
        if (s.initial) {
          if (now >= s.readyMs)
            jobs.push_back({s, true});
          else
            wake_.store(true);  // come back soon
        }
        else if (s.service == "AVTransport" && !avtDiff.empty())
          jobs.push_back({s, false});
        else if (s.service == "RenderingControl" && !rcsDiff.empty())
          jobs.push_back({s, false});
      }
    }
    if (jobs.empty())
      continue;
    lastSend = nowMs();
    for (auto& j : jobs) {
      std::string body = propertySet(j.sub.service, j.full, avt, rcs, avtDiff, rcsDiff);
      bool ok = sendEvent(j.sub, body);
      std::lock_guard<std::mutex> lk(subMu_);
      for (auto& s : subs_)
        if (s.sid == j.sub.sid) {
          if (!j.full && s.initial)
            continue;  // (not possible: initial jobs are full)
          s.initial = false;
          s.seq = s.seq == 0xFFFFFFFFu ? 1 : s.seq + 1;
          s.failures = ok ? 0 : s.failures + 1;
        }
    }
  }
}

}  // namespace dlna
