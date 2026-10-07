#pragma once
// ============================================================================
//  sc_ui_model.h — what the e-paper UI needs from the rest of StreamCore32.
//
//  The UI (sc_ui_app.h) only talks to this interface, so it has no
//  dependency on FreeRTOS, bell or the streams and can be rendered on a PC
//  (see tools/scui_sim).  On the device sc_ui_device.cpp implements it on top
//  of StreamBase (the active stream), the radio station store, the SD card,
//  WiFi, the battery gauge and the VS10xx tone registers.
// ============================================================================
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace einkui {
struct IFileSystem;
}

namespace scui {

// Mirrors StreamBase::PlaybackState / RepeatMode / Capabilities / NowPlaying
enum class PlayState : uint8_t { Stopped = 0, Buffering, Playing, Paused };
enum class Repeat : uint8_t { Off = 0, All, One };

struct Caps {
  bool pause = false;
  bool seek = false;
  bool next = false;
  bool previous = false;
  bool shuffle = false;
  bool repeat = false;
  bool queue = false;
  bool playQueueItem = false;
  bool volume = true;
};

struct Track {
  std::string id;
  std::string title;
  std::string artist;
  std::string album;
  uint32_t durationMs = 0;  // 0 = live / unknown
};

struct NowPlaying {
  bool hasSource = false;  // a stream is selected
  PlayState state = PlayState::Stopped;
  Track track;
  uint32_t positionMs = 0;
  std::string source;   // "Spotify", "Qobuz", "Radio", "SD"
  std::string quality;  // "FLAC - 16-Bit / 44.1 kHz"
  uint8_t volume = 0;   // 0..100
  bool shuffle = false;
  Repeat repeat = Repeat::Off;
  Caps caps;
};

struct Station {
  std::string name;
  std::string url;
};

struct SdInfo {
  bool present = false;
  bool mounted = false;
  std::string name;
  std::string type;
  uint64_t totalBytes = 0;
  uint64_t freeBytes = 0;
};

struct Battery {
  bool available = false;
  int percent = 0;      // 0..100
  int millivolts = 0;
  int milliamps = 0;    // > 0 while charging
  int tempDeciC = 0;    // 0.1 °C
};

// VS10xx bass / treble enhancer (SCI_BASS)
struct Tone {
  int bassDb = 0;        // 0..15 dB
  int bassFreqHz = 20;   // 20..150 Hz
  float trebleDb = 0;    // -12..10.5 dB (1.5 dB steps)
  int trebleFreqKhz = 1; // 1..15 kHz
};

class Backend {
 public:
  virtual ~Backend() = default;

  // ---- active stream (StreamBase) -------------------------------------------
  virtual NowPlaying nowPlaying() = 0;
  virtual void togglePause() = 0;
  virtual bool next() = 0;
  virtual bool previous() = 0;
  virtual bool seek(uint32_t positionMs) = 0;
  virtual bool setShuffle(bool on) = 0;
  virtual bool setRepeat(Repeat mode) = 0;
  virtual void setVolume(uint8_t percent) = 0;
  virtual std::vector<Track> queue(size_t maxItems) = 0;
  virtual bool playQueueItem(size_t index) = 0;
  virtual void stopPlayback() = 0;

  // ---- radio ------------------------------------------------------------------
  virtual std::vector<Station> stations() = 0;
  virtual bool playStation(size_t index) = 0;
  /** Index of the playing station, -1 if none. */
  virtual int currentStation() = 0;

  // ---- SD card ----------------------------------------------------------------
  virtual einkui::IFileSystem* fileSystem() = 0;
  virtual const char* sdRoot() = 0;
  virtual SdInfo sdInfo() = 0;
  virtual bool sdRemount() = 0;
  virtual bool playFile(const std::string& path) = 0;
  virtual bool playFolder(const std::string& dir) = 0;

  // ---- connectivity -------------------------------------------------------------
  virtual bool wifiConnected() = 0;
  virtual int wifiRssi() = 0;  // dBm, 0 = not connected
  virtual std::string wifiSsid() = 0;
  virtual std::string ipAddress() = 0;
  virtual void wifiConnect(const std::string& ssid,
                           const std::string& password) = 0;

  // ---- audio settings -------------------------------------------------------------
  virtual Tone tone() = 0;
  virtual void setTone(const Tone& t) = 0;

  // ---- settings (persistent, see Settings.h) ---------------------------------------
  // keys: "spotify_format", "qobuz_format", "spotify_enabled", "qobuz_enabled", "dlna_enabled",
  //       "dark_mode", "restore_volume",
  //       "led_mode" (0 off, 1 WiFi start, 2 WiFi only, 3 all), "led_brightness"
  virtual int option(const std::string& key) = 0;
  /** Built into this firmware? "spotify", "qobuz", "radio", "sd", "led" */
  virtual bool has(const char* feature) { (void)feature; return true; }
  virtual void setOption(const std::string& key, int value) = 0;
  virtual std::string deviceName() = 0;
  virtual void setDeviceName(const std::string& name) = 0;
  /** Device name / services changed: only active after a restart. */
  virtual bool restartRequired() = 0;
  virtual void restart() = 0;

  // ---- system ---------------------------------------------------------------------
  virtual Battery battery() = 0;
  /** "HH:MM" or "" while the clock is not set. */
  virtual std::string clockText() = 0;
  virtual std::string version() = 0;
  virtual std::vector<std::pair<std::string, std::string>> systemInfo() = 0;
};

}  // namespace scui
