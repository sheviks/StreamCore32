// ============================
// include/streamcore/WebStream.h
// ============================
#pragma once
#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "AudioControl.h"
#include "BellTask.h"
#include "HTTPClient.h"
#include "Logger.h"
#include "MetaPoller.h"  // your existing poller helper
#include "StreamBase.h"

/**
 * Internet radio.
 *
 * StreamBase control API:
 *  - pause()/resume(): live streams can't really pause; reading is suspended
 *    and the sink paused. After more than liveResumeAfterMs (default 10 s)
 *    resume() reconnects so you are "live" again.
 *  - next()/previous()/playQueueItem()/getQueue(): work on the station list
 *    handed in with setStations() (e.g. favourites).
 *  - seek/shuffle/repeat: not supported (capabilities() says so).
 */
class WebStream : public StreamBase {
 public:
  using MetaCb = std::function<void(const std::string&, const std::string&)>;
  using ErrorCb = std::function<void(const std::string&)>;
  using StateCb = std::function<void(bool)>;

  struct MetaSpec {
    MetaPoller::Kind kind = MetaPoller::Kind::Auto;
    std::string url;             // optional explicit endpoint (abs or relative)
    uint32_t intervalMs = 5000;  // polling cadence
    bool enabled = true;
    bool fallbackOnEmptyICY = true;  // keep poller alive if ICY is empty
    bool autoDisarmOnICY = true;     // disarm poller once non-empty ICY seen
  };

  /** Entry of the station list used for next/previous and getQueue(). */
  struct Station {
    std::string name;
    std::string url;
    std::string imageUrl;  // favicon / logo (optional)
    std::string info;      // genre, country ... (optional, shown as album)
  };

  WebStream(std::shared_ptr<AudioControl> audio)
      : StreamBase("WebStream", audio, 1024 * 16, 1, 1, /*PSRAM stack*/ true) {
    // metadata poller (idles until armed)
    poller_ = std::make_shared<MetaPoller>(
        [this](const std::string& s, const std::string& t) {
          setSongTitle(t);
          if (onMeta_)
            onMeta_(s, t);
        },
        [this](const std::string& m) {
          if (onError_)
            onError_(m);
        });
    poller_->startTask();
  }

  ~WebStream() override {
    if (poller_) {
      poller_->stopTask();
      while (poller_->isRunning())
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    if (isRunning_.load())
      stop();
  }

  // ---- config ----
  void setMetaSpec(const MetaSpec& s) {
    std::lock_guard<std::mutex> lk(mu_);
    metaSpec_ = s;
  }

  /** Station list for next()/previous()/getQueue()/playQueueItem(). */
  void setStations(std::vector<Station> stations) {
    std::lock_guard<std::mutex> lk(metaMu_);
    stations_ = std::move(stations);
  }
  std::vector<Station> getStations() {
    std::lock_guard<std::mutex> lk(metaMu_);
    return stations_;
  }
  /** After a pause longer than this, resume() reconnects (0 = never). */
  void setLiveResumeAfterMs(uint32_t ms) { liveResumeAfterMs_.store(ms); }

  // ---- control ----
  // Can be called while playing — causes a seamless restart to new URI
  void play(const std::string& uri,
            const std::string& displayName = std::string()) override {
    {
      std::lock_guard<std::mutex> lk(mu_);
      targetUri_ = uri;
      displayName_ = displayName;
    }
    {
      std::lock_guard<std::mutex> lk(metaMu_);
      songTitle_.clear();
      songArtist_.clear();
    }
    paused_.store(false);
    wantRestart_.store(true);
    if (!isRunning_.load())
      startTask();
    else
      wantSwitch_.store(true);  // break the current read loop
  }

  void stop() override {
    wantStop_.store(true);
    wantRestart_.store(false);
    paused_.store(false);
    if (poller_)
      poller_->disarm();
    std::scoped_lock<std::mutex> lk(isRunningMutex_);
    setPlaybackState(PlaybackState::Stopped);
  }

  // ---- StreamBase control API ----
  const char* sourceName() const override { return "Radio"; }

  Capabilities capabilities() override {
    Capabilities c;
    c.pause = true;
    c.volume = true;
    std::lock_guard<std::mutex> lk(metaMu_);
    const size_t n = stations_.size();
    c.queue = n > 0;
    c.playQueueItem = n > 0;
    c.next = n > 1 || (n == 1 && currentStationIndexLocked() < 0);
    c.previous = c.next;
    return c;
  }

  void pause() override {
    if (!isRunning_.load())
      return;
    pausedAt_ = std::chrono::steady_clock::now();
    paused_.store(true);
    if (feed_)
      feed_->feedCommand(AudioControl::PAUSE, 0);
    setPlaybackState(PlaybackState::Paused);
    publishNowPlaying();
  }

  void resume() override {
    if (!isRunning_.load()) {
      // not running: restart the last station
      std::string uri, name;
      {
        std::lock_guard<std::mutex> lk(mu_);
        uri = targetUri_;
        name = displayName_;
      }
      if (!uri.empty())
        play(uri, name);
      return;
    }
    if (paused_.load()) {
      auto pausedFor = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - pausedAt_)
                           .count();
      const uint32_t limit = liveResumeAfterMs_.load();
      paused_.store(false);
      if (limit && pausedFor > (long long)limit) {
        // too old -> reconnect to be live again (drops the paused audio)
        wantRestart_.store(true);
        wantSwitch_.store(true);
        setPlaybackState(PlaybackState::Buffering);
        publishNowPlaying();
        return;
      }
    }
    if (feed_)
      feed_->feedCommand(AudioControl::PLAY, 0);
    setPlaybackState(PlaybackState::Playing);
    publishNowPlaying();
  }

