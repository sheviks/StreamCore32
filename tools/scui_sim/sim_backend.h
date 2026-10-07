#pragma once
// Fake scui::Backend for the desktop preview: a radio, an SD "card" with a
// few folders and a pretend Spotify session.  Time is driven by the caller.
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "sc_ui_model.h"

namespace sim {

struct MemFS : einkui::IFileSystem {
  struct F {
    const char* dir;
    const char* name;
    bool isDir;
    uint32_t size;
  };
  std::vector<F> files = {
      {"/sdcard", "Music", true, 0},
      {"/sdcard", "Hörspiele", true, 0},
      {"/sdcard", "Podcasts", true, 0},
      {"/sdcard", "readme.txt", false, 812},
      {"/sdcard", "Intro.mp3", false, 2412001},
      {"/sdcard/Music", "Pink Floyd - The Wall", true, 0},
      {"/sdcard/Music", "Daft Punk - Discovery", true, 0},
      {"/sdcard/Music/Daft Punk - Discovery", "01 - One More Time.flac", false, 41234567},
      {"/sdcard/Music/Daft Punk - Discovery", "02 - Aerodynamic.flac", false, 23234567},
      {"/sdcard/Music/Daft Punk - Discovery", "03 - Digital Love.flac", false, 31234567},
      {"/sdcard/Music/Daft Punk - Discovery", "04 - Harder, Better, Faster, Stronger.flac", false, 26234567},
      {"/sdcard/Music/Daft Punk - Discovery", "05 - Crescendolls.flac", false, 21234567},
      {"/sdcard/Music/Daft Punk - Discovery", "06 - Nightvision.flac", false, 9234567},
      {"/sdcard/Music/Daft Punk - Discovery", "07 - Superheroes.flac", false, 25234567},
      {"/sdcard/Music/Daft Punk - Discovery", "08 - High Life.flac", false, 22234567},
      {"/sdcard/Music/Daft Punk - Discovery", "cover.jpg", false, 234567},
  };
  bool list(const char* path, Entry* out, size_t& count, size_t max) override {
    count = 0;
    for (auto& f : files) {
      if (strcmp(f.dir, path) != 0 || count >= max)
        continue;
      snprintf(out[count].name, sizeof(out[count].name), "%s", f.name);
      out[count].isDir = f.isDir;
      out[count].size = f.size;
      ++count;
    }
    return true;
  }
  std::vector<std::string> audioIn(const std::string& dir) {
    std::vector<std::string> v;
    for (auto& f : files)
      if (dir == f.dir && !f.isDir && strstr(f.name, ".flac"))
        v.push_back(dir + "/" + f.name);
    return v;
  }
};

class Backend : public scui::Backend {
 public:
  uint32_t nowMs = 0;  // advanced by the simulation
  bool sdMounted = true;
  std::vector<std::string> log;

  enum Src { None, Radio, Sd, Spotify } src = None;
  scui::PlayState state = scui::PlayState::Stopped;
  uint32_t posBase = 0, posAt = 0;
  uint8_t volume = 65;
  bool shuffle = false;
  scui::Repeat repeat = scui::Repeat::Off;
  int station = -1;
  std::vector<std::string> playlist;
  size_t plIdx = 0;
  scui::Tone toneV;
  MemFS fs;

  std::vector<scui::Station> st = {
      {"SRF 3", "http://stream.srg-ssr.ch/m/drs3/mp3_128"},
      {"Radio Swiss Jazz", "http://stream.srg-ssr.ch/m/rsj/mp3_128"},
      {"FM4", "https://orf-live.ors-shoutcast.at/fm4-q2a"},
      {"Radio Paradise", "https://stream.radioparadise.com/flac"},
      {"BBC Radio 6 Music", "http://as-hls-ww-live.akamaized.net/6music"},
      {"Deutschlandfunk Kultur", "https://st02.sslstream.dlf.de/dlf/02/128/mp3"},
      {"KEXP 90.3", "https://kexp-mp3-128.streamguys1.com/kexp128.mp3"},
  };

  uint32_t pos() const {
    return posBase + (state == scui::PlayState::Playing ? nowMs - posAt : 0);
  }
  static std::string fileTitle(const std::string& p) {
    std::string s = p.substr(p.find_last_of('/') + 1);
    s = s.substr(0, s.find_last_of('.'));
    if (s.size() > 5 && s[2] == ' ' && s[3] == '-')
      s = s.substr(5);
    return s;
  }

