#include "DlnaStream.h"

#include <algorithm>
#include <cstring>

#include "DlnaHttp.h"
#include "Logger.h"
#include "esp_timer.h"

namespace {

constexpr size_t kChunk = 8192;  // bytes per read / sink feed

std::string lowerStr(std::string s) {
  for (auto& c : s)
    c = (char)std::tolower((unsigned char)c);
  return s;
}

// file name for the probe's extension fallback
std::string pseudoName(const std::string& mime, const std::string& url) {
  const std::string m = lowerStr(mime);
  struct {
    const char* key;
    const char* ext;
  } map[] = {{"flac", ".flac"}, {"mpeg", ".mp3"},  {"mp3", ".mp3"},
             {"wav", ".wav"},   {"wave", ".wav"},  {"ogg", ".ogg"},
             {"vorbis", ".ogg"}, {"aac", ".aac"},  {"mp4", ".m4a"},
             {"m4a", ".m4a"},   {"wma", ".wma"},   {"midi", ".mid"}};
  for (auto& e : map)
    if (m.find(e.key) != std::string::npos)
      return std::string("stream") + e.ext;
  std::string p = url.substr(0, url.find('?'));
  auto slash = p.rfind('/');
  return slash == std::string::npos ? p : p.substr(slash + 1);
}

std::string codecLabel(const std::string& mime) {
  const std::string m = lowerStr(mime);
  if (m.find("flac") != std::string::npos)
    return "FLAC";
  if (m.find("mpeg") != std::string::npos || m.find("mp3") != std::string::npos)
    return "MP3";
  if (m.find("aac") != std::string::npos || m.find("mp4") != std::string::npos ||
      m.find("m4a") != std::string::npos)
    return "AAC";
  if (m.find("ogg") != std::string::npos || m.find("vorbis") != std::string::npos)
    return "Ogg Vorbis";
  if (m.find("wav") != std::string::npos)
    return "WAV";
  if (m.find("wma") != std::string::npos)
    return "WMA";
  return "Stream";
}

std::string titleFromUrl(const std::string& url) {
  std::string p = url.substr(0, url.find('?'));
  auto slash = p.rfind('/');
  if (slash != std::string::npos)
    p = p.substr(slash + 1);
  return p.empty() ? url : p;
}

// "audio/L16;rate=48000;channels=1" -> rate / channels
void parseL16(const std::string& mime, uint32_t& rate, uint8_t& ch) {
  rate = 44100;
  ch = 2;
  const std::string m = lowerStr(mime);
  auto r = m.find("rate=");
  if (r != std::string::npos)
    rate = (uint32_t)atoi(m.c_str() + r + 5);
  auto c = m.find("channels=");
  if (c != std::string::npos)
    ch = (uint8_t)atoi(m.c_str() + c + 9);
  if (!rate)
    rate = 44100;
  if (!ch || ch > 2)
    ch = 2;
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 0; i < 4; i++)
    v.push_back(uint8_t(x >> (8 * i)));
}
void put16(std::vector<uint8_t>& v, uint16_t x) {
  v.push_back(uint8_t(x));
  v.push_back(uint8_t(x >> 8));
}

// WAV header for raw big-endian PCM (the data is byte swapped while feeding)
std::shared_ptr<sdfile::AudioFileInfo> l16Info(uint32_t rate, uint8_t ch,
                                               uint64_t size, bool seekable) {
  auto i = std::make_shared<sdfile::AudioFileInfo>();
  i->codec = sdfile::Codec::WAV;
  i->sampleRate = rate;
  i->channels = ch;
  i->bitsPerSample = 16;
  i->blockAlign = ch * 2u;
  const uint32_t byteRate = rate * ch * 2u;
  i->bitrateKbps = byteRate * 8 / 1000;
  i->fileSize = size;
  i->audioStart = 0;
  i->audioEnd = size ? size : ~0ull;
  if (size)
    i->durationMs = uint32_t(size * 1000 / byteRate);
  i->seekable = seekable && size > 0;
  const uint32_t data = size && size < 0x7FFFFFF0ull ? uint32_t(size) : 0x7FFFFFF0u;
  auto& h = i->prefixBytes;
  h.insert(h.end(), {'R', 'I', 'F', 'F'});
  put32(h, data + 36);
  h.insert(h.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put32(h, 16);
  put16(h, 1);  // PCM
  put16(h, ch);
  put32(h, rate);
  put32(h, byteRate);
  put16(h, uint16_t(ch * 2));
  put16(h, 16);
  h.insert(h.end(), {'d', 'a', 't', 'a'});
  put32(h, data);
  return i;
}

}  // namespace