  bool next() override { return stepStation(+1); }
  bool previous() override { return stepStation(-1); }

  std::vector<TrackInfo> getQueue(size_t maxItems = 20) override {
    std::vector<TrackInfo> out;
    std::lock_guard<std::mutex> lk(metaMu_);
    for (size_t i = 0; i < stations_.size() && out.size() < maxItems; i++) {
      TrackInfo t;
      t.id = stations_[i].url;
      t.title = stations_[i].name;
      t.album = stations_[i].info;
      t.imageUrl = stations_[i].imageUrl;
      out.push_back(std::move(t));
    }
    return out;
  }

  bool playQueueItem(size_t index) override {
    Station st;
    {
      std::lock_guard<std::mutex> lk(metaMu_);
      if (index >= stations_.size())
        return false;
      st = stations_[index];
    }
    play(st.url, st.name);
    return true;
  }

  /** Index of the playing station in the station list, -1 if not in it. */
  int currentStationIndex() {
    std::lock_guard<std::mutex> lk(metaMu_);
    return currentStationIndexLocked();
  }

  NowPlaying nowPlaying() override {
    NowPlaying np;
    np.source = sourceName();
    np.state = isRunning_.load() ? playbackState() : PlaybackState::Stopped;
    np.volume = getVolume();
    np.caps = capabilities();
    std::string uri, name;
    {
      std::lock_guard<std::mutex> lk(mu_);
      uri = targetUri_;
      name = displayName_;
    }
    std::lock_guard<std::mutex> lk(metaMu_);
    const std::string station =
        !curHeaders_.stationName.empty() ? curHeaders_.stationName : name;
    np.track.id = uri;
    np.track.title = songTitle_.empty() ? station : songTitle_;
    np.track.artist = songArtist_;
    np.track.album = station;
    int idx = currentStationIndexLocked();
    if (idx >= 0)
      np.track.imageUrl = stations_[idx].imageUrl;
    std::string q = curHeaders_.codec;
    if (curHeaders_.bitrateKbps) {
      if (!q.empty())
        q += " - ";
      q += std::to_string(curHeaders_.bitrateKbps) + " kbps";
    }
    if (curHeaders_.sampleRateHz) {
      if (!q.empty())
        q += " / ";
      q += std::to_string(curHeaders_.sampleRateHz / 1000) + "." +
           std::to_string((curHeaders_.sampleRateHz % 1000) / 100) + " kHz";
    }
    np.quality = q;
    if (np.state == PlaybackState::Playing && streamStartedAt_.time_since_epoch().count())
      np.positionMs = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - streamStartedAt_)
                          .count();
    return np;
  }

  struct IcyHeaders {
    int metaInt = 0;
    std::string contentType;
    std::string stationName;
    std::string codec;
    uint32_t bitrateKbps = 0;
    uint32_t sampleRateHz = 0;
    uint8_t channels = 0;
  };
  IcyHeaders& getIcyHeaders() { return H; }
  uint8_t state = 0;

  std::string getDisplayName() {
    std::lock_guard<std::mutex> lk(mu_);
    return displayName_;
  }
 protected:
  void runTask() override {
    std::scoped_lock<std::mutex> lk(isRunningMutex_);
    isRunning_.store(true);
    feed_->state_callback = [this](uint8_t s) {
      state = s;
      if (paused_.load() && (s == 1 || s == 2))
        return;  // keep "paused" while the sink still drains
      noteSinkState(s);
      if (s == 1 || s == 2 || s == 3)
        publishNowPlaying();
    };
    while (isRunning_.load()) {
      // stop() requested while (re)connecting: clean up and leave the task
      if (wantStop_.load() && !wantRestart_.load()) {
        if (feed_) {
          feed_->feedCommand(AudioControl::FLUSH, 0);
          feed_->feedCommand(AudioControl::DISC, 0);
        }
        if (poller_)
          poller_->disarm();
        wantStop_.store(false);
        isRunning_.store(false);
        break;
      }
      if (!wantRestart_.load()) {
        vTaskDelay(pdMS_TO_TICKS(25));
        continue;
      }

      std::string name;
      {
        std::lock_guard<std::mutex> lk(mu_);
        resolvedUri_ = targetUri_;
        name = displayName_;
      }
      wantRestart_.store(false);
      wantSwitch_.store(false);
      if (resolvedUri_.empty()) {
        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }
      setPlaybackState(PlaybackState::Buffering);
      reportError("Starting resolve");
      // Resolve possible playlists
      auto resolved = resolveIfPlaylist(resolvedUri_);
      if (!resolved) {
        reportError("resolve failed");
        isRunning_.store(false);
        continue;
      } else
        SC32_LOG(info, "Resolved to %s", (*resolved).c_str());
      wantRestart_.store(true);
      // bump track id so the sink treats it as new
      const uint32_t tid = audio_->makeUniqueTrackId();
      if (onState_)
        onState_(true);
      auto resp = open(*resolved, name, tid);
      if (resp == nullptr)
        continue;
      if (wantStop_.load() && !wantRestart_.load())
        continue;  // stop() came in while connecting
      wantStop_.store(false);
      streamStartedAt_ = std::chrono::steady_clock::now();
      publishNowPlaying();
      const size_t CHUNK = 1024;
      uint8_t buf[CHUNK];
      int ret = 0;
      while (!wantStop_.load() && !wantSwitch_.load()) {
        if (paused_.load()) {
          // live stream: don't read while paused (resume() reconnects if
          // the pause took too long)
          BELL_SLEEP_MS(50);
          continue;
        }
        ret = read(resp.get(), buf, CHUNK, tid);
        if (ret < 0)
          break;
        if (ret == 0) {
          BELL_SLEEP_MS(10);
          wantRestart_.store(true);
          wantStop_.store(true);
          break;
        }
        uint16_t written = 0;
        while (written < ret && !wantStop_.load() && !wantSwitch_.load()) {
          uint16_t ret_ =
              feed_->feedData(buf + written, ret - written, tid, false);
          if (ret_ == 0)
            BELL_SLEEP_MS(10);
          written += ret_;
        }
      }

      //const bool cleanEnd = streamOnce(*resolvedUri_, name, tid);
      if (onState_)
        onState_(false);

      if (wantSwitch_.load() && !wantStop_.load()) {
        // station change / live reconnect requested: drop the old stream
        // right away and start the new one without the reconnect delay
        wantSwitch_.store(false);
        if (feed_)
          feed_->feedCommand(AudioControl::SKIP, 0);
        if (poller_)
          poller_->disarm();
        wantRestart_.store(true);
        continue;
      }

      if (wantStop_.load() && !wantRestart_.load()) {
        if (feed_) {
          feed_->feedCommand(AudioControl::FLUSH, 0);
          feed_->feedCommand(AudioControl::DISC, 0);
        }
        if (poller_)
          poller_->disarm();
        wantStop_.store(false);
        isRunning_.store(false);
      } else {
        if (feed_)
          feed_->feedCommand(AudioControl::SKIP, 0);
        if (poller_)
          poller_->disarm();
        vTaskDelay(pdMS_TO_TICKS(reconnectDelayMs_));
        // reconnect if unexpected end or user queued another play
        // (but not when stop() came in during the delay)
        if (!(wantStop_.load() && !wantRestart_.load()))
          wantRestart_.store(true);
      }
    }
    // wait (bounded) until the sink reports the end of the stream
    for (int i = 0; state != 7 && i < 500; i++)
      BELL_SLEEP_MS(10);
    isRunning_.store(false);
    setPlaybackState(PlaybackState::Stopped);
    publishNowPlaying();
  }

 private:
  int currentStationIndexLocked() {
    std::string uri;
    {
      std::lock_guard<std::mutex> lk(mu_);
      uri = targetUri_;
    }
    for (size_t i = 0; i < stations_.size(); i++)
      if (stations_[i].url == uri)
        return (int)i;
    return -1;
  }

  bool stepStation(int dir) {
    Station st;
    {
      std::lock_guard<std::mutex> lk(metaMu_);
      const int n = (int)stations_.size();
      if (n == 0)
        return false;
      int cur = currentStationIndexLocked();
      int idx;
      if (cur < 0)
        idx = dir > 0 ? 0 : n - 1;
      else {
        if (n == 1)
          return false;
        idx = ((cur + dir) % n + n) % n;
      }
      st = stations_[idx];
    }
    play(st.url, st.name);
    return true;
  }

  /** Store ICY / poller title ("Artist - Title") for nowPlaying(). */
  void setSongTitle(const std::string& raw) {
    std::string artist, title = raw;
    auto p = raw.find(" - ");
    if (p != std::string::npos) {
      artist = raw.substr(0, p);
      title = raw.substr(p + 3);
      trim(artist);
      trim(title);
    }
    {
      std::lock_guard<std::mutex> lk(metaMu_);
      if (songTitle_ == title && songArtist_ == artist)
        return;
      songTitle_ = title;
      songArtist_ = artist;
    }
    publishNowPlaying();
  }

  std::unique_ptr<bell::HTTPClient::Response> open(const std::string& url,
                                                   const std::string& station,
                                                   uint32_t trackId) override {

    bell::HTTPClient::Headers hdrs = {{"Icy-MetaData", "1"},
                                      {"User-Agent", ua_},
                                      {"Accept-Encoding", "identity"},
                                      {"Upgrade-Insecure-Requests", "1"}};
    auto resp = bell::HTTPClient::get(url, hdrs, true, 32);
    if (!resp) {
      reportError("HTTP connect failed");
      return nullptr;
    }
    isChunked_ = false;
    chunkBytesRemaining_ = 0;
    IcyHeaders h;  // fresh per connection (don't inherit the old station)
    auto hl = resp->headers();
    for (auto& header : hl) {
      std::string lh = toLower(header.first);
      if (lh == "content-type") {
        std::string lc = toLower(header.second);
        h.contentType = header.second;
        if (lc.find("audio/mpeg") != std::string::npos ||
            lc.find("audio/mp3") != std::string::npos ||
            lc.find("audio/x-mpeg") != std::string::npos) {
          h.codec = "Mp3";
        } else if (lc.find("audio/aac") != std::string::npos ||
                   lc.find("aacp") != std::string::npos ||
                   lc.find("audio/aacp") != std::string::npos ||
                   lc.find("audio/mp4") != std::string::npos ||
                   lc.find("application/aac") != std::string::npos) {
          h.codec = "AAC";
          BELL_LOG(debug, "webstream", "AAC");
        } else if (lc.find("audio/ogg") != std::string::npos ||
                   lc.find("application/ogg") != std::string::npos) {
          if (lc.find("opus") != std::string::npos)
            h.codec = "Opus";
          else if (lc.find("vorbis") != std::string::npos)
            h.codec = "Vorbis";
          else
            h.codec = "Ogg";
        } else if (lc.find("audio/wav") != std::string::npos ||
                   lc.find("audio/x-wav") != std::string::npos ||
                   lc.find("audio/l16") != std::string::npos) {
          h.codec = "Pcm";
        } else if (lc.find("audio/flac") != std::string::npos ||
                   lc.find("flac") != std::string::npos) {
          h.codec = "FLAC";
        } else
          h.codec = "unknown";
      } else if ((lh == "icy-name" || lh == "name") && !header.second.empty()) {
        h.stationName = header.second;
      } else if ((lh == "icy-br" || lh == "icy-bitrate" || lh == "br") &&
                 !header.second.empty()) {
        h.bitrateKbps = toInt(header.second);
        if (h.bitrateKbps > 320 && h.bitrateKbps < 2000000)
          h.bitrateKbps /= 1000;
      } else if ((lh == "icy-sr" || lh == "samplerate" || lh == "sr") &&
                 !header.second.empty()) {
        h.sampleRateHz = toInt(header.second);
      } else if ((lh == "icy-channels" || lh == "channels" || lh == "ch") &&
                 !header.second.empty()) {
        h.channels = static_cast<uint8_t>(toInt(header.second));
      } else if (lh == "icy-metaint") {
        h.metaInt = toInt(header.second);
      } else if (lh == "transfer-encoding" &&
                 header.second.find("chunked") != std::string::npos) {
        isChunked_ = true;
      }
    }
    if (h.stationName.empty())
      h.stationName = station;
    H = h;
    {
      std::lock_guard<std::mutex> lk(metaMu_);
      curHeaders_ = h;
    }
    SC32_LOG(info, "headers: %s %d %s", H.contentType.c_str(), H.metaInt,
             H.stationName.c_str());

    hadNonEmptyICY_ = false;
    if (poller_ && metaSpec_.enabled &&
        metaSpec_.kind != MetaPoller::Kind::Disabled) {
      MetaPoller::Spec ps;
      ps.kind = (MetaPoller::Kind)metaSpec_.kind;
      ps.url = metaSpec_.url;
      ps.intervalMs = metaSpec_.intervalMs;
      ps.enabled = metaSpec_.enabled;
      if (H.metaInt <= 0)
        poller_->arm(originFromUrl(url), H.stationName, ps);
      else if (metaSpec_.fallbackOnEmptyICY && H.stationName.empty())
        poller_->arm(originFromUrl(url), H.stationName, ps);
      else
        poller_->disarm();
    }
    bytesUntilMeta =
        (H.metaInt > 0) ? H.metaInt : -1;
    return resp;
  }
  int read(bell::HTTPClient::Response* stream, uint8_t* buffer,
           size_t chunk_size, size_t trackId) {

    size_t want = (bytesUntilMeta > 0)
                      ? (size_t)std::min<int>(bytesUntilMeta, (int)chunk_size)
                      : chunk_size;
    int got = 0;
    if (!isChunked_) {
      got = stream->read(buffer, want);
    } else {
      got = readChunkedBody(stream, buffer, want);
    }
    if (got <= 0)
      return got;
    if(bytesUntilMeta > 0) bytesUntilMeta -= (int)got;
    if (bytesUntilMeta == 0) {

      uint8_t Lbyte = 0;
      int r;
      if (!isChunked_) {
        if (stream->read(&Lbyte, 1) != 1)
          return -1;
      } else {
        r = readChunkedBody(stream, &Lbyte, 1);
        if (r <= 0)
          return r;
      }
      int metaLen = (int)Lbyte * 16;
      if (metaLen > 0) {
        std::string meta;
        meta.resize((size_t)metaLen);
        if (!isChunked_) {
          stream->readExact(reinterpret_cast<uint8_t*>(&meta[0]), metaLen);
          // readExact already handles short reads
        } else {
          int m = readChunkedBody(stream, reinterpret_cast<uint8_t*>(&meta[0]),
                                  (size_t)metaLen);
          if (m != metaLen) {
            // truncated metadata; bail out
            return -1;
          }
        }
        parseAndEmitIcy(meta, H.stationName);
      } else if (poller_ && poller_->isRunning() && metaSpec_.enabled &&
                 metaSpec_.kind != MetaPoller::Kind::Disabled &&
                 metaSpec_.fallbackOnEmptyICY && !hadNonEmptyICY_) {
        MetaPoller::Spec ps;
        ps.kind = MetaPoller::Kind::Auto;
        ps.url = metaSpec_.url;
        ps.intervalMs = metaSpec_.intervalMs;
        ps.enabled = metaSpec_.enabled;
        poller_->arm(originFromUrl(resolvedUri_), H.stationName, ps);
      }
      bytesUntilMeta =
          (H.metaInt > 0) ? H.metaInt : std::numeric_limits<int>::max();
    }
    return got;
  }
  // ---------- playlist helpers ----------
  static bool hasPlaylistExt(const std::string& u) {
    auto L = toLower(u);
    return endsWith(L, ".m3u") || endsWith(L, ".m3u8") || endsWith(L, ".pls");
  }

  std::optional<std::string> resolveIfPlaylist(const std::string& url) {
    if (hasPlaylistExt(url))
      return fetchPlaylist(url);
    reportError("Checking content-type for playlist");
   
    bell::HTTPClient::Headers hdrs = {{"Icy-MetaData", "1"},
                                      {"User-Agent", ua_},
                                      {"Accept-Encoding", "identity"},
                                      {"Upgrade-Insecure-Requests", "1"}};
    auto resp = bell::HTTPClient::get(url, hdrs, false,  32);
    reportError("Fetched for playlist check");
    if (!resp){
      reportError("HTTP connect failed");
      return std::nullopt;
    }
    reportError("Fetched for playlist check");
    if(resp->status() == 302){
      reportError("Redirected to " + svToString(resp->header("location")));
      return resolveIfPlaylist(svToString(resp->header("location")));
    } else if(resp->status() != 200){
      reportError("HTTP status " + std::to_string(resp->status()));
      return std::nullopt;
    }
    auto ctype = svToString(resp->header("content-type"));
    if (startsWith(toLower(ctype), "audio/"))
      return url;
    else reportError("Content-Type: " + ctype);
    if (isPlaylistContentType(ctype)) {
      auto body_sv = resp->body();
      std::string body(body_sv.data(), body_sv.size());
      return parsePlaylistBody(body);
    }
    return url;
  }

  std::optional<std::string> fetchPlaylist(const std::string& url) {

    bell::HTTPClient::Headers hdrs = {{"User-Agent", ua_},
                                      {"Accept-Encoding", "identity"},
                                      {"Upgrade-Insecure-Requests", "1"}};
    auto resp = bell::HTTPClient::get(url, hdrs, false,  32);
    if (!resp)
      return std::nullopt;
    auto body_sv = resp->body();
    std::string body(body_sv.data(), body_sv.size());
    return parsePlaylistBody(body);
  }

  static bool isPlaylistContentType(const std::string& ct) {
    auto L = toLower(ct);
    return startsWith(L, "audio/x-mpegurl") ||
           startsWith(L, "application/vnd.apple.mpegurl") ||
           startsWith(L, "application/x-mpegURL") ||
           startsWith(L, "application/pls") || startsWith(L, "audio/x-scpls") ||
           startsWith(L, "text/");
  }

  static std::optional<std::string> parsePlaylistBody(const std::string& body) {
    size_t pos = 0;
    bool first = true;
    while (pos < body.size()) {
      size_t end = body.find_first_of("\r\n", pos);
      size_t len =
          (end == std::string::npos) ? (body.size() - pos) : (end - pos);
      std::string line = body.substr(pos, len);
      if (end == std::string::npos)
        pos = body.size();
      else
        pos = (body[end] == '\r' && end + 1 < body.size() &&
               body[end + 1] == '\n')
                  ? end + 2
                  : end + 1;
      if (first && line.size() >= 3 && (unsigned char)line[0] == 0xEF &&
          (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
        line.erase(0, 3);
      first = false;
      trim(line);
      if (line.empty())
        continue;
      if (line[0] == '#' || line[0] == ';' || line[0] == '[')
        continue;
      auto eq = line.find('=');
      std::string cand = (eq != std::string::npos) ? line.substr(eq + 1) : line;
      trim(cand);
      if (startsWith(cand, "http://") || startsWith(cand, "https://"))
        return cand;
    }
    return std::nullopt;
  }

  // ---------- streaming ----------

  bool streamOnce(const std::string& url, const std::string& station,
                  uint32_t trackId) {
    bell::HTTPClient::Headers hdrs = {{"Icy-MetaData", "1"},
                                      {"User-Agent", ua_},
                                      {"Accept-Encoding", "identity"},
                                      {"Upgrade-Insecure-Requests", "1"}};
    auto resp = bell::HTTPClient::get(url, hdrs, false,  32);
    if (!resp) {
      reportError("HTTP connect failed");
      return false;
    }

    IcyHeaders H;
    auto h = resp->headers();
    for (auto& header : h) {
      BELL_LOG(debug, "webstream", "Checking header: %s: %s",
               header.first.c_str(), header.second.c_str());
    }
    H.metaInt = toInt(resp->header("icy-metaint"));
    std::string meta = svToString(resp->header("icy-audio-info"));
    std::string br = svToString(resp->header("icy-br"));

    H.contentType = svToString(resp->header("content-type"));
    H.stationName = svToString(resp->header("icy-name"));
    if (H.stationName.empty())
      H.stationName = station;
    SC32_LOG(info, "headers: %s %d %s", H.contentType.c_str(), H.metaInt,
             H.stationName.c_str());

    hadNonEmptyICY_ = false;
    if (poller_ && metaSpec_.enabled &&
        metaSpec_.kind != MetaPoller::Kind::Disabled) {
      MetaPoller::Spec ps;
      ps.kind = (MetaPoller::Kind)metaSpec_.kind;
      ps.url = metaSpec_.url;
      ps.intervalMs = metaSpec_.intervalMs;
      ps.enabled = metaSpec_.enabled;
      if (H.metaInt <= 0)
        poller_->arm(originFromUrl(url), H.stationName, ps);
      else if (metaSpec_.fallbackOnEmptyICY && H.stationName.empty())
        poller_->arm(originFromUrl(url), H.stationName, ps);
      else
        poller_->disarm();
    }

    auto& is = resp->stream();
    const size_t CHUNK = 1024;
    std::vector<uint8_t> buf(CHUNK);
    int bytesUntilMeta =
        (H.metaInt > 0) ? H.metaInt : std::numeric_limits<int>::max();
    isRunning_.store(true);
    while (isRunning_.load()) {
      size_t want = (size_t)std::min<int>(bytesUntilMeta, (int)CHUNK);
      auto got = resp->read(buf.data(), want);
      if (got <= 0)
        break;
      size_t fed = feed_->feedData(buf.data(), (size_t)got, trackId, false);
      if (fed == 0)
        vTaskDelay(pdMS_TO_TICKS(1));
      bytesUntilMeta -= (int)got;
      if (bytesUntilMeta == 0) {
        uint8_t L = 0;
        if (resp->read(&L, 1) != 1)
          break;
        int metaLen = L * 16;
        if (metaLen > 0) {
          std::string meta;
          meta.resize((size_t)metaLen);
          resp->readExact(reinterpret_cast<uint8_t*>(&meta[0]), metaLen);
          parseAndEmitIcy(meta, H.stationName);
        } else if (poller_ && poller_->isRunning() && metaSpec_.enabled &&
                   metaSpec_.kind != MetaPoller::Kind::Disabled &&
                   metaSpec_.fallbackOnEmptyICY && !hadNonEmptyICY_) {
          MetaPoller::Spec ps;
          ps.kind = MetaPoller::Kind::Auto;
          ps.url = metaSpec_.url;
          ps.intervalMs = metaSpec_.intervalMs;
          ps.enabled = metaSpec_.enabled;
          poller_->arm(originFromUrl(url), H.stationName, ps);
        }
        bytesUntilMeta =
            (H.metaInt > 0) ? H.metaInt : std::numeric_limits<int>::max();
      }
      vTaskDelay(pdMS_TO_TICKS(1));
    }

    return true;  // true if asked to stop; false = unexpected end
  }

  // ---------- ICY helpers ----------
  static std::string parseStreamTitle(const std::string& meta) {
    std::string m = meta;
    // remove embedded NULs
    m.erase(std::remove(m.begin(), m.end(), '\0'), m.end());

    std::string ml = toLower(m);
    const std::string key = "streamtitle=";
    size_t p = ml.find(key);
    if (p == std::string::npos)
      return {};

    size_t v = p + key.size();
    if (v >= m.size())
      return {};

    // skip whitespace after '='
    while (v < m.size() && (m[v] == ' ' || m[v] == '\t'))
      ++v;
    if (v >= m.size())
      return {};

    // find end of value at first ';' (or end of string)
    size_t end = m.find(';', v);
    if (end == std::string::npos)
      end = m.size();

    size_t s = v;
    size_t e = end;

    // strip *outer* quotes only, keep inner quotes like in DESTINY'S
    if (s < e && (m[s] == '\'' || m[s] == '"')) {
      char quote = m[s];
      ++s;
      if (e > s && m[e - 1] == quote) {
        --e;
      }
    }

    std::string val = m.substr(s, e - s);
    trim(val);
    return val;
  }
  void parseAndEmitIcy(const std::string& raw, const std::string& station) {
    auto title = parseStreamTitle(raw);
    if (!title.empty()) {
      hadNonEmptyICY_ = true;
      setSongTitle(title);
      if (onMeta_)
        onMeta_(station, title);
      if (poller_ && metaSpec_.autoDisarmOnICY)
        poller_->disarm();
    }
  }
  static bool readLine(bell::HTTPClient::Response* is, std::string& out) {
    out.clear();
    char c;
    while (true) {
      if (!is->read(reinterpret_cast<uint8_t*>(&c), 1))
        return false;
      if (c == '\r') {
        // expect \n
        if (!is->read(reinterpret_cast<uint8_t*>(&c), 1))
          return false;
        return true;
      }
      if (c == '\n')
        return true;
      out.push_back(c);
    }
  }

  static size_t parseHexSize(const std::string& s) {
    size_t val = 0;
    for (char c : s) {
      if (c == ';' || c == ' ' || c == '\t')
        break;  // ignore extensions
      int v = -1;
      if (c >= '0' && c <= '9')
        v = c - '0';
      else if (c >= 'a' && c <= 'f')
        v = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F')
        v = c - 'A' + 10;
      else
        break;
      val = (val << 4) | (size_t)v;
    }
    return val;
  }
  int readChunkedBody(bell::HTTPClient::Response* stream, uint8_t* dst, size_t max) {
    size_t out = 0;

    while (out < max) {
      // Need a new chunk?
      if (chunkBytesRemaining_ == 0) {
        std::string line;
        if (!readLine(stream, line)) {
          return (out > 0) ? (int)out : -1;  // EOF/error
        }

        size_t sz = parseHexSize(line);
        if (sz == 0) {
          // terminal chunk: spec says trailers follow, but for streaming we just stop
          return (int)out;
        }
        chunkBytesRemaining_ = sz;
      }

      size_t want = std::min(chunkBytesRemaining_, max - out);
      auto got = stream->read(dst + out, want);
      if (got <= 0) {
        return (out > 0) ? (int)out : (int)got;
      }

      chunkBytesRemaining_ -= (size_t)got;
      out += (size_t)got;

      if (chunkBytesRemaining_ == 0) {
        // Consume CRLF after the chunk body
        char crlf[2];
        stream->read(reinterpret_cast<uint8_t*>(crlf), 2);
        // don’t care if it's exactly "\r\n", we’re just best effort
      }
    }

    return (int)out;
  }
  static std::string originFromUrl(const std::string& url) {
    auto p = url.find("://");
    if (p == std::string::npos)
      return url;
    auto start = p + 3;
    auto slash = url.find('/', start);
    if (slash == std::string::npos)
      return url.substr(0, url.size());
    return url.substr(0, slash);
  }

 private:
  std::shared_ptr<MetaPoller> poller_;

  // state (isRunning_, wantStop_, wantRestart_, mu_, targetUri_,
  // displayName_ and the callbacks live in StreamBase)
  std::mutex isRunningMutex_;
  std::atomic<bool> wantSwitch_{false};  // restart with the new target now
  std::atomic<bool> paused_{false};
  std::atomic<uint32_t> liveResumeAfterMs_{10000};
  std::chrono::steady_clock::time_point pausedAt_{};
  std::chrono::steady_clock::time_point streamStartedAt_{};
  bool isChunked_ = false;
  size_t chunkBytesRemaining_ = 0;
  std::string resolvedUri_;

  // guarded by metaMu_ (read from UI threads)
  std::mutex metaMu_;
  std::vector<Station> stations_;
  std::string songTitle_;
  std::string songArtist_;
  IcyHeaders curHeaders_{};

  MetaSpec metaSpec_{};
  IcyHeaders H{};
  bool hadNonEmptyICY_ = false;
  int bytesUntilMeta = std::numeric_limits<int>::max();

  static constexpr const char* ua_ = "StreamCore32/WebStream (ESP32)";
};
