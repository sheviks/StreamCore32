#pragma once
// ============================================================================
//  DlnaStream — the playing side of the DLNA renderer (a StreamBase).
//
//  A control point hands in a URL (SetAVTransportURI) and maybe the next one
//  (SetNextAVTransportURI); this stream fetches them over HTTP(S) and feeds
//  the VS1053, which decodes MP3, AAC, FLAC, Ogg Vorbis, WAV, WMA, MIDI.
//  audio/L16 (raw PCM, what many control points transcode to) gets a WAV
//  header and its bytes swapped.
//
//  • files from a server that supports Range requests are probed like the
//    files on the SD card (AudioFileInfo): exact duration, quality, and
//    seeking (decoder header re-sent, then "Range: bytes=<offset>-")
//  • the next track is opened while the current one plays out: gap-less
//  • live streams (no length) play front to back, reconnect if dropped
//  • the device UI / web UI control it through StreamBase like any source
//  • when another source takes over, the DLNA side shows "paused"; Play in
//    the control point takes the device back and continues there
//
//  Thread safe (control points, web UI, display and the sink callback call in).
// ============================================================================
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "AudioFileInfo.h"
#include "DlnaRenderer.h"
#include "StreamBase.h"

class DlnaStream : public StreamBase {
 public:
  explicit DlnaStream(std::shared_ptr<AudioControl> audio);
  ~DlnaStream() override;

  /** The control point wants to play: make DLNA the active source. */
  void onActivate(std::function<void()> cb) { onActivate_ = std::move(cb); }
  /** Renderer to wake when something changed (events to the control point). */
  void setRenderer(dlna::Renderer* r) { renderer_ = r; }

  /** The protocol side's view of this stream. */
  dlna::Player& player() { return adapter_; }

  // ---- StreamBase ------------------------------------------------------------
  const char* sourceName() const override { return "DLNA"; }
  bool isActive() override { return isRunning_.load(); }
  Capabilities capabilities() override;
  void play(const std::string& uri,
            const std::string& displayName = std::string()) override;
  /** Another source takes over (StreamManager): DLNA shows "paused". */
  void stop() override;
  void pause() override;
  void resume() override;
  bool next() override;
  bool previous() override;
  bool seek(uint32_t positionMs) override;
  void setVolume(uint8_t percent) override;
  std::vector<TrackInfo> getQueue(size_t maxItems = 20) override;
  bool playQueueItem(size_t index) override;
  NowPlaying nowPlaying() override;

 protected:
  void runTask() override;

 private:
  using TrackPtr = std::shared_ptr<const dlna::Track>;

  // dlna::Player (names clash with StreamBase: a small adapter)
  class Adapter : public dlna::Player {
   public:
    explicit Adapter(DlnaStream& s) : s_(s) {}
    int setUri(TrackPtr t) override { return s_.dSetUri(std::move(t)); }
    int setNextUri(TrackPtr t) override { return s_.dSetNextUri(std::move(t)); }
    int play() override { return s_.dPlay(); }
    int pause() override { return s_.dPause(); }
    int stop() override { return s_.dStop(); }
    int seek(uint32_t ms) override { return s_.dSeek(ms); }
    int next() override { return s_.dNext(); }
    int previous() override { return s_.dPrevious(); }
    void setVolume(uint8_t v) override { s_.setVolume(v); }
    void setMute(bool on) override { s_.dSetMute(on); }
    dlna::PlayerStatus status() override { return s_.dStatus(); }

   private:
    DlnaStream& s_;
  };

  struct Fed {  // a track handed to the sink
    size_t tid = 0;
    TrackPtr track;
    std::shared_ptr<sdfile::AudioFileInfo> info;  // null: not probed
    uint32_t startMs = 0;
    bool seekable = false;
    std::string quality;
  };

  int dSetUri(TrackPtr t);
  int dSetNextUri(TrackPtr t);
  int dPlay();
  int dPause();
  int dStop();
  int dSeek(uint32_t ms);
  int dNext();
  int dPrevious();
  void dSetMute(bool on);
  dlna::PlayerStatus dStatus();

  // helpers (call with mu2_ held)
  void requestLocked(uint32_t startMs);
  uint32_t positionLocked() const;
  uint32_t durationLocked() const;
  bool seekableLocked() const;
  void startPlayback(uint32_t startMs);  // activate + request
  void halt();  // stop reading, keep the tracks

  void ensureTask();
  void tryDrain();
  void onSinkState(uint8_t s);
  void changed();  // publish to the UIs + wake the renderer
  static int64_t nowUs();

  Adapter adapter_{*this};
  dlna::Renderer* renderer_ = nullptr;
  std::function<void()> onActivate_;

  std::mutex mu2_;
  TrackPtr cur_, next_;
  std::optional<uint32_t> request_;  // start cur_ at ms
  std::deque<Fed> fed_;              // in the sink, not audible yet
  std::optional<Fed> playing_;       // audible
  TrackPtr chained_;                 // next_ already opened by the reader
  uint32_t posBaseMs_ = 0;
  int64_t posStartUs_ = 0;
  bool clockRunning_ = false;
  bool detached_ = false;   // another source took over (shown as paused)
  uint32_t detachedMs_ = 0;
  uint32_t seekPendingMs_ = 0;  // Seek while stopped: start there
  bool error_ = false;
  bool muted_ = false;

  std::atomic<bool> switch_{false};
  std::atomic<bool> readerIdle_{true};
  std::atomic<size_t> drainTid_{0};
  std::atomic<bool> drainCmdPending_{false};
};
