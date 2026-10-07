#pragma once
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "AudioControl.h"
#include "BellTask.h"
#include "BellUtils.h"
#include "HTTPClient.h"
#include "Logger.h"
#include "StreamCoreFile.h"
#include "nlohmann/json.hpp"

/**
 * Common base class of all streams (radio / Spotify / Qobuz / plain HTTP).
 *
 * Besides the streaming plumbing it defines a uniform control and query
 * interface, so that a UI (web UI, touch display, buttons, ...) can drive any
 * stream through a `StreamBase*` without knowing the concrete type:
 *
 *   transport : pause(), resume(), togglePause(), next(), previous(),
 *               seek(), seekRelative(), seekPercent()
 *   modes     : setShuffle(), setRepeat()
 *   volume    : setVolume(), getVolume(), changeVolume()
 *   queue     : getQueue(), playQueueItem()
 *   state     : nowPlaying(), playbackState(), capabilities(), isActive()
 *   UI glue   : nowPlayingJson(), queueJson(), publishNowPlaying(),
 *               publishQueue(), handleCommand()
 *
 * Every method has a safe default. A stream that cannot do something returns
 * false and reports it in capabilities(), so the UI can grey out the control.
 */
class StreamBase : public bell::Task {
 public:
  using MetaCb = std::function<void(const std::string&, const std::string&)>;
  using ErrorCb = std::function<void(const std::string&)>;
  using StateCb = std::function<void(bool)>;
  using UiMessageCb = std::function<void(const std::string&)>;

  // ---------------------------------------------------------------- types --
  enum class PlaybackState : uint8_t { Stopped = 0, Buffering, Playing, Paused };
  enum class RepeatMode : uint8_t { Off = 0, All, One };

  /** What the stream supports right now (may change at runtime). */
  struct Capabilities {
    bool pause = false;
    bool seek = false;
    bool next = false;
    bool previous = false;
    bool shuffle = false;
    bool repeat = false;
    bool queue = false;          // getQueue() returns something useful
    bool playQueueItem = false;  // playQueueItem() works
    bool volume = true;
  };

  /** One entry of the queue / station list, also used for "now playing". */
  struct TrackInfo {
    std::string id;  // uri / track id / stream url
    std::string title;
    std::string artist;
    std::string album;  // radio: station name
    std::string imageUrl;
    uint32_t durationMs = 0;  // 0 = unknown / live
  };

  struct NowPlaying {
    PlaybackState state = PlaybackState::Stopped;
    TrackInfo track;
    uint32_t positionMs = 0;
    std::string source;   // "Spotify", "Qobuz", "Radio", ...
    std::string quality;  // human readable, e.g. "FLAC - 24-Bit / 96kHz"
    uint8_t volume = 0;   // 0..100
    bool shuffle = false;
    RepeatMode repeat = RepeatMode::Off;
    Capabilities caps;
  };

  explicit StreamBase(const char* taskName, std::shared_ptr<AudioControl> audio,
                      int stackSize = 1024 * 16, int prio = 1, int core = 1,
                      bool runOnPSRAM = true)
      : bell::Task(taskName, stackSize, prio, core, runOnPSRAM) {
    audio_ = audio;
    feed_ = std::make_shared<AudioControl::FeedControl>(audio_);
    volumePercent_.store(
        (uint8_t)std::min<size_t>(audio_ ? audio_->volume.load() : 0, 100));
    feed_->state_callback = [this](uint8_t state) {
      noteSinkState(state);
      if (state == 2)
        feedIsRunning_.store(true);
      if (onState_)
        onState_(state);
      if (state == 7)
        feedIsRunning_.store(false);
    };
  }

  virtual ~StreamBase() {}

  // Common callbacks
  void onMetadata(MetaCb cb) { onMeta_ = std::move(cb); }
  void onError(ErrorCb cb) { onError_ = std::move(cb); }
  void onState(StateCb cb) { onState_ = std::move(cb); }
  /** JSON messages for the UI ("playback", "queue"), see nowPlayingJson(). */
  void onUiMessage(UiMessageCb cb) { onUiMessage_ = std::move(cb); }

