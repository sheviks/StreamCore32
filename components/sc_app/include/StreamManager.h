#pragma once
// ============================================================================
//  StreamManager — owns the streams and decides which one is "active".
//
//  Only one source plays at a time.  Selecting a source (Spotify/Qobuz
//  connect, a radio station, a file on the SD card, a DLNA control point) stops the previous one.
//  Everything that controls playback (touch UI, web UI) goes through
//  active(), i.e. through the common StreamBase API.
// ============================================================================
#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "AudioControl.h"
#include "StreamBase.h"
#include "sdkconfig.h"
#if CONFIG_SC32_QOBUZ
#include "QobuzStream.h"
#endif
#if CONFIG_SC32_SDFILE
#include "SDFileStream.h"
#endif
#if CONFIG_SC32_SPOTIFY
#include "SpotifyStream.h"
#endif
#if CONFIG_SC32_DLNA
#include "DlnaStream.h"
#endif
#if CONFIG_SC32_WEBSTREAM
#include "WebStream.h"
using RadioStation = WebStream::Station;
#else
struct RadioStation {
  std::string name, url, imageUrl, info;
};
#endif

class StreamManager {
 public:
  enum class Service : uint8_t { None = 0, Spotify, Qobuz, Radio, SD, Dlna };

  explicit StreamManager(std::shared_ptr<AudioControl> audio)
      : audio_(std::move(audio)),
        feed_(std::make_shared<AudioControl::FeedControl>(audio_)) {
    lastVolume_.store((uint8_t)std::min<size_t>(audio_->volume.load(), 100));
  }

  /** Feed control for things that are not a stream (volume, tone). */
  std::shared_ptr<AudioControl::FeedControl> feed() const { return feed_; }

  std::shared_ptr<AudioControl> audio() const { return audio_; }

  // streams are created by the app and registered here (only the sources
  // enabled in menuconfig exist)
#if CONFIG_SC32_SPOTIFY
  std::shared_ptr<SpotifyStream> spotify;
#endif
#if CONFIG_SC32_QOBUZ
  std::shared_ptr<QobuzStream> qobuz;
#endif
#if CONFIG_SC32_WEBSTREAM
  std::shared_ptr<WebStream> radio;
#endif
#if CONFIG_SC32_SDFILE
  std::shared_ptr<SDFileStream> sd;
#endif
#if CONFIG_SC32_DLNA
  std::shared_ptr<DlnaStream> dlna;
#endif

  /** Called with every "playback"/"queue" JSON of the ACTIVE stream. */
  std::function<void(const std::string& json)> onUiMessage;

  /** Hook a stream's UI messages up (call once per stream). */
  void attach(StreamBase* s, Service svc) {
    if (!s)
      return;
    s->onUiMessage_ = [this, svc](const std::string& msg) {
      if (current() == svc && onUiMessage)
        onUiMessage(msg);
    };
  }

  // Lock free on purpose: these are called from stream tasks (UI messages)
  // while activate() may be waiting for exactly that task to finish.
  Service current() { return current_.load(); }

  StreamBase* active() { return streamOf(current_.load()); }

  static const char* name(Service s) {
    switch (s) {
      case Service::Spotify:
        return "Spotify";
      case Service::Qobuz:
        return "Qobuz";
      case Service::Radio:
        return "Radio";
      case Service::SD:
        return "SD";
      case Service::Dlna:
        return "DLNA";
      default:
        return "None";
    }
  }

  /** Make `s` the active source; stops the previous one. */
  void activate(Service s) {
    // serialises source switches only; stream tasks never take this lock
    std::lock_guard<std::recursive_mutex> lk(mu_);
    Service old = current_.exchange(s);  // first: callbacks fired by stop()
    if (old == s)                        // already see the new state
      return;
    stopService(old);
    // local sources start with the volume the user had last (every stream
    // keeps its own volume value; Spotify / Qobuz get theirs from the app)
    if (s == Service::Radio || s == Service::SD || s == Service::Dlna)
      if (StreamBase* st = streamOf(s))
        st->setVolume(lastVolume_.load());
    SC32_LOG(info, "source: %s -> %s", name(old), name(s));
  }

  /** A source went away by itself (e.g. Spotify disconnected). */
  void deactivate(Service s) {
    Service expected = s;
    current_.compare_exchange_strong(expected, Service::None);
  }

