#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "BellTask.h"

#include "DeviceStateHandler.h"
#include "Logger.h"
#include "StreamBase.h"
#include "StreamCoreFile.h"
#include "ZeroConfServer.h"

/** TODO**
 * when spotify is running but not active, log 178 and 181
 */
#ifdef CONFIG_SPOTIFY_DISCOVERY_MODE_OPEN
static constexpr bool kDiscoveryOpen = true;
#else
static constexpr bool kDiscoveryOpen = false;
#endif

class SpotifyStream : public StreamBase {
 private:
  std::unique_ptr<StreamCoreFile> creds;
  std::function<void(bool)> onConnect_ = nullptr;
  std::string deviceName_;

 public:
  using OnUiMessage = std::function<void(const std::string&)>;
  using MetaCb = std::function<void(const std::string&, const std::string&)>;
  using ErrorCb = std::function<void(const std::string&)>;
  using StateCb = std::function<void(bool)>;
  // NOTE: written by several tasks; read it through getHandler()
  std::shared_ptr<spotify::DeviceStateHandler> handler = nullptr;
  std::shared_ptr<ZeroconfAuthenticator> zeroconfServer;
  std::shared_ptr<spotify::Context> ctx;
  std::atomic<bool> isRunning = false;
  std::atomic<bool> isConnected = false;
  std::string currentUserName = "";
  /** deviceName: name shown in the Spotify apps ("" = sdkconfig default) */
  SpotifyStream(std::shared_ptr<AudioControl> _audioController,
                std::unique_ptr<StreamCoreFile> _creds,
                std::function<void(bool)> _onConnect,
                const std::string& deviceName = "")
      : StreamBase("Spotify", _audioController, 1024 * 20, 1, 1, 1),
        creds(std::move(_creds)),
        onConnect_(_onConnect),
        deviceName_(deviceName.empty() ? std::string(CONFIG_SC32_DEVICE_NAME)
                                       : deviceName) {  // cannot run on PSRAM because of NVS
    SC32_LOG(info, "Starting SpotifyStream");
    feed_->state_callback = [this](uint8_t state) {
      auto h = getHandler();
      if (h == nullptr || !h->is_active)
        return;
      if (h->trackQueue == nullptr ||
          h->trackQueue->preloadedTracks.size() == 0)
        return;

      std::shared_ptr<spotify::QueuedTrack> track =
          h->trackQueue->preloadedTracks[0];
      noteSinkState(state);
      switch (state) {
        case 1:
          setCurrentTrack(track);
          track->trackMetrics->startTrackPlaying(track->requestedPosition);
          h->putPlayerState();
          publishQueue();
          break;
        case 2:
          [[fallthrough]];
        case 3:
          setCurrentTrack(track);
          publishNowPlaying();
          break;
        case 7:
          publishNowPlaying();
          break;
        default:
          break;
      }
    };
    // BELL_SLEEP_MS(1000); // wait for audio controller to be ready
    SC32_LOG(info, "Starting ZeroconfAuthenticator");
    this->zeroconfServer =
        std::make_shared<ZeroconfAuthenticator>(deviceName_);
    this->zeroconfServer->onClose = [this]() {
      if (this->isRunning) {
        auto h = getHandler();
        if (h && h->isRunning.load())
          setHandler(nullptr);
        this->isRunning.store(false);
        this->onStartup();
      }
    };
    zeroconfServer->onAuthSuccess =
        [this](std::shared_ptr<spotify::LoginBlob> blob) {
          isRunning.store(false);
          this->onAuthSuccess(blob);
        };
    SC32_LOG(info, "Starting Task");
    if (kDiscoveryOpen) {
      zeroconfServer->blob =
          std::make_shared<spotify::LoginBlob>(deviceName_);
      if (!zeroconfServer->isRunning.load())
        zeroconfServer->registerMdnsService();
    }
    onStartup();
  }

