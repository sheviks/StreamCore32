#pragma once
#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <vector>
#include "HTTPClient.h"
#include "StreamBase.h"
#include "URLParser.h"

#include "Heartbeat.h"
#include "TimeSync.h"

#include "QobuzQueue.h"
#include "QobuzTrack.h"

#include "Logger.h"

class QobuzPlayer : public StreamBase {
 public:
  using OnUiMessage = std::function<void(const std::string&)>;

  using OnWsMessage = std::function<void(_qconnect_QConnectMessage*, size_t)>;

  using OnQobuzGet = std::function<std::unique_ptr<bell::HTTPClient::Response>(
      const std::string& object,  //session, user, file
      const std::string& action,  // start, url...
      const std::vector<std::pair<std::string, std::string>>& params,
      bool sign)>;

  using OnQobuzPost = std::function<std::unique_ptr<bell::HTTPClient::Response>(
      const std::string& object, const std::string& action,
      const std::string& body,
      const std::vector<std::pair<std::string, std::string>>& params,
      bool sign)>;

  QobuzPlayer(std::shared_ptr<AudioControl> audio,
              std::shared_ptr<qobuz::QobuzQueue> queue);
  ~QobuzPlayer() {
    stopTask();
    while (hb_)
      BELL_SLEEP_MS(50);
    std::scoped_lock lock(isRunningMutex_);
    pb_release(qconnect_QueueRendererState_fields, &player_state);
  }
  void runTask() override;

  void onGet(OnQobuzGet f) { on_qobuz_get_ = std::move(f); }
  void onPost(OnQobuzPost f) { on_qobuz_post_ = std::move(f); }
  void onWsMessage(OnWsMessage f) { on_ws_msg_ = std::move(f); }
  bool requestSkipTo(size_t offset) {
    auto t = getPlayingTrack();
    if (!t)
      return false;
    t->skipTo_.store(offset);
    t->wantSkip_.store(true);
    return true;
  }
  void setEOF(bool v = true) {
    eofMode_.store(v);
  }  // if true, do not auto-resume after EOF
  bool eofSeen() const { return eofSeen_.load(); }
  std::shared_ptr<qobuz::QobuzQueueTrack> getCurrentTrack() const {
    return current_track_buffering;
  }
  void setRepeatOne(bool v) { repeatOne_.store(v); }
  void stopTask() {
    stopTrack();
    isRunning_.store(false);
  }
  void stopTrack() {
    if (isRunning_.load()) {
      wantStop_.store(true);
    }
  }
  /**
   * Stop feeding the current track and drop all audio of this feed that is
   * still queued in the sink, so the next track (see QobuzQueue::setIndex)
   * starts immediately. The flush happens on the player task, after it left
   * the feeding loop, so no stale bytes of the old track sneak in.
   */
  void skipTrackNow() {
    if (isRunning_.load()) {
      flushOnStop_.store(true);
      wantStop_.store(true);
    }
  }

  // ---- local control (device / UI) ----
  /** Pause like a remote SET_STATE(PAUSED); state is reported to Qobuz. */
  void pausePlayback();
  /** Resume like a remote SET_STATE(PLAYING); state is reported to Qobuz. */
  void resumePlayback();
  bool isPlayingState() const {
    return player_state.playingState ==
           qconnect_PlayingState_PLAYING_STATE_PLAYING;
  }
  bool isPausedState() const {
    return player_state.playingState ==
           qconnect_PlayingState_PLAYING_STATE_PAUSED;
  }
  /** Position of the audible track in ms (pause aware). */
  uint32_t positionMs();
  /** Track that is audible right now (may differ from getCurrentTrack(),
   *  which is the one being buffered). */
  std::shared_ptr<qobuz::QobuzQueueTrack> getPlayingTrack() {
    std::scoped_lock lock(trackMutex_);
    return current_track_playing;
  }
  static std::string qualityString(const qobuz::QobuzQueueTrack& t);
  /** Sink state notifications (1 start, 2 playing, 3 paused, 7 ended). */
  std::function<void(uint8_t)> onSinkState = nullptr;

  size_t currentTrackValueMs() const {
    if (!current_track_playing)
      return 0;
    if (player_state.currentPosition.has_timestamp &&
        player_state.currentPosition.timestamp != 0) {
      return player_state.currentPosition.value +
             (timesync::now_ms() - player_state.currentPosition.timestamp);
    }
    return 0;
  }

  void setTracks() {
    auto t = queue_->consumeTrack(nullptr, player_state.nextQueueItemId);
    std::scoped_lock lock(trackMutex_);
    current_track_playing = t;
  }

  void sendPlayerState();
  std::string user_id_{""};
  qconnect_QueueRendererState player_state =
      qconnect_QueueRendererState_init_default;
  // onUiMessage_ is inherited from StreamBase

 private:
  std::vector<uint8_t> header_;  // optional cached header
  std::shared_ptr<qobuz::QobuzQueue> queue_{nullptr};
  std::shared_ptr<qobuz::QobuzQueueTrack> current_track_buffering = nullptr;
  std::shared_ptr<qobuz::QobuzQueueTrack> current_track_playing = nullptr;

  std::unique_ptr<Heartbeat> hb_;

  size_t totalSize_{0};   // playable size = Content-Length - baseOffset_
  size_t baseOffset_{0};  // byte position of 'fLaC' in the CDN file
  size_t startedPlayingAt = 0;

  std::atomic<bool> eofMode_{true};
  std::atomic<bool> eofSeen_{false};
  std::atomic<bool> wantRestart_{false};
  std::atomic<bool> repeatOne_{false};
  std::atomic<bool> flushOnStop_{false};
  std::mutex isRunningMutex_;
  std::mutex trackMutex_;  // guards current_track_playing
  /**
   * @brief Get stream information from a URL (HTTP(S) or HTTPS).
   * @param url URL to query
   * @param[out] length Total length of the stream in bytes
   * @param[out] offset Offset in bytes to start playing from
   * @param format Audio format to probe for (if supported)
   * @param buffer Optional buffer to store FLAC metadata in
   * @return true if successful, false otherwise
   *
   * Queries the URL and returns the total length of the stream in bytes, as well as the offset in bytes to start playing from.
   * If the format is FLAC, it will probe the stream for FLAC metadata and store reduced metadata in the provided buffer.
   * If the format is not supported, it will not probe the stream and return false.
   */
  bool getStreamInfo(std::string url, size_t& length, size_t& offset,
                     qobuz::AudioFormat format, uint8_t* buffer);
  /**
   * @brief Probes a SocketStream for FLAC metadata.
   * @param s SocketStream to read from
   * @param n Maximum number of bytes to read
   * @param dst Optional destination for reduced FLAC metadata
   * @param offset Offset in bytes to store the parsed metadata
   * @return true if successful, false otherwise
   *
   * Probes a SocketStream for FLAC metadata.
   * If not found, it looks for the first frame header (0xFF f8 fb)
   * and parses the FLAC metadata from there.
   * A reduced metadata buffer (42 bytes) is provided in dst.
   */
  bool probeFlac(bell::HTTPClient::Response* s, size_t n, uint8_t* dst,
                 size_t& offset);

  bool probeHttpHeaders(bell::HTTPClient::Headers& headers, size_t& totalLength,
                        qobuz::AudioFormat fmt);

  OnWsMessage on_ws_msg_;
  OnQobuzGet on_qobuz_get_;
  OnQobuzPost on_qobuz_post_;
};