  // Start playback of a URI (meaning depends on derived stream)
  virtual void play(const std::string& uri,
                    const std::string& displayName = std::string()) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      targetUri_ = uri;
      displayName_ = displayName;
    }
    wantStop_.store(false);
    wantRestart_.store(true);
    if (!isRunning_.load())
      startTask();
  }

  // Stop and flush/close sink
  virtual void stop() {
    wantStop_.store(true);
    if (feed_) {
      feed_->feedCommand(AudioControl::FLUSH, 0);
      feed_->feedCommand(AudioControl::DISC, 0);
    }
    while (feedIsRunning_.load())
      sleepMs(10);
    setPlaybackState(PlaybackState::Stopped);
  }

  bool isRunning() const { return isRunning_.load(); }

  // ============================================================ control API

  /** Short name of the source, used as "src" in UI messages. */
  virtual const char* sourceName() const { return "Stream"; }

  /** True while this stream owns playback (connected / streaming). */
  virtual bool isActive() { return isRunning_.load(); }

  virtual Capabilities capabilities() {
    Capabilities c;
    c.pause = true;
    c.volume = true;
    return c;
  }

  // ---- transport ----
  virtual void pause() {
    if (feed_)
      feed_->feedCommand(AudioControl::PAUSE, 0);
    setPlaybackState(PlaybackState::Paused);
  }
  virtual void resume() {
    if (feed_)
      feed_->feedCommand(AudioControl::PLAY, 0);
    setPlaybackState(PlaybackState::Playing);
  }
  void togglePause() {
    if (playbackState() == PlaybackState::Paused)
      resume();
    else
      pause();
  }
  virtual PlaybackState playbackState() { return playState_.load(); }
  bool isPaused() { return playbackState() == PlaybackState::Paused; }

  /** Jump to the next track / station. */
  virtual bool next() { return false; }
  /** Previous track (or restart of the current one if it played > 3 s). */
  virtual bool previous() { return false; }

  /** Absolute seek inside the current track. */
  virtual bool seek(uint32_t positionMs) {
    (void)positionMs;
    return false;
  }
  /** Relative seek (e.g. +/-10 s buttons). */
  virtual bool seekRelative(int32_t deltaMs) {
    if (!capabilities().seek)
      return false;
    auto np = nowPlaying();
    int64_t target = (int64_t)np.positionMs + deltaMs;
    if (target < 0)
      target = 0;
    if (np.track.durationMs && target >= (int64_t)np.track.durationMs)
      target = (int64_t)np.track.durationMs - 1000;
    if (target < 0)
      target = 0;
    return seek((uint32_t)target);
  }
  /** Seek to 0..100 % of the current track (progress bar). */
  bool seekPercent(float percent) {
    auto np = nowPlaying();
    if (!np.track.durationMs)
      return false;
    percent = std::max(0.0f, std::min(100.0f, percent));
    return seek((uint32_t)((double)np.track.durationMs * percent / 100.0));
  }

  // ---- modes ----
  virtual bool setShuffle(bool on) {
    (void)on;
    return false;
  }
  virtual bool setRepeat(RepeatMode mode) {
    (void)mode;
    return false;
  }

  // ---- volume (0..100) ----
  virtual void setVolume(uint8_t percent) {
    percent = std::min<uint8_t>(percent, 100);
    volumePercent_.store(percent);
    if (feed_)
      feed_->feedCommand(AudioControl::VOLUME_LINEAR, (uint32_t)percent,
                         std::optional<uint32_t>(100));
  }
  virtual uint8_t getVolume() { return volumePercent_.load(); }
  void changeVolume(int delta) {
    int v = (int)getVolume() + delta;
    setVolume((uint8_t)std::max(0, std::min(100, v)));
  }

  // ---- queue / tracklist ----
  /**
   * Upcoming items, index 0 = the item after the current one.
   * Radio: the station list (index 0 = first station of the list).
   */
  virtual std::vector<TrackInfo> getQueue(size_t maxItems = 20) {
    (void)maxItems;
    return {};
  }
  /** Play an entry of getQueue() (same indexing). */
  virtual bool playQueueItem(size_t index) {
    (void)index;
    return false;
  }

  // ---- state ----
  virtual NowPlaying nowPlaying() {
    NowPlaying np;
    np.source = sourceName();
    np.state = playbackState();
    np.volume = getVolume();
    np.caps = capabilities();
    std::lock_guard<std::mutex> lk(mu_);
    np.track.id = targetUri_;
    np.track.title = displayName_;
    return np;
  }
  uint32_t getPositionMs() { return nowPlaying().positionMs; }

  // ============================================================ UI helpers

  static const char* toString(PlaybackState s) {
    switch (s) {
      case PlaybackState::Buffering:
        return "buffering";
      case PlaybackState::Playing:
        return "playing";
      case PlaybackState::Paused:
        return "paused";
      default:
        return "stopped";
    }
  }
  static const char* toString(RepeatMode m) {
    switch (m) {
      case RepeatMode::All:
        return "all";
      case RepeatMode::One:
        return "one";
      default:
        return "off";
    }
  }
  static std::optional<RepeatMode> repeatFromString(const std::string& s) {
    auto l = toLower(s);
    if (l == "off" || l == "none" || l == "0")
      return RepeatMode::Off;
    if (l == "all" || l == "context" || l == "1")
      return RepeatMode::All;
    if (l == "one" || l == "track" || l == "2")
      return RepeatMode::One;
    return std::nullopt;
  }

  static nlohmann::json toJson(const TrackInfo& t) {
    nlohmann::json j;
    j["id"] = t.id;
    j["title"] = t.title;
    j["artist"] = t.artist;
    j["album"] = t.album;
    j["image"] = t.imageUrl;
    j["duration_ms"] = t.durationMs;
    return j;
  }
  static nlohmann::json toJson(const Capabilities& c) {
    return nlohmann::json{{"pause", c.pause},
                          {"seek", c.seek},
                          {"next", c.next},
                          {"previous", c.previous},
                          {"shuffle", c.shuffle},
                          {"repeat", c.repeat},
                          {"queue", c.queue},
                          {"play_queue_item", c.playQueueItem},
                          {"volume", c.volume}};
  }

  /**
   * "playback" message. Keeps the keys the web UI already understands
   * (src, quality, state, position_ms, duration_ms, track{...}, volume) and
   * adds playback_state, shuffle, repeat and caps.
   */
  nlohmann::json nowPlayingJson() {
    auto np = nowPlaying();
    nlohmann::json j;
    j["type"] = "playback";
    j["src"] = np.source;
    j["quality"] = np.quality;
    j["state"] = (int)(np.state == PlaybackState::Playing);
    j["playback_state"] = toString(np.state);
    j["position_ms"] = np.positionMs;
    j["duration_ms"] = np.track.durationMs;
    j["volume"] = np.volume;
    j["shuffle"] = np.shuffle;
    j["repeat"] = toString(np.repeat);
    j["track"] = toJson(np.track);
    j["caps"] = toJson(np.caps);
    return j;
  }

  /** "queue" message: {"type":"queue","src":..,"items":[{index,...}]} */
  nlohmann::json queueJson(size_t maxItems = 20) {
    nlohmann::json j;
    j["type"] = "queue";
    j["src"] = sourceName();
    j["items"] = nlohmann::json::array();
    auto q = getQueue(maxItems);
    for (size_t i = 0; i < q.size(); i++) {
      auto e = toJson(q[i]);
      e["index"] = i;
      j["items"].push_back(std::move(e));
    }
    return j;
  }

  void publishNowPlaying() {
    if (onUiMessage_)
      onUiMessage_(nowPlayingJson().dump());
  }
  void publishQueue(size_t maxItems = 20) {
    if (onUiMessage_)
      onUiMessage_(queueJson(maxItems).dump());
  }

  /**
   * Dispatch a UI command, e.g. {"cmd":"next"} or {"cmd":"seek","value":42000}.
   *
   *  play | resume, pause, toggle | play_pause, next | skip_next,
   *  prev | previous | skip_previous,
   *  seek (ms), seek_relative (+/- ms), seek_percent (0..100),
   *  set_volume (0..100), volume_up / volume_down (step, default 5),
   *  shuffle (bool, toggles without value),
   *  repeat ("off"|"all"|"one", cycles without value),
   *  play_index (queue index), get_queue, get_state
   *
   * Returns false if the command is unknown or not supported by the stream.
   */
  bool handleCommand(const nlohmann::json& j) {
    if (!j.is_object())
      return false;
    std::string cmd;
    if (j.contains("cmd") && j["cmd"].is_string())
      cmd = j["cmd"].get<std::string>();
    else if (j.contains("command") && j["command"].is_string())
      cmd = j["command"].get<std::string>();
    if (cmd.empty())
      return false;
    const nlohmann::json value =
        j.contains("value") ? j["value"] : nlohmann::json();
    auto num = [&](double def) -> double {
      if (value.is_number())
        return value.get<double>();
      if (value.is_string()) {
        const auto& s = value.get_ref<const std::string&>();
        char* end = nullptr;
        double d = strtod(s.c_str(), &end);
        if (end != s.c_str())
          return d;
      }
      return def;
    };

    bool ok = true;
    if (cmd == "play" || cmd == "resume")
      resume();
    else if (cmd == "pause")
      pause();
    else if (cmd == "toggle" || cmd == "play_pause")
      togglePause();
    else if (cmd == "next" || cmd == "skip_next")
      ok = next();
    else if (cmd == "prev" || cmd == "previous" || cmd == "skip_previous")
      ok = previous();
    else if (cmd == "seek")
      ok = seek((uint32_t)std::max(0.0, num(0)));
    else if (cmd == "seek_relative")
      ok = seekRelative((int32_t)num(0));
    else if (cmd == "seek_percent")
      ok = seekPercent((float)num(0));
    else if (cmd == "set_volume")
      setVolume((uint8_t)std::max(0.0, std::min(100.0, num(getVolume()))));
    else if (cmd == "volume_up")
      changeVolume((int)num(5));
    else if (cmd == "volume_down")
      changeVolume(-(int)num(5));
    else if (cmd == "shuffle") {
      bool on = value.is_boolean() ? value.get<bool>()
                : value.is_number() ? value.get<double>() != 0
                                    : !nowPlaying().shuffle;
      ok = setShuffle(on);
    } else if (cmd == "repeat") {
      std::optional<RepeatMode> m;
      if (value.is_string())
        m = repeatFromString(value.get<std::string>());
      else if (value.is_number_integer())
        m = repeatFromString(std::to_string(value.get<int>()));
      else if (value.is_boolean())
        m = value.get<bool>() ? RepeatMode::All : RepeatMode::Off;
      else {  // cycle off -> all -> one -> off
        switch (nowPlaying().repeat) {
          case RepeatMode::Off:
            m = RepeatMode::All;
            break;
          case RepeatMode::All:
            m = RepeatMode::One;
            break;
          default:
            m = RepeatMode::Off;
            break;
        }
      }
      ok = m.has_value() && setRepeat(*m);
    } else if (cmd == "play_index" || cmd == "play_queue_item")
      ok = playQueueItem((size_t)std::max(0.0, num(0)));
    else if (cmd == "get_queue")
      publishQueue((size_t)std::max(1.0, num(20)));
    else if (cmd == "get_state")
      publishNowPlaying();
    else
      ok = false;
    return ok;
  }
  bool handleCommand(const std::string& json) {
    auto j = nlohmann::json::parse(json, nullptr, false);
    if (j.is_discarded())
      return false;
    return handleCommand(j);
  }

  // ============================================================ streaming

  virtual std::unique_ptr<bell::HTTPClient::Response> open(
      const std::string& uri, const std::string& displayName, uint32_t tid) {
    (void)tid;
    if (!displayName.empty())
      emitMeta(displayName, "");
    auto resp = bell::HTTPClient::get(uri);  // uses SocketStream under the hood
    if (!resp || resp->contentLength() ==
                     0) {  // len may be 0 for live streams; still ok
      reportError("DLNA: failed to open " + uri);
      return nullptr;
    }
    return resp;  // StreamBase::runTask() will pump resp->stream()
  }

  virtual int read(bell::SocketStream& is, uint8_t* dst, int n, uint32_t tid) {
    is.read(reinterpret_cast<char*>(dst), n);
    return is.gcount();
  }

  void runTask() override {

    isRunning_.store(true);

    while (isRunning_.load()) {
      if (wantStop_.load()) {
        isRunning_.store(false);
        break;
      }
      if (!wantRestart_.load()) {
        sleepMs(25);
        continue;
      }

      std::string uri;
      std::string name;
      {
        std::lock_guard<std::mutex> lk(mu_);
        uri = targetUri_;
        name = displayName_;
      }
      wantRestart_.store(false);
      if (uri.empty()) {
        sleepMs(100);
        continue;
      }

      // Bump track id so the sink treats this as a fresh stream and soft-stops previous

      const uint32_t tid = audio_->makeUniqueTrackId();
      if (onState_)
        onState_(true);
      setPlaybackState(PlaybackState::Buffering);
      auto resp = open(uri, name, tid);
      if (resp == nullptr) {
        isRunning_.store(false);
        break;
      }
      const size_t CHUNK = 1024;
      uint8_t buf[CHUNK];
      int ret = 0;
      while (!wantStop_.load()) {
        ret = read(resp->stream(), buf, 1024, tid);
        if (ret <= 0) {
          BELL_LOG(debug, "StreamBase", "read %d bytes", ret);
          break;
        }
        uint16_t written = 0;
        while (written < ret && !wantStop_.load()) {
          uint16_t ret_ =
              feed_->feedData(buf + written, ret - written, tid, false);
          if (ret_ == 0)
            BELL_SLEEP_MS(10);
          written += ret_;
        }
      }

      if (onState_)
        onState_(false);

      if (wantStop_.load()) {
        // final cleanup
        if (feed_) {
          feed_->feedCommand(AudioControl::FLUSH, 0);
          feed_->feedCommand(AudioControl::DISC, 0);
        }
        wantStop_.store(false);
        isRunning_.store(false);
        break;
      } else {
        if (feed_)
          feed_->feedCommand(AudioControl::SKIP, 0);
        sleepMs(reconnectDelayMs_);
        wantRestart_.store(ret < 0);  // reconnect only on unexpected end
      }
    }
    setPlaybackState(PlaybackState::Stopped);
  }

  // -------------- shared helpers --------------
  void emitMeta(const std::string& station, const std::string& title) {
    if (onMeta_)
      onMeta_(station, title);
  }

  void reportError(const std::string& msg) {
    SC32_LOG(error, "%s", msg.c_str());
    if (onError_)
      onError_(msg);
  }

  static void sleepMs(uint32_t ms) {
#ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(ms));
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
#endif
  }

 public:
  // utils
  static std::string toLower(std::string s) {
    for (char& c : s)
      c = (char)std::tolower((unsigned char)c);
    return s;
  }
  static bool startsWith(const std::string& s, const char* pfx) {
    return s.rfind(pfx, 0) == 0;
  }
  static bool endsWith(const std::string& s, const char* sfx) {
    size_t n = strlen(sfx);
    return s.size() >= n && std::equal(s.end() - n, s.end(), sfx);
  }
  static void ltrim(std::string& s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
              return !std::isspace(ch);
            }));
  }
  static void rtrim(std::string& s) {
    s.erase(std::find_if(s.rbegin(), s.rend(),
                         [](unsigned char ch) { return !std::isspace(ch); })
                .base(),
            s.end());
  }
  static void trim(std::string& s) {
    ltrim(s);
    rtrim(s);
  }
  static int toInt(std::string_view sv) {
    int v = 0;
    for (char c : sv)
      if (std::isdigit((unsigned char)c))
        v = v * 10 + (c - '0');
    return v;
  }
  static uint32_t parseUint(const std::string& s) {
    uint32_t v = 0;
    for (char c : s) {
      if (c < '0' || c > '9')
        break;
      v = v * 10 + (uint32_t)(c - '0');
      if (v > 36000u)
        break;
    }
    return v;
  }
  static std::string svToString(std::string_view sv) {
    return std::string(sv.data(), sv.size());
  }

  std::shared_ptr<AudioControl> audio_;
  std::shared_ptr<AudioControl::FeedControl> feed_;

  /** Receives "playback"/"queue" JSON (see nowPlayingJson()/queueJson()). */
  UiMessageCb onUiMessage_ = nullptr;

 protected:
  /**
   * Map the sink state (as delivered by FeedControl::state_callback) to
   * PlaybackState. Derived classes that replace feed_->state_callback should
   * call this first.  1/2 = playing, 3 = paused, 7 = stream ended.
   */
  void noteSinkState(uint8_t s) {
    switch (s) {
      case 1:
      case 2:
        playState_.store(PlaybackState::Playing);
        break;
      case 3:
        playState_.store(PlaybackState::Paused);
        break;
      case 7:
        playState_.store(PlaybackState::Stopped);
        break;
      default:
        break;
    }
  }
  void setPlaybackState(PlaybackState s) { playState_.store(s); }

  std::atomic<bool> isRunning_{false};
  std::atomic<bool> feedIsRunning_{false};
  std::atomic<bool> wantStop_{false};
  std::atomic<bool> wantRestart_{false};
  std::atomic<PlaybackState> playState_{PlaybackState::Stopped};
  std::atomic<uint8_t> volumePercent_{0};

  std::mutex mu_;
  std::string targetUri_;
  std::string displayName_;
  uint32_t trackId_ = 0;

  MetaCb onMeta_;
  ErrorCb onError_;
  StateCb onState_;

  const uint32_t reconnectDelayMs_ = 1500;
  static constexpr const char* ua_ = "StreamCore32/StreamBase (ESP32)";
};