  void onLoginSuccess(std::shared_ptr<spotify::LoginBlob> blob) {
    Record r;
    r.userkey = blob->username;
    currentUserName = blob->username;
    r.fields.push_back(
        Field{"authType",
              std::vector<uint8_t>{static_cast<uint8_t>(blob->authType)}});
    r.fields.push_back(Field{"authData", blob->authData});
    creds->save(r, true);
    creds->set_current(r.userkey);
  }
  void stop() override {
    auto h = getHandler();
    if (h != nullptr) {
      h->disconnect();
      setHandler(nullptr);
    }
    setCurrentTrack(nullptr);
    setPlaybackState(PlaybackState::Stopped);
  }
  void onClose(bool logout) {
    if (logout) {
      creds->erase(currentUserName);
      SC32_LOG(info, "Logout");
    }
    setCurrentTrack(nullptr);
    setPlaybackState(PlaybackState::Stopped);
    if (isRunning.load() == false)
      return;
    isRunning.store(false);
    onStartup();
  }
  void onAuthSuccess(std::shared_ptr<spotify::LoginBlob> blob) {
    onConnect_(true);
    zeroconfServer->blob = blob;
    startTask();
  }
  void runTask() override {
    //if(CONFIG_SPOTIFY_DISCOVERY_MODE_OPEN){
    if (auto old = getHandler()) {

      SC32_LOG(info, "Resetting handler");
      old->disconnect();
      setHandler(nullptr);
      SC32_LOG(info, "Handler reset");
    }
    //}
    SC32_LOG(info, "Login success");
    if (!isRunning.load()) {
      isRunning.store(true);
      try {
        std::function<void(bool)> _onClose = [this](bool logout) {
          this->onClose(logout);
          onConnect_(false);
        };
        std::function<void()> _onTransfer = [this]() {
        };
        std::function<void(const std::string&, const std::vector<uint8_t>&)>
            _onLoginSuccess = [this](const std::string& username,
                                     const std::vector<uint8_t>& auth_data) {
              this->onLoginSuccess(std::make_shared<spotify::LoginBlob>(
                  deviceName_, username, auth_data));
            };
        std::function<uint16_t()> _getVolume = [this]() {
          return this->feed_->audioSink->get_logarithmic_volume<uint16_t>(
              this->audio_->volume);
        };
        auto h = std::make_shared<spotify::DeviceStateHandler>(
            this->zeroconfServer->blob, _onClose, _onTransfer, _onLoginSuccess,
            _getVolume);
        if (!kDiscoveryOpen) {
          if (zeroconfServer->isRunning.load())
            zeroconfServer->unregisterMdnsService();
        }
        h->trackPlayer->dataCallback =
            [fC = feed_](uint8_t* data, size_t bytes, size_t trackId,
                         bool VOLATILE) {
              return fC->feedData(data, bytes, trackId, VOLATILE);
            };
        h->stateToSinkCallback = [this](spotify::Command cmd) {
          auto map = [](spotify::CommandType t)
              -> std::optional<AudioControl::CommandType> {
            using CT = spotify::CommandType;
            using AC = AudioControl::CommandType;
            switch (t) {
              case CT::PLAY:
                return AC::PLAY;
              case CT::PAUSE:
                return AC::PAUSE;
              case CT::DISC:
                return AC::DISC;
              case CT::FLUSH:
                return AC::FLUSH;
              case CT::SKIP_NEXT:
              case CT::SKIP_PREV:
                return AC::SKIP;
              case CT::VOLUME:
                return AC::VOLUME_LOGARITHMIC;
              default:
                return std::nullopt;
                // e.g. PLAYBACK /* std::shared_ptr<spotify::QueuedTrack> track = std::get<std::shared_ptr<spotify::QueuedTrack>>(command.data);*/
            }
          };
          // keep the UI state in sync with remote commands
          switch (cmd.commandType) {
            case spotify::CommandType::PAUSE:
              setPlaybackState(PlaybackState::Paused);
              break;
            case spotify::CommandType::PLAY:
              setPlaybackState(PlaybackState::Playing);
              break;
            case spotify::CommandType::DISC:
              setPlaybackState(PlaybackState::Stopped);
              break;
            default:
              break;
          }
          if (auto ac = map(cmd.commandType)) {
            uint16_t value = 0;
            if (*ac == AudioControl::CommandType::VOLUME_LOGARITHMIC) {
              if (auto p = std::get_if<int32_t>(&cmd.data))
                value = *p;
              volumePercent_.store((uint8_t)(((uint32_t)value * 100) / 65535));
            }
            this->feed_->feedCommand(*ac, value);
          }
          if (cmd.commandType == spotify::CommandType::PAUSE ||
              cmd.commandType == spotify::CommandType::PLAY ||
              cmd.commandType == spotify::CommandType::SEEK ||
              cmd.commandType == spotify::CommandType::SET_SHUFFLE ||
              cmd.commandType == spotify::CommandType::SET_REPEAT ||
              cmd.commandType == spotify::CommandType::VOLUME)
            publishNowPlaying();
        };
        h->onQueueChanged = [this]() {
          publishQueue();
        };
        setHandler(h);
        h->ctx->session->startTask();
        h->startTask();
      } catch (std::exception& e) {
        SC32_LOG(error, "Error while connecting %s", e.what());
        isRunning.store(false);
        return this->onClose(false);
      }
    }
  }