  /** Stop whatever plays and select nothing. */
  void stopAll() {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    Service old = current_.exchange(Service::None);
    stopService(old);
  }

  // ---- convenience ---------------------------------------------------------
#if CONFIG_SC32_WEBSTREAM
  bool playStation(const std::vector<RadioStation>& stations,
                   size_t index) {
    if (!radio || index >= stations.size())
      return false;
    activate(Service::Radio);
    radio->setStations(stations);
    radio->play(stations[index].url, stations[index].name);
    return true;
  }
  bool playRadioUrl(const std::string& url, const std::string& name,
                    const std::vector<RadioStation>& stations) {
    if (!radio || url.empty())
      return false;
    activate(Service::Radio);
    radio->setStations(stations);
    radio->play(url, name);
    return true;
  }
#else
  bool playStation(const std::vector<RadioStation>&, size_t) { return false; }
  bool playRadioUrl(const std::string&, const std::string&,
                    const std::vector<RadioStation>&) {
    return false;
  }
#endif
#if CONFIG_SC32_SDFILE
  bool playFile(const std::string& path) {
    if (!sd)
      return false;
    activate(Service::SD);
    return sd->playFile(path);
  }
  bool playFolder(const std::string& dir) {
    if (!sd)
      return false;
    activate(Service::SD);
    return sd->playFolder(dir);
  }
  bool playPlaylist(const std::string& m3u, size_t index = 0) {
    if (!sd)
      return false;
    activate(Service::SD);
    return sd->playPlaylist(m3u, index);
  }
  /** Stop the SD player when it plays `path` or a file below it (before the
   *  file manager deletes / moves / overwrites it). */
  void releaseSdPath(const std::string& path) {
    if (!sd || current_.load() != Service::SD)
      return;
    const std::string cur = sd->currentPath();
    if (cur.empty())
      return;
    if (cur == path || cur.compare(0, path.size() + 1, path + "/") == 0) {
      SC32_LOG(info, "SD: stopping playback, %s is being changed", path.c_str());
      stopAll();
    }
  }

#else
  bool playFile(const std::string&) { return false; }
  bool playFolder(const std::string&) { return false; }
  bool playPlaylist(const std::string&, size_t = 0) { return false; }
  void releaseSdPath(const std::string&) {}
#endif

  /** Volume 0..100 for the active stream (or the sink if none). */
  void setVolume(uint8_t v) {
    v = std::min<uint8_t>(v, 100);
    lastVolume_.store(v);
    if (auto* s = active()) {
      s->setVolume(v);
      return;
    }
    // (feedCommand queues a lambda that keeps a pointer to the FeedControl:
    //  it must outlive the call, hence the member)
    feed_->feedCommand(AudioControl::VOLUME_LINEAR, (uint32_t)v,
                       std::optional<uint32_t>(100));
  }
  uint8_t volume() {
    if (auto* s = active()) {
      uint8_t v = s->getVolume();
      lastVolume_.store(v);  // e.g. changed in the Spotify app
      return v;
    }
    return lastVolume_.load();
  }

  bool isPlaying() {
    auto* s = active();
    return s && s->playbackState() == StreamBase::PlaybackState::Playing;
  }

 private:
  std::recursive_mutex mu_;
  std::atomic<Service> current_{Service::None};
  std::atomic<uint8_t> lastVolume_{50};
  std::shared_ptr<AudioControl> audio_;
  std::shared_ptr<AudioControl::FeedControl> feed_;

  StreamBase* streamOf(Service s) {
    switch (s) {
#if CONFIG_SC32_SPOTIFY
      case Service::Spotify:
        return spotify.get();
#endif
#if CONFIG_SC32_QOBUZ
      case Service::Qobuz:
        return qobuz.get();
#endif
#if CONFIG_SC32_WEBSTREAM
      case Service::Radio:
        return radio.get();
#endif
#if CONFIG_SC32_SDFILE
      case Service::SD:
        return sd.get();
#endif
#if CONFIG_SC32_DLNA
      case Service::Dlna:
        return dlna.get();
#endif
      default:
        return nullptr;
    }
  }

  void stopService(Service s) {
    if (StreamBase* st = streamOf(s))
      st->stop();
  }
};