// ============================================================================
DlnaStream::DlnaStream(std::shared_ptr<AudioControl> audio)
    // stack in PSRAM: network + sink only (no flash access); TLS needs room
    : StreamBase("DLNA", audio, 1024 * 12, 2, 1, true) {
  feed_->state_callback = [this](uint8_t s) { onSinkState(s); };
}

DlnaStream::~DlnaStream() {
  halt();
}

int64_t DlnaStream::nowUs() {
  return esp_timer_get_time();
}

void DlnaStream::changed() {
  publishNowPlaying();
  if (renderer_)
    renderer_->notify();
}

void DlnaStream::ensureTask() {
  bool expected = false;
  if (isRunning_.compare_exchange_strong(expected, true)) {
    if (!startTask()) {
      isRunning_.store(false);
      reportError("DLNA: could not start the reader task");
    }
  }
}

// stop reading, keep the current / next track
void DlnaStream::halt() {
  wantStop_.store(true);
  for (int i = 0; i < 400 && isRunning_.load(); i++)
    sleepMs(10);
  {
    std::lock_guard<std::mutex> lk(mu2_);
    request_.reset();
    fed_.clear();
    playing_.reset();
    chained_.reset();
    clockRunning_ = false;
  }
  switch_.store(false);
  drainTid_.store(0);
  setPlaybackState(PlaybackState::Stopped);
}

void DlnaStream::requestLocked(uint32_t startMs) {
  request_ = startMs;
  chained_.reset();
  drainTid_.store(0);
  wantStop_.store(false);
  switch_.store(true);
}

void DlnaStream::startPlayback(uint32_t startMs) {
  if (onActivate_)
    onActivate_();  // DLNA becomes the active source (stops the others)
  {
    std::lock_guard<std::mutex> lk(mu2_);
    detached_ = false;
    error_ = false;
    seekPendingMs_ = 0;
    requestLocked(startMs);
    clockRunning_ = false;
    posBaseMs_ = startMs;
  }
  feed_->feedCommand(AudioControl::VOLUME_LINEAR, (uint32_t)(muted_ ? 0 : getVolume()),
                     std::optional<uint32_t>(100));
  setPlaybackState(PlaybackState::Buffering);
  ensureTask();
  changed();
}

uint32_t DlnaStream::positionLocked() const {
  uint64_t pos = posBaseMs_;
  if (clockRunning_)
    pos += uint64_t(nowUs() - posStartUs_) / 1000;
  const uint32_t d = durationLocked();
  if (d && pos > d)
    pos = d;
  return (uint32_t)pos;
}

uint32_t DlnaStream::durationLocked() const {
  const Fed* t = playing_ ? &*playing_ : (!fed_.empty() ? &fed_.front() : nullptr);
  if (t) {
    if (t->info && t->info->durationMs)
      return t->info->durationMs;
    return t->track ? t->track->durationMs : 0;
  }
  return cur_ ? cur_->durationMs : 0;
}

bool DlnaStream::seekableLocked() const {
  const Fed* t = playing_ ? &*playing_ : (!fed_.empty() ? &fed_.front() : nullptr);
  return t && t->seekable && durationLocked() > 0;
}

// ============================================================ DLNA actions ==
int DlnaStream::dSetUri(TrackPtr t) {
  bool wasPlaying;
  {
    std::lock_guard<std::mutex> lk(mu2_);
    const auto ps = playbackState();
    wasPlaying = t && isRunning_.load() && !detached_ &&
                 (ps == PlaybackState::Playing || ps == PlaybackState::Buffering);
    cur_ = t;
    next_.reset();
    error_ = false;
    seekPendingMs_ = 0;
    detached_ = false;
  }
  if (wasPlaying) {
    startPlayback(0);  // control points expect it to keep playing
  } else {
    halt();
    changed();
  }
  return dlna::kOk;
}

