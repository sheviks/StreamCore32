# Changelog

## 1.3.0

### New
- **DLNA / UPnP renderer** (`sc_dlna`): SSDP discovery, AVTransport /
  RenderingControl / ConnectionManager, GENA events, gap-less
  (SetNextAVTransportURI), seeking via HTTP Range, LPCM, http and https.
  Takes over and hands back playback like the other sources. PC tests in
  `tools/dlna_test`.
- **Modular build**: every source and every piece of hardware can be switched
  off in menuconfig; headless builds without display / touch. The web UI and
  the e-paper UI hide what is not built in.
- **E-paper UI in the web UI's look**: monospaced font (DejaVu Sans Mono),
  square and flat, titles in capitals; player laid out like the web player
  (shuffle · prev · play · next · repeat, volume, source / quality, "up next").
- **Web UI on phones**: dropdown menu with the current page, queue as a bar
  that opens upwards, volume on its own line when needed.
- Crash log on the SD card (`/sdcard/logs`): restart reason, panic output,
  core dump summary, last log lines.
- Status LED (SK6812): colour and effect per state, modes, brightness.
- Web UI file manager: upload / download, folders, text editor, M3U playlists.
- Settings page (web + display): device name, services on/off, qualities,
  volume, bass / treble, dark mode, LED.
- Changes are mostly made with the help of claude or by claude.
- Documentation and Readme's by claude.

### Changed
- **New repository layout**: all code in `components/` (one ESP-IDF component
  per part, each with its own `Kconfig.sc32` and README); `targets/esp32` only
  holds the project shell, `partitions.csv` and a minimal `sdkconfig.defaults`.
- `bell` is no longer a submodule but a reduced copy (≈ 45 MB → 3 MB).
- mDNS uses the device name as host name (`http://<device name>.local/`).
- Spotify: one CDN connection per track (no more stalls), fallback title
  (track id) in the queue when the metadata is missing.
- Qobuz: quality setting applied from the next track.
- Web UI messages go through a send queue (a slow browser no longer blocks
  the audio).
- Version 1.3.0.
- Changes are mostly made with the help of claude or by claude.

### Fixed
- Crashes with the flash cache disabled (VS1053 DREQ interrupt, NVS work from
  PSRAM stacks).
- Audio stutter while the web UI was connected.
- Spotify client ID was described as optional in menuconfig (it is required).

### Removed
- The old `wiki/` notes and the Nix flake for the former Linux CLI target.
- Unused headers (`Token.h`, `NvsCredStore.h`, `Playlist.h`, `HttpBackoff.h`).

## 1.2.0 and earlier

Spotify Connect and Qobuz Connect on ESP32 + VS1053, internet radio, SD card
player, web UI, e-paper UI (einkui), settings.