  void onStartup() {
    /*
    Record r;
    r.fields.push_back(Field{"authType", std::vector<uint8_t>()});
    r.fields.push_back(Field{"authData", std::vector<uint8_t>()});
    if (creds->get_current(&r) == 0) {
      SC32_LOG(info, "Found startup credentials");
      onAuthSuccess(std::make_shared<spotify::LoginBlob>(deviceName_,
                      r.userkey, r.fields[1].value));
    } else
    */
    if (zeroconfServer->isRunning.load() == false) {
      zeroconfServer->blob =
          std::make_shared<spotify::LoginBlob>(deviceName_);
      zeroconfServer->registerMdnsService();
    }
  }

  // ======================================================= StreamBase API

  const char* sourceName() const override { return "Spotify"; }

  bool isActive() override {
    auto h = getHandler();
    return h && h->is_active;
  }

  Capabilities capabilities() override {
    Capabilities c;
    c.volume = true;
    auto h = getHandler();
    if (!h || !h->is_active)
      return c;
    c.pause = c.seek = c.next = c.previous = true;
    c.queue = c.playQueueItem = true;
    // Spotify disallows shuffle/repeat while autoplay/radio is running
    auto track = getCurrentTrack();
    const bool autoplay = track && track->trackInfo.provider == "autoplay";
    c.shuffle = c.repeat = !autoplay;
    return c;
  }

  void pause() override {
    if (command({{"endpoint", "pause"}}))
      setPlaybackState(PlaybackState::Paused);
  }
  void resume() override {
    if (command({{"endpoint", "resume"}}))
      setPlaybackState(PlaybackState::Playing);
  }
  void play(const std::string& uri,
            const std::string& displayName = std::string()) override {
    // Spotify is controlled through Spotify Connect, a URI can't be started
    // from here -> treat play() as resume
    (void)uri;
    (void)displayName;
    resume();
  }

  bool next() override { return command({{"endpoint", "skip_next"}}); }
  bool previous() override { return command({{"endpoint", "skip_prev"}}); }

  bool seek(uint32_t positionMs) override {
    return command({{"endpoint", "seek_to"},
                    {"relative", "beginning"},
                    {"value", positionMs}});
  }

  bool setShuffle(bool on) override {
    if (!capabilities().shuffle)
      return false;
    return command({{"endpoint", "set_shuffling_context"}, {"value", on}});
  }

  bool setRepeat(RepeatMode mode) override {
    if (!capabilities().repeat)
      return false;
    return command({{"endpoint", "set_options"},
                    {"repeating_context", mode != RepeatMode::Off},
                    {"repeating_track", mode == RepeatMode::One}});
  }