int DlnaStream::dSetNextUri(TrackPtr t) {
  {
    std::lock_guard<std::mutex> lk(mu2_);
    if (!cur_)
      return dlna::kTransitionNotAvailable;
    next_ = std::move(t);
  }
  changed();
  return dlna::kOk;
}

int DlnaStream::dPlay() {
  bool resumeOnly = false;
  uint32_t from = 0;
  {
    std::lock_guard<std::mutex> lk(mu2_);
    if (!cur_)
      return dlna::kTransitionNotAvailable;
    const auto ps = playbackState();
    if (detached_) {
      from = detachedMs_;
    } else if (isRunning_.load() && (playing_ || !fed_.empty() || request_)) {
      if (ps == PlaybackState::Paused)
        resumeOnly = true;
      else if (ps == PlaybackState::Playing || ps == PlaybackState::Buffering)
        return dlna::kOk;  // already playing
      else
        from = seekPendingMs_;
    } else {
      from = seekPendingMs_;
    }
  }
  if (resumeOnly)
    resume();
  else
    startPlayback(from);
  return dlna::kOk;
}

int DlnaStream::dPause() {
  {
    std::lock_guard<std::mutex> lk(mu2_);
    if (!cur_ || detached_)
      return dlna::kTransitionNotAvailable;
  }
  const auto ps = playbackState();
  if (ps == PlaybackState::Paused)
    return dlna::kOk;
  if (ps != PlaybackState::Playing && ps != PlaybackState::Buffering)
    return dlna::kTransitionNotAvailable;
  pause();
  return dlna::kOk;
}

int DlnaStream::dStop() {
  halt();
  {
    std::lock_guard<std::mutex> lk(mu2_);
    detached_ = false;
    seekPendingMs_ = 0;
    posBaseMs_ = 0;
  }
  changed();
  return dlna::kOk;
}

int DlnaStream::dSeek(uint32_t ms) {
  {
    std::lock_guard<std::mutex> lk(mu2_);
    if (!cur_)
      return dlna::kTransitionNotAvailable;
    if (detached_) {
      detachedMs_ = ms;
      return dlna::kOk;
    }
    const bool loaded = isRunning_.load() && (playing_ || !fed_.empty() || request_);
    if (!loaded) {
      // stopped: Play starts there
      seekPendingMs_ = ms;
      posBaseMs_ = ms;
      return dlna::kOk;
    }
    if (!seekableLocked())
      return dlna::kTransitionNotAvailable;
    const uint32_t d = durationLocked();
    if (d && ms >= d)
      return dlna::kIllegalSeekTarget;
    requestLocked(ms);
    posBaseMs_ = ms;
    clockRunning_ = false;
  }
  ensureTask();
  changed();
  return dlna::kOk;
}

int DlnaStream::dNext() {
  bool playing;
  {
    std::lock_guard<std::mutex> lk(mu2_);
    if (!next_)
      return dlna::kTransitionNotAvailable;
    cur_ = next_;
    next_.reset();
    playing = !detached_ && isRunning_.load() && playbackState() != PlaybackState::Stopped;
  }
  if (playing) {
    startPlayback(0);
  } else {
    halt();
    changed();
  }
  return dlna::kOk;
}

int DlnaStream::dPrevious() {
  {
    std::lock_guard<std::mutex> lk(mu2_);
    if (!cur_)
      return dlna::kTransitionNotAvailable;
  }
  if (dSeek(0) == dlna::kOk)
    return dlna::kOk;
  startPlayback(0);  // not seekable: start the track again
  return dlna::kOk;
}

void DlnaStream::dSetMute(bool on) {
  {
    std::lock_guard<std::mutex> lk(mu2_);
    muted_ = on;
  }
  if (isRunning_.load() && !detached_)
    feed_->feedCommand(AudioControl::VOLUME_LINEAR, (uint32_t)(on ? 0 : getVolume()),
                       std::optional<uint32_t>(100));
  changed();
}

void DlnaStream::setVolume(uint8_t percent) {
  percent = std::min<uint8_t>(percent, 100);
  volumePercent_.store(percent);
  bool mute, detached;
  {
    std::lock_guard<std::mutex> lk(mu2_);
    mute = muted_;
    detached = detached_;
  }
  // only while DLNA owns the sink (a control point must not change the
  // volume of another source)
  if (!mute && !detached && isRunning_.load())
    feed_->feedCommand(AudioControl::VOLUME_LINEAR, (uint32_t)percent,
                       std::optional<uint32_t>(100));
  changed();
}

