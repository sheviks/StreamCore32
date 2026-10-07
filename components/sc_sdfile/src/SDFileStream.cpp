#include "SDFileStream.h"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstring>
#include <random>

#include "Logger.h"

#ifdef ESP_PLATFORM
#include "EspRandomEngine.h"
#include "esp_timer.h"
#else
#include <chrono>
#endif

namespace {

constexpr size_t kChunk = 4096;  // bytes per SD read / sink feed

std::string parentDir(const std::string& path) {
  auto s = path.find_last_of('/');
  if (s == std::string::npos)
    return ".";
  if (s == 0)
    return "/";
  return path.substr(0, s);
}

std::string baseName(const std::string& path) {
  auto s = path.find_last_of('/');
  return s == std::string::npos ? path : path.substr(s + 1);
}

bool isDirectory(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

}  // namespace

// ============================================================================
//  construction
// ============================================================================
SDFileStream::SDFileStream(std::shared_ptr<AudioControl> audio,
                           std::string root)
    // stack in PSRAM: the reader only touches the SD card (no flash access),
    // internal RAM is kept for WiFi / TLS
    : StreamBase("SDFile", audio, 1024 * 8, 2, 1, true),
      root_(std::move(root)) {
  // own sink-state handler: 0 = a stream starts, 1/2 = playing,
  // 3 = paused, 7 = a stream ended
  feed_->state_callback = [this](uint8_t s) { onSinkState(s); };
}

SDFileStream::~SDFileStream() {
  stop();
}

int64_t SDFileStream::nowUs() {
#ifdef ESP_PLATFORM
  return esp_timer_get_time();
#else
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
#endif
}

// ============================================================================
//  playlist
// ============================================================================
std::vector<std::string> SDFileStream::listAudioFiles(const std::string& dir) {
  std::vector<std::string> out;
  DIR* d = opendir(dir.c_str());
  if (!d)
    return out;
  struct dirent* e;
  while ((e = readdir(d)) != nullptr) {
    if (e->d_name[0] == '.')
      continue;
    std::string full = dir;
    if (full.empty() || full.back() != '/')
      full += '/';
    full += e->d_name;
    bool isDir = e->d_type == DT_DIR;
    if (e->d_type == DT_UNKNOWN)
      isDir = isDirectory(full);
    if (!isDir && sdfile::isPlayableFile(e->d_name))
      out.push_back(std::move(full));
  }
  closedir(d);
  std::sort(out.begin(), out.end(), [](const std::string& a,
                                       const std::string& b) {
    return strcasecmp(a.c_str(), b.c_str()) < 0;
  });
  return out;
}

bool SDFileStream::playFile(const std::string& path, uint32_t startMs) {
  if (isPlaylistFile(path))
    return playPlaylist(path, 0);
  auto files = listAudioFiles(parentDir(path));
  size_t idx = 0;
  auto it = std::find(files.begin(), files.end(), path);
  if (it == files.end()) {
    files.assign(1, path);  // not a "playable" name: let the decoder try
  } else {
    idx = size_t(it - files.begin());
  }
  return setPlaylist(std::move(files), idx, startMs);
}

// ============================================================================
//  M3U playlists
// ============================================================================
bool SDFileStream::isPlaylistFile(const std::string& name) {
  auto dot = name.find_last_of('.');
  if (dot == std::string::npos)
    return false;
  std::string e = toLower(name.substr(dot + 1));
  return e == "m3u" || e == "m3u8";
}

std::string SDFileStream::normalizePath(const std::string& path) {
  std::vector<std::string> parts;
  size_t i = 0;
  const bool abs = !path.empty() && path[0] == '/';
  while (i <= path.size()) {
    size_t j = path.find('/', i);
    if (j == std::string::npos)
      j = path.size();
    std::string p = path.substr(i, j - i);
    if (p == "..") {
      if (!parts.empty() && parts.back() != "..")
        parts.pop_back();
      else if (!abs)
        parts.push_back(p);
    } else if (!p.empty() && p != ".") {
      parts.push_back(p);
    }
    i = j + 1;
  }
  std::string out = abs ? "/" : "";
  for (size_t k = 0; k < parts.size(); k++) {
    if (k)
      out += '/';
    out += parts[k];
  }
  return out.empty() ? "." : out;
}

std::string SDFileStream::relativePath(const std::string& fromDir,
                                       const std::string& target) {
  auto split = [](const std::string& p) {
    std::vector<std::string> v;
    size_t i = 0;
    while (i < p.size()) {
      size_t j = p.find('/', i);
      if (j == std::string::npos)
        j = p.size();
      if (j > i)
        v.push_back(p.substr(i, j - i));
      i = j + 1;
    }
    return v;
  };
  auto a = split(normalizePath(fromDir)), b = split(normalizePath(target));
  size_t k = 0;
  while (k < a.size() && k < b.size() && a[k] == b[k])
    k++;
  std::string out;
  for (size_t i = k; i < a.size(); i++)
    out += "../";
  for (size_t i = k; i < b.size(); i++) {
    out += b[i];
    if (i + 1 < b.size())
      out += '/';
  }
  return out;
}

std::vector<std::string> SDFileStream::readPlaylist(const std::string& m3uPath) {
  std::vector<std::string> out;
  FILE* f = fopen(m3uPath.c_str(), "rb");
  if (!f)
    return out;
  const std::string dir = parentDir(m3uPath);
  char line[512];
  bool first = true;
  while (fgets(line, sizeof line, f)) {
    std::string s(line);
    if (first && s.size() >= 3 && (uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB &&
        (uint8_t)s[2] == 0xBF)
      s.erase(0, 3);  // UTF-8 BOM
    first = false;
    trim(s);
    if (s.empty() || s[0] == '#')
      continue;
    for (auto& c : s)
      if (c == '\\')
        c = '/';  // playlists written on Windows
    if (startsWith(s, "file://"))
      s.erase(0, 7);
    if (s.find("://") != std::string::npos)
      continue;  // web URLs are not supported here
    out.push_back(normalizePath(s[0] == '/' ? s : dir + "/" + s));
  }
  fclose(f);
  return out;
}

bool SDFileStream::writePlaylist(const std::string& m3uPath,
                                 const std::vector<std::string>& absPaths) {
  const std::string tmp = m3uPath + ".tmp";
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f)
    return false;
  const std::string dir = parentDir(m3uPath);
  bool ok = fputs("#EXTM3U\n", f) >= 0;
  for (auto& p : absPaths) {
    if (!ok)
      break;
    std::string line = "#EXTINF:-1," + sdfile::titleFromFileName(p) + "\n" +
                       relativePath(dir, p) + "\n";
    ok = fputs(line.c_str(), f) >= 0;
  }
  ok = (fclose(f) == 0) && ok;
  if (!ok) {
    remove(tmp.c_str());
    return false;
  }
  remove(m3uPath.c_str());
  return rename(tmp.c_str(), m3uPath.c_str()) == 0;
}

bool SDFileStream::playPlaylist(const std::string& m3uPath, size_t index) {
  auto files = readPlaylist(m3uPath);
  // drop entries that do not exist (any more)
  files.erase(std::remove_if(files.begin(), files.end(),
                             [](const std::string& p) {
                               struct stat st;
                               return stat(p.c_str(), &st) != 0 || S_ISDIR(st.st_mode);
                             }),
              files.end());
  if (files.empty()) {
    reportError("SD: playlist is empty: " + m3uPath);
    return false;
  }
  return setPlaylist(std::move(files), index, 0);
}

bool SDFileStream::playFolder(const std::string& dir, size_t index) {
  auto files = listAudioFiles(dir);
  if (files.empty()) {
    reportError("SD: no playable files in " + dir);
    return false;
  }
  return setPlaylist(std::move(files), index, 0);
}

bool SDFileStream::setPlaylist(std::vector<std::string> paths, size_t index,
                               uint32_t startMs) {
  if (paths.empty())
    return false;
  {
    std::lock_guard<std::mutex> lk(plMu_);
    playlist_ = std::move(paths);
    if (index >= playlist_.size())
      index = 0;
    rebuildOrderLocked(index);
    requestLocked(index, startMs);
    clockRunning_ = false;
    posBaseMs_ = startMs;
  }
  setPlaybackState(PlaybackState::Buffering);
  ensureTask();
  return true;
}

std::vector<std::string> SDFileStream::playlist() {
  std::lock_guard<std::mutex> lk(plMu_);
  return playlist_;
}

std::string SDFileStream::currentPath() {
  std::lock_guard<std::mutex> lk(plMu_);
  if (playing_)
    return playing_->path;
  if (!fed_.empty())
    return fed_.front().path;
  return "";
}

void SDFileStream::rebuildOrderLocked(size_t first) {
  const size_t n = playlist_.size();
  order_.resize(n);
  for (size_t i = 0; i < n; i++)
    order_[i] = i;
  if (shuffle_ && n > 1) {
#ifdef ESP_PLATFORM
    streamcore::esp_random_engine rng;
#else
    std::mt19937 rng(std::random_device{}());
#endif
    std::shuffle(order_.begin(), order_.end(), rng);
    // the current track stays where playback is: first in the new order
    auto it = std::find(order_.begin(), order_.end(), first);
    if (it != order_.end())
      std::iter_swap(order_.begin(), it);
  }
  posOf_.assign(n, 0);
  for (size_t i = 0; i < n; i++)
    posOf_[order_[i]] = i;
}

size_t SDFileStream::currentPlIdxLocked() const {
  if (playing_)
    return playing_->plIdx;
  if (!fed_.empty())
    return fed_.front().plIdx;
  return lastPlIdx_;
}

std::optional<size_t> SDFileStream::upcomingLocked(size_t i) const {
  const size_t n = playlist_.size();
  if (!n)
    return std::nullopt;
  size_t cur = currentPlIdxLocked();
  if (cur >= n)
    return std::nullopt;
  size_t k = posOf_[cur] + 1 + i;
  if (k < n)
    return order_[k];
  if (repeat_ == RepeatMode::All && i + 1 < n)
    return order_[k % n];
  return std::nullopt;
}

uint32_t SDFileStream::positionLocked() const {
  uint64_t pos = posBaseMs_;
  if (clockRunning_)
    pos += uint64_t(nowUs() - posStartUs_) / 1000;
  const Fed* t = playing_ ? &*playing_ : nullptr;
  if (t && t->info && t->info->durationMs && pos > t->info->durationMs)
    pos = t->info->durationMs;
  return (uint32_t)pos;
}

void SDFileStream::requestLocked(size_t plIdx, uint32_t startMs) {
  request_ = Request{plIdx, startMs};
  lastPlIdx_ = plIdx;
  atEnd_ = false;
  drainTid_.store(0);
  wantStop_.store(false);
  switch_.store(true);
}

void SDFileStream::ensureTask() {
  bool expected = false;
  if (isRunning_.compare_exchange_strong(expected, true)) {
    if (!startTask()) {
      isRunning_.store(false);
      reportError("SD: could not start the reader task");
    }
  }
}

// ============================================================================
//  StreamBase API
// ============================================================================
void SDFileStream::play(const std::string& uri, const std::string& name) {
  (void)name;
  std::string p = uri;
  if (startsWith(p, "file://"))
    p.erase(0, 7);
  if (p.empty()) {
    resume();
    return;
  }
  if (isDirectory(p))
    playFolder(p);
  else
    playFile(p);
}

void SDFileStream::stop() {
  wantStop_.store(true);
  for (int i = 0; i < 300 && isRunning_.load(); i++)
    sleepMs(10);
  {
    std::lock_guard<std::mutex> lk(plMu_);
    request_.reset();
    fed_.clear();
    if (playing_)
      lastPlIdx_ = playing_->plIdx;
    playing_.reset();
    clockRunning_ = false;
    posBaseMs_ = 0;
  }
  switch_.store(false);
  drainTid_.store(0);
  setPlaybackState(PlaybackState::Stopped);
}

StreamBase::Capabilities SDFileStream::capabilities() {
  Capabilities c;
  std::lock_guard<std::mutex> lk(plMu_);
  const size_t n = playlist_.size();
  const Fed* t = playing_ ? &*playing_ : (!fed_.empty() ? &fed_.front() : nullptr);
  c.volume = true;
  c.pause = t != nullptr || n > 0;
  c.seek = playing_ && playing_->info && playing_->info->seekable &&
           playing_->info->durationMs > 0;
  c.next = n > 1 || (n > 0 && repeat_ != RepeatMode::Off);
  c.previous = n > 0;
  c.shuffle = n > 1;
  c.repeat = n > 0;
  c.queue = c.playQueueItem = n > 1;
  return c;
}

void SDFileStream::pause() {
  {
    std::lock_guard<std::mutex> lk(plMu_);
    if (!playing_ && fed_.empty())
      return;
    posBaseMs_ = positionLocked();
    clockRunning_ = false;
  }
  feed_->feedCommand(AudioControl::PAUSE, 0);
  setPlaybackState(PlaybackState::Paused);
  publishNowPlaying();
}

void SDFileStream::resume() {
  bool restart = false;
  {
    std::lock_guard<std::mutex> lk(plMu_);
    if (playlist_.empty())
      return;
    // nothing loaded any more (end of playlist / stopped): start again
    if (!isRunning_.load() ||
        (readerIdle_.load() && fed_.empty() &&
         playbackState() == PlaybackState::Stopped)) {
      size_t idx = lastPlIdx_ < playlist_.size() ? lastPlIdx_ : 0;
      if (atEnd_ && !order_.empty())
        idx = order_[0];  // played to the end: start over
      requestLocked(idx, 0);
      restart = true;
    } else if (playing_ && !clockRunning_) {
      posStartUs_ = nowUs();
      clockRunning_ = true;
    }
  }
  if (restart) {
    setPlaybackState(PlaybackState::Buffering);
    ensureTask();
    return;
  }
  feed_->feedCommand(AudioControl::PLAY, 0);
  setPlaybackState(PlaybackState::Playing);
  publishNowPlaying();
}

bool SDFileStream::next() {
  {
    std::lock_guard<std::mutex> lk(plMu_);
    const size_t n = playlist_.size();
    if (!n)
      return false;
    size_t cur = currentPlIdxLocked();
    size_t pos = cur < n ? posOf_[cur] : 0;
    size_t target;
    if (pos + 1 < n)
      target = order_[pos + 1];
    else if (repeat_ != RepeatMode::Off)
      target = order_[0];
    else
      return false;
    requestLocked(target, 0);
    clockRunning_ = false;
    posBaseMs_ = 0;
  }
  ensureTask();
  publishNowPlaying();
  return true;
}

bool SDFileStream::previous() {
  {
    std::lock_guard<std::mutex> lk(plMu_);
    const size_t n = playlist_.size();
    if (!n)
      return false;
    size_t cur = currentPlIdxLocked();
    if (cur >= n)
      cur = 0;
    size_t target = cur;  // restart the current track ...
    if (!(playing_ && positionLocked() > 3000)) {  // ... unless near its start
      size_t pos = posOf_[cur];
      if (pos > 0)
        target = order_[pos - 1];
      else if (repeat_ == RepeatMode::All)
        target = order_[n - 1];
    }
    requestLocked(target, 0);
    clockRunning_ = false;
    posBaseMs_ = 0;
  }
  ensureTask();
  publishNowPlaying();
  return true;
}

bool SDFileStream::seek(uint32_t positionMs) {
  {
    std::lock_guard<std::mutex> lk(plMu_);
    if (!playing_ || !playing_->info || !playing_->info->seekable ||
        !playing_->info->durationMs)
      return false;
    if (positionMs >= playing_->info->durationMs)
      positionMs = playing_->info->durationMs > 1000
                       ? playing_->info->durationMs - 1000
                       : 0;
    requestLocked(playing_->plIdx, positionMs);
    // show the new position right away; the clock restarts with the stream
    posBaseMs_ = positionMs;
    clockRunning_ = false;
  }
  ensureTask();
  publishNowPlaying();
  return true;
}

bool SDFileStream::setShuffle(bool on) {
  {
    std::lock_guard<std::mutex> lk(plMu_);
    if (shuffle_ != on) {
      shuffle_ = on;
      rebuildOrderLocked(currentPlIdxLocked());
    }
  }
  publishNowPlaying();
  publishQueue();
  return true;
}

bool SDFileStream::setRepeat(RepeatMode mode) {
  {
    std::lock_guard<std::mutex> lk(plMu_);
    repeat_ = mode;
  }
  publishNowPlaying();
  publishQueue();
  return true;
}

std::vector<StreamBase::TrackInfo> SDFileStream::getQueue(size_t maxItems) {
  std::vector<TrackInfo> out;
  std::lock_guard<std::mutex> lk(plMu_);
  for (size_t i = 0; out.size() < maxItems; i++) {
    auto p = upcomingLocked(i);
    if (!p)
      break;
    const std::string& path = playlist_[*p];
    TrackInfo t;
    t.id = path;
    t.title = sdfile::titleFromFileName(path);
    std::string folder = baseName(parentDir(path));
    if (parentDir(path) != root_)
      t.album = folder;
    out.push_back(std::move(t));
  }
  return out;
}

bool SDFileStream::playQueueItem(size_t index) {
  {
    std::lock_guard<std::mutex> lk(plMu_);
    auto p = upcomingLocked(index);
    if (!p)
      return false;
    requestLocked(*p, 0);
    clockRunning_ = false;
    posBaseMs_ = 0;
  }
  ensureTask();
  return true;
}

StreamBase::NowPlaying SDFileStream::nowPlaying() {
  NowPlaying np;
  np.source = sourceName();
  np.volume = getVolume();
  {
    std::lock_guard<std::mutex> lk(plMu_);
    np.state = isRunning_.load() ? playbackState() : PlaybackState::Stopped;
    np.shuffle = shuffle_;
    np.repeat = repeat_;
    const Fed* t =
        playing_ ? &*playing_ : (!fed_.empty() ? &fed_.front() : nullptr);
    if (t && t->info) {
      const auto& i = *t->info;
      np.track.id = t->path;
      np.track.title = i.title;
      np.track.artist = i.artist;
      np.track.album = i.album;
      np.track.durationMs = i.durationMs;
      np.quality = i.qualityString();
      np.positionMs = playing_ ? positionLocked() : t->startMs;
    } else if (lastPlIdx_ < playlist_.size()) {
      // stopped: show what would play on resume()
      np.track.id = playlist_[lastPlIdx_];
      np.track.title = sdfile::titleFromFileName(np.track.id);
    }
  }
  np.caps = capabilities();
  return np;
}

// ============================================================================
//  sink state (runs in the decoder task)
// ============================================================================
void SDFileStream::onSinkState(uint8_t s) {
  bool publish = false, publishQ = false;
  switch (s) {
    case 0: {  // a stream starts: the oldest fed file becomes audible
      std::lock_guard<std::mutex> lk(plMu_);
      if (!fed_.empty()) {
        playing_ = std::move(fed_.front());
        fed_.pop_front();
        posBaseMs_ = playing_->startMs;
        posStartUs_ = nowUs();
        clockRunning_ = true;
        lastPlIdx_ = playing_->plIdx;
        publish = publishQ = true;
      }
      setPlaybackState(PlaybackState::Playing);
      break;
    }
    case 1:  // playback (also after a pause)
    case 2: {
      std::lock_guard<std::mutex> lk(plMu_);
      if (playing_ && !clockRunning_ && !switch_.load()) {
        posStartUs_ = nowUs();
        clockRunning_ = true;
        publish = true;
      }
      setPlaybackState(PlaybackState::Playing);
      break;
    }
    case 3: {
      std::lock_guard<std::mutex> lk(plMu_);
      if (clockRunning_) {
        posBaseMs_ = positionLocked();
        clockRunning_ = false;
      }
      setPlaybackState(PlaybackState::Paused);
      publish = true;
      break;
    }
    case 7: {  // a stream ended: stopped if nothing follows
      std::lock_guard<std::mutex> lk(plMu_);
      if (fed_.empty() && readerIdle_.load() && !switch_.load()) {
        if (playing_)
          posBaseMs_ = positionLocked();
        if (!wantStop_.load() && !request_)
          atEnd_ = true;
        clockRunning_ = false;
        setPlaybackState(PlaybackState::Stopped);
        publish = true;
      }
      break;
    }
    default:
      break;
  }
  if (publish)
    publishNowPlaying();
  if (publishQ)
    publishQueue();
}

// ============================================================================
//  end of the playlist
// ============================================================================
// The sink keeps a stream "playing" until it is told to stop, even when no
// more data arrives.  Once our last stream is the audible one, soft-stop it:
// the sink plays out what is buffered and then reports state 7 (stopped).
// The check runs as a sink command, i.e. in the decoder task, so the stream
// list is not read while the decoder changes it.
void SDFileStream::tryDrain() {
#ifdef CONFIG_AUDIO_SINK_VS1053
  if (drainCmdPending_.load())
    return;
  auto sink = feed_->audioSink;
  if (!sink || sink->streams.empty()) {
    drainTid_.store(0);  // nothing left in the sink
    return;
  }
  const size_t tid = drainTid_.load();
  drainCmdPending_.store(true);
  sink->feed_command([this, sink, tid](size_t) {
    drainCmdPending_.store(false);
    if (drainTid_.load() != tid)
      return;  // a new request came in meanwhile
    bool queued = false;
    for (auto& st : sink->streams)
      if (st && st->streamId == tid)
        queued = true;
    if (!queued) {
      drainTid_.store(0);
      return;
    }
    if (sink->streams[0]->streamId == tid) {
      sink->soft_stop_feed();
      drainTid_.store(0);
    }
  });
#else
  drainTid_.store(0);
#endif
}

// ============================================================================
//  reader task
// ============================================================================
void SDFileStream::runTask() {
  isRunning_.store(true);
  std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[kChunk]);
  if (!buf) {
    reportError("SD: out of memory");
    isRunning_.store(false);
    return;
  }