  void setVolume(uint8_t percent) override {
    percent = std::min<uint8_t>(percent, 100);
    auto h = getHandler();
    if (!h) {
      StreamBase::setVolume(percent);
      return;
    }
    volumePercent_.store(percent);
    // Spotify volume is 0..65535 (logarithmic in the sink, like remote)
    h->localSetVolume((uint16_t)(((uint32_t)percent * 65535) / 100));
  }

  std::vector<TrackInfo> getQueue(size_t maxItems = 20) override {
    std::vector<TrackInfo> out;
    auto h = getHandler();
    if (!h)
      return out;
    for (auto& e : h->getQueueSnapshot(maxItems)) {
      TrackInfo t;
      t.id = e.uri;
      t.title = e.title;
      t.artist = e.artist;
      t.album = e.album;
      t.imageUrl = e.imageUrl;
      t.durationMs = e.durationMs;
      out.push_back(std::move(t));
    }
    return out;
  }

  bool playQueueItem(size_t index) override {
    auto h = getHandler();
    if (!h)
      return false;
    auto q = h->getQueueSnapshot(index + 1);
    if (index >= q.size())
      return false;
    return command({{"endpoint", "skip_next"}, {"track", {{"uri", q[index].uri}}}});
  }

  NowPlaying nowPlaying() override {
    NowPlaying np;
    np.source = sourceName();
    np.volume = getVolume();
    np.caps = capabilities();
    auto h = getHandler();
    if (!h || !h->is_active) {
      np.state = PlaybackState::Stopped;
      return np;
    }
    np.state = playbackState();
    np.shuffle = h->device.player_state.options.shuffling_context;
    if (h->device.player_state.options.repeating_track)
      np.repeat = RepeatMode::One;
    else if (h->device.player_state.options.repeating_context)
      np.repeat = RepeatMode::All;
    auto track = getCurrentTrack();
    if (!track)
      return np;
    np.track.id = track->trackInfo.trackId;
    np.track.title = track->trackInfo.name;
    np.track.artist = track->trackInfo.artist;
    np.track.album = track->trackInfo.album;
    np.track.imageUrl = track->trackInfo.imageUrl;
    np.track.durationMs = track->trackInfo.duration;
    switch (track->audioFormat) {
      case 0:
        np.quality = "Ogg Vorbis - 96 kbps";
        break;
      case 1:
        np.quality = "Ogg Vorbis - 160 kbps";
        break;
      case 2:
        np.quality = "Ogg Vorbis - 320 kbps";
        break;
      default:
        np.quality = "Unknown";
        break;
    }
    if (track->trackMetrics && track->trackMetrics->currentInterval) {
      uint64_t pos = track->trackMetrics->getPosition(
          np.state == PlaybackState::Paused);
      if (np.track.durationMs && pos > np.track.durationMs)
        pos = np.track.durationMs;
      np.positionMs = (uint32_t)pos;
    }
    return np;
  }

 private:
  std::mutex handlerMu_;
  std::mutex trackMu_;
  std::shared_ptr<spotify::QueuedTrack> currentTrack_;

 public:
  std::shared_ptr<spotify::DeviceStateHandler> getHandler() {
    std::scoped_lock l(handlerMu_);
    return handler;
  }

 private:
  void setHandler(std::shared_ptr<spotify::DeviceStateHandler> h) {
    std::shared_ptr<spotify::DeviceStateHandler> old;
    {
      std::scoped_lock l(handlerMu_);
      old = std::move(handler);
      handler = std::move(h);
    }
    // 'old' is released outside the lock
  }
  void setCurrentTrack(std::shared_ptr<spotify::QueuedTrack> t) {
    std::scoped_lock l(trackMu_);
    currentTrack_ = std::move(t);
  }
  std::shared_ptr<spotify::QueuedTrack> getCurrentTrack() {
    std::scoped_lock l(trackMu_);
    return currentTrack_;
  }
  bool command(const nlohmann::json& cmd) {
    auto h = getHandler();
    if (!h)
      return false;
    return h->localCommand(cmd);
  }
};