dlna::PlayerStatus DlnaStream::dStatus() {
  dlna::PlayerStatus s;
  s.volume = getVolume();
  std::lock_guard<std::mutex> lk(mu2_);
  s.current = cur_;
  s.next = next_;
  s.error = error_;
  s.mute = muted_;
  s.durationMs = durationLocked();
  s.seekable = seekableLocked() || (detached_ && cur_);
  if (!cur_) {
    s.state = dlna::Transport::NoMedia;
  } else if (detached_) {
    s.state = dlna::Transport::Paused;
    s.positionMs = detachedMs_;
    return s;
  } else if (!isRunning_.load()) {
    s.state = dlna::Transport::Stopped;
  } else if (request_ || switch_.load() || (!playing_ && !fed_.empty())) {
    s.state = dlna::Transport::Transitioning;
  } else {
    switch (playbackState()) {
      case PlaybackState::Playing:
        s.state = playing_ ? dlna::Transport::Playing : dlna::Transport::Transitioning;
        break;
      case PlaybackState::Paused:
        s.state = dlna::Transport::Paused;
        break;
      case PlaybackState::Buffering:
        s.state = dlna::Transport::Transitioning;
        break;
      default:
        s.state = dlna::Transport::Stopped;
        break;
    }
  }
  s.positionMs = s.state == dlna::Transport::Stopped && !playing_ ? seekPendingMs_
                                                                   : positionLocked();
  return s;
}

// ============================================================= StreamBase ==
StreamBase::Capabilities DlnaStream::capabilities() {
  Capabilities c;
  std::lock_guard<std::mutex> lk(mu2_);
  c.volume = true;
  c.pause = cur_ != nullptr;
  c.seek = seekableLocked();
  c.next = next_ != nullptr;
  c.previous = cur_ != nullptr;
  c.queue = c.playQueueItem = next_ != nullptr;
  return c;
}

void DlnaStream::play(const std::string& uri, const std::string& displayName) {
  auto t = std::make_shared<dlna::Track>();
  t->uri = uri;
  t->title = displayName;
  dSetUri(t);
  startPlayback(0);
}

void DlnaStream::stop() {
  // another source takes over: keep the track, show "paused" to the
  // control point (it would skip to its next track on STOPPED)
  {
    std::lock_guard<std::mutex> lk(mu2_);
    if (cur_ && (playing_ || !fed_.empty() || request_)) {
      detached_ = true;
      detachedMs_ = positionLocked();
    }
  }
  halt();
  changed();
}

void DlnaStream::pause() {
  {
    std::lock_guard<std::mutex> lk(mu2_);
    if (!playing_ && fed_.empty())
      return;
    posBaseMs_ = positionLocked();
    clockRunning_ = false;
  }
  feed_->feedCommand(AudioControl::PAUSE, 0);
  setPlaybackState(PlaybackState::Paused);
  changed();
}

void DlnaStream::resume() {
  bool restart = false;
  uint32_t from = 0;
  {
    std::lock_guard<std::mutex> lk(mu2_);
    if (!cur_)
      return;
    if (detached_ || !isRunning_.load() || (readerIdle_.load() && fed_.empty() && !playing_)) {
      restart = true;
      from = detached_ ? detachedMs_ : 0;
    } else if (playing_ && !clockRunning_) {
      posStartUs_ = nowUs();
      clockRunning_ = true;
    }
  }
  if (restart) {
    startPlayback(from);
    return;
  }
  feed_->feedCommand(AudioControl::PLAY, 0);
  setPlaybackState(PlaybackState::Playing);
  changed();
}

bool DlnaStream::next() {
  return dNext() == dlna::kOk;
}

bool DlnaStream::previous() {
  return dPrevious() == dlna::kOk;
}

bool DlnaStream::seek(uint32_t positionMs) {
  return dSeek(positionMs) == dlna::kOk;
}