  FILE* fp = nullptr;
  uint64_t fpPos = 0;
  Fed cur;
  bool haveCur = false;
  // the three parts of a track (see AudioFileInfo)
  size_t memPos = 0;
  uint64_t f1Pos = 0, f1End = 0;  // header pages from the file
  uint64_t f2Pos = 0, f2End = 0;  // audio data

  auto closeFile = [&]() {
    if (fp)
      fclose(fp);
    fp = nullptr;
    haveCur = false;
  };

  auto openTrack = [&](size_t plIdx, uint32_t startMs) -> bool {
    std::string path;
    {
      std::lock_guard<std::mutex> lk(plMu_);
      if (plIdx >= playlist_.size())
        return false;
      path = playlist_[plIdx];
    }
    fp = fopen(path.c_str(), "rb");
    if (!fp) {
      reportError("SD: cannot open " + path);
      return false;
    }
    auto info = std::make_shared<sdfile::AudioFileInfo>();
    if (!sdfile::probe(fp, path, *info)) {
      reportError("SD: unsupported file " + path);
      fclose(fp);
      fp = nullptr;
      return false;
    }
    if (info->album.empty() && parentDir(path) != root_)
      info->album = baseName(parentDir(path));
    if (!info->seekable || startMs >= info->durationMs)
      startMs = 0;
    uint64_t off = info->offsetForMs(startMs);
    memPos = 0;
    f1Pos = 0;
    f1End = info->prefixFileLen;
    if (off < f1End)
      off = f1End;
    f2Pos = off;
    f2End = info->audioEnd;
    fpPos = UINT64_MAX;  // force a seek

    cur = Fed{};
    cur.tid = audio_->makeUniqueTrackId();
    cur.plIdx = plIdx;
    cur.path = path;
    cur.info = info;
    cur.startMs = startMs;
    haveCur = true;
    readerIdle_.store(false);
    {
      std::lock_guard<std::mutex> lk(plMu_);
      fed_.push_back(cur);
    }
    SC32_LOG(info, "SD: %s [%s] %u ms @%u", path.c_str(),
             info->qualityString().c_str(), (unsigned)info->durationMs,
             (unsigned)startMs);
    return true;
  };