  scui::NowPlaying nowPlaying() override {
    scui::NowPlaying np;
    np.volume = volume;
    if (src == None)
      return np;
    np.hasSource = true;
    np.state = state;
    np.positionMs = pos();
    np.shuffle = shuffle;
    np.repeat = repeat;
    np.caps.pause = true;
    switch (src) {
      case Radio:
        np.source = "Radio";
        np.track.title = "Harder, Better, Faster, Stronger";
        np.track.artist = "Daft Punk";
        np.track.album = st[size_t(station)].name;
        np.quality = "MP3 - 128 kbps / 44.1 kHz";
        np.caps.next = np.caps.previous = np.caps.queue = np.caps.playQueueItem = true;
        break;
      case Sd:
        np.source = "SD";
        np.track.title = fileTitle(playlist[plIdx]);
        np.track.artist = "Daft Punk";
        np.track.album = "Discovery";
        np.track.durationMs = 225000 + uint32_t(plIdx) * 17000;
        np.quality = "FLAC - 16-Bit / 44.1 kHz";
        np.caps.seek = np.caps.next = np.caps.previous = true;
        np.caps.shuffle = np.caps.repeat = np.caps.queue = np.caps.playQueueItem = true;
        break;
      case Spotify:
        np.source = "Spotify";
        np.track.title = "Comfortably Numb";
        np.track.artist = "Pink Floyd";
        np.track.album = "The Wall";
        np.track.durationMs = 383000;
        np.quality = "Ogg Vorbis - 320 kbps";
        np.caps.seek = np.caps.next = np.caps.previous = true;
        np.caps.shuffle = np.caps.repeat = np.caps.queue = np.caps.playQueueItem = true;
        break;
      default:
        break;
    }
    if (np.track.durationMs && np.positionMs > np.track.durationMs)
      np.positionMs = np.track.durationMs;
    return np;
  }
  void togglePause() override {
    posBase = pos();
    posAt = nowMs;
    state = state == scui::PlayState::Playing ? scui::PlayState::Paused
                                              : scui::PlayState::Playing;
    log.push_back(state == scui::PlayState::Playing ? "resume" : "pause");
  }
  void startTrack() {
    posBase = 0;
    posAt = nowMs;
    state = scui::PlayState::Playing;
  }
  bool next() override {
    log.push_back("next");
    if (src == Radio) {
      station = (station + 1) % int(st.size());
    } else if (src == Sd) {
      if (plIdx + 1 >= playlist.size())
        return false;
      plIdx++;
    }
    startTrack();
    return true;
  }
  bool previous() override {
    log.push_back("previous");
    if (src == Radio)
      station = (station + int(st.size()) - 1) % int(st.size());
    else if (src == Sd && plIdx > 0 && pos() < 3000)
      plIdx--;
    startTrack();
    return true;
  }
  bool seek(uint32_t ms) override {
    char b[32];
    snprintf(b, sizeof b, "seek %u", ms);
    log.push_back(b);
    posBase = ms;
    posAt = nowMs;
    return true;
  }
  bool setShuffle(bool on) override {
    shuffle = on;
    log.push_back(on ? "shuffle on" : "shuffle off");
    return true;
  }
  bool setRepeat(scui::Repeat r) override {
    repeat = r;
    log.push_back("repeat " + std::to_string(int(r)));
    return true;
  }
  void setVolume(uint8_t v) override {
    volume = v;
    log.push_back("volume " + std::to_string(v));
  }
  std::vector<scui::Track> queue(size_t maxItems) override {
    std::vector<scui::Track> q;
    if (src == Sd) {
      for (size_t i = plIdx + 1; i < playlist.size() && q.size() < maxItems; i++) {
        scui::Track t;
        t.id = playlist[i];
        t.title = fileTitle(playlist[i]);
        t.album = "Daft Punk - Discovery";
        q.push_back(t);
      }
    } else if (src == Radio) {
      for (auto& s : st)
        q.push_back({s.url, s.name, "", "", 0});
    } else if (src == Spotify) {
      const char* t[][2] = {{"Run Like Hell", "4:20"}, {"Waiting for the Worms", "4:04"},
                            {"Stop", "0:30"}, {"The Trial", "5:13"}, {"Outside the Wall", "1:41"}};
      for (auto& x : t)
        q.push_back({"spotify:track:x", x[0], "Pink Floyd", "The Wall", 240000});
    }
    return q;
  }
  bool playQueueItem(size_t i) override {
    log.push_back("play queue item " + std::to_string(i));
    if (src == Sd) {
      plIdx += i + 1;
      startTrack();
    } else if (src == Radio) {
      station = int(i);
      startTrack();
    }
    return true;
  }
  void stopPlayback() override { state = scui::PlayState::Stopped; }