std::vector<StreamBase::TrackInfo> DlnaStream::getQueue(size_t maxItems) {
  std::vector<TrackInfo> out;
  std::lock_guard<std::mutex> lk(mu2_);
  if (next_ && maxItems) {
    TrackInfo t;
    t.id = next_->uri;
    t.title = next_->title.empty() ? titleFromUrl(next_->uri) : next_->title;
    t.artist = next_->artist;
    t.album = next_->album;
    t.imageUrl = next_->artUri;
    t.durationMs = next_->durationMs;
    out.push_back(std::move(t));
  }
  return out;
}

bool DlnaStream::playQueueItem(size_t index) {
  return index == 0 && next();
}

StreamBase::NowPlaying DlnaStream::nowPlaying() {
  NowPlaying np;
  np.source = sourceName();
  np.volume = getVolume();
  np.caps = capabilities();
  auto st = dStatus();
  switch (st.state) {
    case dlna::Transport::Playing:
      np.state = PlaybackState::Playing;
      break;
    case dlna::Transport::Paused:
      np.state = PlaybackState::Paused;
      break;
    case dlna::Transport::Transitioning:
      np.state = PlaybackState::Buffering;
      break;
    default:
      np.state = PlaybackState::Stopped;
      break;
  }
  np.positionMs = st.positionMs;
  np.track.durationMs = st.durationMs;
  std::lock_guard<std::mutex> lk(mu2_);
  const Fed* f = playing_ ? &*playing_ : (!fed_.empty() ? &fed_.front() : nullptr);
  TrackPtr t = f ? f->track : cur_;
  if (t) {
    np.track.id = t->uri;
    np.track.title = t->title;
    np.track.artist = t->artist;
    np.track.album = t->album;
    np.track.imageUrl = t->artUri;
    if (f && f->info) {  // tags of the file when the control point sent none
      if (np.track.title.empty())
        np.track.title = f->info->title;
      if (np.track.artist.empty())
        np.track.artist = f->info->artist;
      if (np.track.album.empty())
        np.track.album = f->info->album;
    }
    if (np.track.title.empty() || np.track.title == "stream")
      np.track.title = titleFromUrl(t->uri);
  }
  if (f)
    np.quality = f->quality;
  return np;
}

// ============================================================== sink state ==
void DlnaStream::onSinkState(uint8_t s) {
  bool publish = false;
  switch (s) {
    case 0: {  // a stream starts: the oldest fed track becomes audible
      std::lock_guard<std::mutex> lk(mu2_);
      if (!fed_.empty()) {
        playing_ = std::move(fed_.front());
        fed_.pop_front();
        posBaseMs_ = playing_->startMs;
        posStartUs_ = nowUs();
        clockRunning_ = true;
        if (playing_->track && playing_->track != cur_) {
          // gap-less transition: the next track is the current one now
          if (playing_->track == next_)
            next_.reset();
          cur_ = playing_->track;
        }
        if (chained_ == cur_)
          chained_.reset();
        publish = true;
      }
      setPlaybackState(PlaybackState::Playing);
      break;
    }
    case 1:
    case 2: {
      std::lock_guard<std::mutex> lk(mu2_);
      if (playing_ && !clockRunning_ && !switch_.load()) {
        posStartUs_ = nowUs();
        clockRunning_ = true;
        publish = true;
      }
      if (playbackState() != PlaybackState::Playing) {
        setPlaybackState(PlaybackState::Playing);
        publish = true;
      }
      break;
    }
    case 3: {
      std::lock_guard<std::mutex> lk(mu2_);
      if (clockRunning_) {
        posBaseMs_ = positionLocked();
        clockRunning_ = false;
      }
      setPlaybackState(PlaybackState::Paused);
      publish = true;
      break;
    }
    case 7: {  // a stream ended: stopped if nothing follows
      std::lock_guard<std::mutex> lk(mu2_);
      if (fed_.empty() && readerIdle_.load() && !switch_.load() && !request_) {
        clockRunning_ = false;
        posBaseMs_ = 0;
        playing_.reset();
        setPlaybackState(PlaybackState::Stopped);
        publish = true;
      }
      break;
    }
    default:
      break;
  }
  if (publish)
    changed();
}