  // next track after `plIdx` when a file ended (repeat / end of list)
  auto openNextAuto = [&](size_t plIdx) -> bool {
    size_t tries = 0;
    for (;;) {
      std::optional<size_t> nxt;
      {
        std::lock_guard<std::mutex> lk(plMu_);
        const size_t n = playlist_.size();
        if (!n || tries++ >= n)
          return false;
        if (repeat_ == RepeatMode::One && plIdx < n) {
          nxt = plIdx;
        } else {
          size_t pos = plIdx < n ? posOf_[plIdx] : 0;
          if (pos + 1 < n)
            nxt = order_[pos + 1];
          else if (repeat_ == RepeatMode::All)
            nxt = order_[0];
        }
      }
      if (!nxt)
        return false;
      if (openTrack(*nxt, 0))
        return true;
      plIdx = *nxt;  // unreadable file: try the one after it
    }
  };

  while (!wantStop_.load()) {
    // ---- new request: drop everything in the sink and start over -----------
    if (switch_.exchange(false)) {
      std::optional<Request> rq;
      {
        std::lock_guard<std::mutex> lk(plMu_);
        rq = request_;
        request_.reset();
        fed_.clear();
      }
      closeFile();
      drainTid_.store(0);
      feed_->feedCommand(AudioControl::FLUSH, 0);
      feed_->feedCommand(AudioControl::DISC, 0);
      if (rq && !openTrack(rq->plIdx, rq->startMs))
        openNextAuto(rq->plIdx);
    }
    if (!haveCur) {
      readerIdle_.store(true);
      if (drainTid_.load())
        tryDrain();
      sleepMs(20);
      continue;
    }

    // ---- next chunk -----------------------------------------------------------
    size_t n = 0;
    const auto& prefix = cur.info->prefixBytes;
    bool fileErr = false;
    if (memPos < prefix.size()) {
      n = std::min(kChunk, prefix.size() - memPos);
      memcpy(buf.get(), prefix.data() + memPos, n);
      memPos += n;
    } else {
      uint64_t* pos = f1Pos < f1End ? &f1Pos : &f2Pos;
      uint64_t end = f1Pos < f1End ? f1End : f2End;
      if (*pos < end) {
        if (fpPos != *pos) {
          if (fseek(fp, (long)*pos, SEEK_SET) != 0)
            fileErr = true;
          fpPos = *pos;
        }
        if (!fileErr) {
          size_t want = (size_t)std::min<uint64_t>(kChunk, end - *pos);
          n = fread(buf.get(), 1, want, fp);
          if (n == 0)
            fileErr = true;
          *pos += n;
          fpPos += n;
        }
      }
    }
    if (n == 0) {
      // end of this file (or read error): chain the next one gap-less
      if (fileErr)
        reportError("SD: read error in " + cur.path);
      size_t done = cur.plIdx;
      size_t doneTid = cur.tid;
      closeFile();
      if (!openNextAuto(done)) {
        readerIdle_.store(true);
        drainTid_.store(doneTid);  // end of the playlist
      }
      continue;
    }

    // ---- feed it --------------------------------------------------------------
    size_t w = 0;
    while (w < n && !wantStop_.load() && !switch_.load()) {
      size_t r = feed_->feedData(buf.get() + w, n - w, cur.tid, false);
      if (r == 0)
        sleepMs(10);
      w += r;
    }
  }

  closeFile();
  feed_->feedCommand(AudioControl::FLUSH, 0);
  feed_->feedCommand(AudioControl::DISC, 0);
  {
    std::lock_guard<std::mutex> lk(plMu_);
    fed_.clear();
    if (playing_)
      lastPlIdx_ = playing_->plIdx;
    playing_.reset();
    clockRunning_ = false;
  }
  readerIdle_.store(true);
  setPlaybackState(PlaybackState::Stopped);
  isRunning_.store(false);
  publishNowPlaying();
}
