#pragma once
// ============================================================================
//  SDFileStream — plays audio files from the SD card (or any mounted VFS).
//
//  The VS10xx decoder does the decoding, this stream only reads the files and
//  feeds them gap-less into the sink.  Everything StreamBase defines works:
//
//    transport : pause / resume / next / previous / seek / seekPercent
//    modes     : shuffle, repeat (off / all / one)
//    queue     : getQueue() = upcoming files, playQueueItem()
//    state     : nowPlaying() with title / artist / album from the file tags,
//                duration, position, quality ("FLAC - 16-Bit / 44.1 kHz")
//
//  Playlist: playFile(path) plays the file and uses all playable files of the
//  same folder (sorted by name) as the playlist; playFolder(dir) starts at the
//  first file.  play("file:///sdcard/Music/x.mp3") does the same as
//  playFile(), so the generic StreamBase::play() works too.
//
//  Seeking re-sends the header the decoder needs (FLAC STREAMINFO, Ogg header
//  pages, WAV header) and continues at the computed byte offset (Xing TOC for
//  VBR MP3).  MP4/M4A, WMA and MIDI play front to back only.
// ============================================================================
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "AudioFileInfo.h"
#include "StreamBase.h"

class SDFileStream : public StreamBase {
 public:
  explicit SDFileStream(std::shared_ptr<AudioControl> audio,
                        std::string root = "/sdcard");
  ~SDFileStream() override;

  // ---- playlist --------------------------------------------------------------
  /** Play `path`; the playlist becomes all playable files of its folder. */
  bool playFile(const std::string& path, uint32_t startMs = 0);
  /** Play all playable files of `dir`, starting with entry `index`. */
  bool playFolder(const std::string& dir, size_t index = 0);
  /** Play an M3U playlist (.m3u / .m3u8), starting with entry `index`. */
  bool playPlaylist(const std::string& m3uPath, size_t index = 0);

  // ---- M3U playlists ---------------------------------------------------------
  //  #EXTM3U / #EXTINF lines are optional; entries may be absolute
  //  ("/sdcard/Music/a.mp3") or relative to the playlist file
  //  ("../Music/a.mp3", "a.mp3").  writePlaylist() stores relative paths so
  //  the card also works on a PC.
  static bool isPlaylistFile(const std::string& name);
  /** Absolute paths of the entries (missing files are kept, see exists). */
  static std::vector<std::string> readPlaylist(const std::string& m3uPath);
  static bool writePlaylist(const std::string& m3uPath,
                            const std::vector<std::string>& absPaths);
  /** "a/b/../c/./d" -> "a/c/d" */
  static std::string normalizePath(const std::string& path);
  /** path of `target` relative to directory `fromDir` (both absolute) */
  static std::string relativePath(const std::string& fromDir,
                                  const std::string& target);

  /** Play an explicit list of files. */
  bool setPlaylist(std::vector<std::string> paths, size_t index = 0,
                   uint32_t startMs = 0);

  std::vector<std::string> playlist();
  /** Path of the audible file ("" when nothing is loaded). */
  std::string currentPath();
  const std::string& root() const { return root_; }

  /** Sorted list (full paths) of the playable files in `dir`. */
  static std::vector<std::string> listAudioFiles(const std::string& dir);
  static bool isAudioFile(const std::string& name) {
    return sdfile::isPlayableFile(name);
  }

  // ---- StreamBase control API ------------------------------------------------
  void play(const std::string& uri,
            const std::string& displayName = std::string()) override;
  void stop() override;

  const char* sourceName() const override { return "SD"; }
  bool isActive() override { return isRunning_.load(); }
  Capabilities capabilities() override;

  void pause() override;
  void resume() override;
  bool next() override;
  bool previous() override;
  bool seek(uint32_t positionMs) override;
  bool setShuffle(bool on) override;
  bool setRepeat(RepeatMode mode) override;
  std::vector<TrackInfo> getQueue(size_t maxItems = 20) override;
  bool playQueueItem(size_t index) override;
  NowPlaying nowPlaying() override;

 protected:
  void runTask() override;

 private:
  struct Request {
    size_t plIdx;      // index into playlist_
    uint32_t startMs;  // start position
  };
  struct Fed {  // a file handed to the sink
    size_t tid = 0;
    size_t plIdx = 0;
    std::string path;
    std::shared_ptr<sdfile::AudioFileInfo> info;
    uint32_t startMs = 0;
  };

  // all members below are guarded by plMu_
  std::mutex plMu_;
  std::vector<std::string> playlist_;
  std::vector<size_t> order_;  // play order -> playlist index
  std::vector<size_t> posOf_;  // playlist index -> play order position
  bool shuffle_ = false;
  RepeatMode repeat_ = RepeatMode::Off;
  std::optional<Request> request_;
  std::deque<Fed> fed_;          // in the sink, not started yet
  std::optional<Fed> playing_;   // audible track
  size_t lastPlIdx_ = 0;         // for resume() after the end / stop()
  uint32_t posBaseMs_ = 0;       // position clock
  int64_t posStartUs_ = 0;
  bool clockRunning_ = false;

  bool atEnd_ = false;           // the playlist played to its end

  std::atomic<bool> switch_{false};     // request_ pending
  std::atomic<bool> readerIdle_{true};  // nothing left to read
  // last stream of the playlist: let the sink play it out, then stop it
  std::atomic<size_t> drainTid_{0};
  std::atomic<bool> drainCmdPending_{false};
  std::string root_;

  // helpers (call with plMu_ held)
  void rebuildOrderLocked(size_t keepFirstPlIdx);
  std::optional<size_t> upcomingLocked(size_t i) const;
  size_t currentPlIdxLocked() const;
  uint32_t positionLocked() const;
  void requestLocked(size_t plIdx, uint32_t startMs);

  void ensureTask();
  void tryDrain();
  void onSinkState(uint8_t state);
  static int64_t nowUs();
};