// the last stream: let the sink play it out, then stop it (see SDFileStream)
void DlnaStream::tryDrain() {
#ifdef CONFIG_AUDIO_SINK_VS1053
  if (drainCmdPending_.load())
    return;
  auto sink = feed_->audioSink;
  if (!sink || sink->streams.empty()) {
    drainTid_.store(0);
    return;
  }
  const size_t tid = drainTid_.load();
  drainCmdPending_.store(true);
  sink->feed_command([this, sink, tid](size_t) {
    drainCmdPending_.store(false);
    if (drainTid_.load() != tid)
      return;
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

// ================================================================== reader ==
void DlnaStream::runTask() {
  isRunning_.store(true);
  std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[kChunk + 2]);
  if (!buf) {
    reportError("DLNA: out of memory");
    isRunning_.store(false);
    return;
  }
  auto abortFn = [this] { return wantStop_.load() || switch_.load(); };
  std::unique_ptr<dlna::HttpFile> file;
  Fed cur;
  bool haveCur = false;
  bool l16 = false, live = false;
  int liveRetries = 0;
  size_t memPos = 0;
  uint64_t f1Pos = 0, f1End = 0;  // header bytes re-sent from the file
  uint64_t f2Pos = 0, f2End = 0;  // audio data

  auto closeFile = [&]() {
    file.reset();
    haveCur = false;
  };

  auto openTrack = [&](TrackPtr t, uint32_t startMs) -> bool {
    file.reset(new dlna::HttpFile(abortFn));
    if (!file->open(t->uri)) {
      reportError("DLNA: cannot open " + t->uri);
      file.reset();
      return false;
    }
    const std::string mime = !t->mime.empty() ? t->mime : file->contentType();
    std::shared_ptr<sdfile::AudioFileInfo> info;
    l16 = lowerStr(mime).rfind("audio/l16", 0) == 0;
    if (l16) {
      uint32_t rate;
      uint8_t ch;
      parseL16(mime, rate, ch);
      info = l16Info(rate, ch, file->size(), file->seekable());
    } else if (file->seekable()) {
      // a file: probe it like the SD player does (duration, seek table)
      FILE* f = file->asFile();
      auto inf = std::make_shared<sdfile::AudioFileInfo>();
      if (f) {
        if (sdfile::probe(f, pseudoName(mime, t->uri), *inf) && inf->valid())
          info = inf;
        fclose(f);
      }
    }
    Fed n;
    n.track = t;
    if (info) {
      n.seekable = info->seekable && file->seekable();
      if (!n.seekable || !info->durationMs || startMs >= info->durationMs)
        startMs = 0;
      uint64_t off = info->offsetForMs(startMs);
      f1End = info->prefixFileLen;
      if (off < f1End)
        off = f1End;
      f2Pos = off;
      f2End = info->audioEnd;
      n.quality = info->qualityString();
    } else {
      // unknown / live: front to back, the decoder finds out what it is
      startMs = 0;
      f1End = 0;
      f2Pos = 0;
      f2End = file->size() ? file->size() : ~0ull;
      n.quality = codecLabel(mime);
    }
    memPos = 0;
    f1Pos = 0;
    live = file->size() == 0;
    liveRetries = 0;
    n.info = info;
    n.startMs = startMs;
    n.tid = audio_->makeUniqueTrackId();
    cur = n;
    haveCur = true;
    readerIdle_.store(false);
    {
      std::lock_guard<std::mutex> lk(mu2_);
      fed_.push_back(cur);
    }
    SC32_LOG(info, "DLNA: %s [%s%s] %u ms @%u", t->uri.c_str(), n.quality.c_str(),
             live ? ", live" : n.seekable ? ", seekable" : "",
             (unsigned)(info ? info->durationMs : t->durationMs), (unsigned)startMs);
    return true;
  };

  while (!wantStop_.load()) {
    // ---- new request: drop everything in the sink and start over ----------
    if (switch_.exchange(false)) {
      std::optional<uint32_t> rq;
      TrackPtr t;
      {
        std::lock_guard<std::mutex> lk(mu2_);
        rq = request_;
        request_.reset();
        fed_.clear();
        chained_.reset();
        t = cur_;
      }
      closeFile();
      drainTid_.store(0);
      feed_->feedCommand(AudioControl::FLUSH, 0);
      feed_->feedCommand(AudioControl::DISC, 0);
      if (rq && t && !openTrack(t, *rq) && !switch_.load()) {
        {
          std::lock_guard<std::mutex> lk(mu2_);
          error_ = true;
        }
        setPlaybackState(PlaybackState::Stopped);
        changed();
      }
      continue;
    }
    if (!haveCur) {
      // SetNextAVTransportURI came late: chain it while the last one plays
      TrackPtr nx;
      {
        std::lock_guard<std::mutex> lk(mu2_);
        if (next_ && next_ != chained_ && drainTid_.load() && cur_)
          nx = next_;
      }
      if (nx) {
        size_t dt = drainTid_.exchange(0);
        bool ok = openTrack(nx, 0);
        std::lock_guard<std::mutex> lk(mu2_);
        chained_ = nx;
        if (!ok)
          drainTid_.store(dt);
        continue;
      }
      readerIdle_.store(true);
      if (drainTid_.load())
        tryDrain();
      sleepMs(20);
      continue;
    }

    // ---- next chunk -----------------------------------------------------------
    size_t n = 0;
    bool readErr = false;
    const std::vector<uint8_t>* prefix = cur.info ? &cur.info->prefixBytes : nullptr;
    bool data = false;
    if (prefix && memPos < prefix->size()) {
      n = std::min(kChunk, prefix->size() - memPos);
      memcpy(buf.get(), prefix->data() + memPos, n);
      memPos += n;
    } else {
      const bool hdr = f1Pos < f1End;
      uint64_t* pos = hdr ? &f1Pos : &f2Pos;
      const uint64_t end = hdr ? f1End : f2End;
      data = !hdr;
      if (*pos < end) {
        size_t want = (size_t)std::min<uint64_t>(kChunk, end - *pos);
        int r = file->readAt(*pos, buf.get(), want);
        if (r > 0) {
          n = size_t(r);
          *pos += n;
          // L16: whole samples only (the bytes are swapped below)
          if (l16 && data && (n & 1)) {
            if (*pos < end && file->readFull(*pos, buf.get() + n, 1) == 1) {
              n++;
              (*pos)++;
            } else {
              n--;
            }
          }
          liveRetries = 0;
        } else if (r < 0) {
          readErr = true;
        } else if (hdr) {
          f1Pos = f1End;  // short header: go on with the audio
          continue;
        }
      }
    }
    if (readErr && live && liveRetries < 5 && !abortFn()) {
      // live stream dropped: connect again (same sink stream)
      liveRetries++;
      SC32_LOG(info, "DLNA: live stream dropped, reconnecting (%d)", liveRetries);
      sleepMs(500 * liveRetries);
      std::string url = file->url();
      if (file->open(url)) {
        f2Pos = 0;
        f2End = file->size() ? file->size() : ~0ull;
      }
      continue;
    }
    if (n == 0) {
      if (abortFn())
        continue;
      // end of this track (or read error): chain the next one gap-less
      if (readErr)
        reportError("DLNA: read error in " + cur.track->uri);
      const size_t doneTid = cur.tid;
      closeFile();
      TrackPtr nx;
      {
        std::lock_guard<std::mutex> lk(mu2_);
        if (next_ && next_ != chained_)
          nx = next_;
      }
      bool chained = false;
      if (nx) {
        chained = openTrack(nx, 0);
        std::lock_guard<std::mutex> lk(mu2_);
        chained_ = nx;
      }
      if (!chained) {
        readerIdle_.store(true);
        drainTid_.store(doneTid);  // nothing follows: play out, then stop
      }
      continue;
    }
    if (l16 && data) {
      uint8_t* p = buf.get();
      for (size_t i = 0; i + 1 < n; i += 2)
        std::swap(p[i], p[i + 1]);
    }

    // ---- feed it ----------------------------------------------------------------
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
    std::lock_guard<std::mutex> lk(mu2_);
    fed_.clear();
    playing_.reset();
    chained_.reset();
    clockRunning_ = false;
  }
  readerIdle_.store(true);
  setPlaybackState(PlaybackState::Stopped);
  isRunning_.store(false);
  // a request that came in while this run was ending must not get lost
  if (switch_.load() && !wantStop_.load()) {
    setPlaybackState(PlaybackState::Buffering);
    ensureTask();
  }
  changed();
}