  std::vector<scui::Station> stations() override { return st; }
  bool playStation(size_t i) override {
    src = Radio;
    station = int(i);
    startTrack();
    log.push_back("play station " + st[i].name);
    return true;
  }
  int currentStation() override { return src == Radio ? station : -1; }

  einkui::IFileSystem* fileSystem() override { return &fs; }
  const char* sdRoot() override { return "/sdcard"; }
  scui::SdInfo sdInfo() override {
    scui::SdInfo i;
    i.present = true;
    i.mounted = sdMounted;
    i.name = "SC64G";
    i.type = "SDXC";
    i.totalBytes = 63864569856ull;
    i.freeBytes = 41234567890ull;
    return i;
  }
  bool sdRemount() override {
    log.push_back("sd remount");
    return sdMounted;
  }
  bool playFile(const std::string& path) override {
    std::string dir = path.substr(0, path.find_last_of('/'));
    playlist = fs.audioIn(dir);
    plIdx = 0;
    for (size_t i = 0; i < playlist.size(); i++)
      if (playlist[i] == path)
        plIdx = i;
    if (playlist.empty())
      playlist.push_back(path);
    src = Sd;
    startTrack();
    log.push_back("play file " + path);
    return true;
  }
  bool playFolder(const std::string& dir) override {
    auto v = fs.audioIn(dir);
    return !v.empty() && playFile(v[0]);
  }

  bool wifiConnected() override { return true; }
  int wifiRssi() override { return -61; }
  std::string wifiSsid() override { return "Guyer-Net"; }
  std::string ipAddress() override { return "192.168.1.42"; }
  void wifiConnect(const std::string& s, const std::string&) override {
    log.push_back("wifi connect " + s);
  }

  scui::Tone tone() override { return toneV; }
  void setTone(const scui::Tone& t) override { toneV = t; }

  // ---- settings ----
  std::map<std::string, int> opts{{"spotify_format", 1}, {"qobuz_format", 7},
                                  {"spotify_enabled", 1}, {"qobuz_enabled", 1},
                                  {"dark_mode", 0}, {"restore_volume", 1},
                                  {"led_mode", 3}, {"led_brightness", 20}};
  std::string name = "StreamCore32";
  bool restartNeeded = false;
  int option(const std::string& k) override { return opts[k]; }
  void setOption(const std::string& k, int v) override {
    if ((k == "spotify_enabled" || k == "qobuz_enabled") && opts[k] != v)
      restartNeeded = true;
    opts[k] = v;
  }
  std::string deviceName() override { return name; }
  void setDeviceName(const std::string& n) override {
    if (n != name) restartNeeded = true;
    name = n;
  }
  bool restartRequired() override { return restartNeeded; }
  void restart() override { log.push_back("restart"); }

  scui::Battery battery() override {
    scui::Battery b;
    b.available = true;
    b.percent = 78;
    b.millivolts = 3987;
    b.milliamps = -112;
    b.tempDeciC = 281;
    return b;
  }
  std::string clockText() override {
    char b[8];
    uint32_t m = 14 * 60 + 32 + nowMs / 60000;
    snprintf(b, sizeof b, "%02u:%02u", unsigned(m / 60 % 24), unsigned(m % 60));
    return b;
  }
  std::string version() override { return "1.3.0"; }
  std::vector<std::pair<std::string, std::string>> systemInfo() override {
    return {{"Version", "1.3.0"},          {"IP address", "192.168.1.42"},
            {"WiFi", "Guyer-Net -61 dBm"}, {"Battery", "78 % / 3.99 V"},
            {"Current", "-112 mA"},        {"Temperature", "28.1 \xC2\xB0" "C"},
            {"Free heap", "182 KB"}};
  }
};

}  // namespace sim
