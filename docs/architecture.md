# Architecture

StreamCore32 is a set of ESP-IDF components. `targets/esp32` is only the
project shell; everything happens in [`sc_app`](../components/sc_app/README.md),
which builds the parts that are enabled in menuconfig and wires them together.

## Overview

```mermaid
flowchart LR
  subgraph CTRL["control"]
    direction TB
    APPS(["Spotify / Qobuz apps"])
    CP(["DLNA control points"])
    BR(["web browser"])
    TOUCH(["touch display"])
  end

  subgraph APP["sc_app"]
    direction TB
    WEB["web UI<br/>(streamcore WebUI)"]
    UI["e-paper UI"]
    SM["StreamManager<br/>one source at a time"]
  end

  subgraph SRC["sources · StreamBase"]
    direction TB
    SP["sc_spotify"]
    QB["sc_qobuz"]
    WR["sc_webstream"]
    SD["sc_sdfile"]
    DL["sc_dlna"]
  end

  subgraph OUT["audio + hardware"]
    direction TB
    AC["AudioControl<br/>(streamcore)"]
    VS["VS1053 decoder<br/>(bell)"]
    SDC["SD card<br/>(sc_sdcard)"]
    EPD["e-paper + touch<br/>(gdey027t91, ft6x36)"]
  end

  APPS -- "zeroconf + cloud" --> SP & QB
  CP -- "SSDP / SOAP / GENA" --> DL
  BR -- "WebSocket" --> WEB
  TOUCH --> UI
  WEB --> SM
  UI --> SM
  SM --> SP & QB & WR & SD & DL
  SP & QB & WR & SD & DL --> AC --> VS
  SD --> SDC
  UI --> EPD
```

| Part | What it is |
|---|---|
| [streamcore](../components/streamcore/README.md) | `StreamBase` (common control API), `AudioControl`, web UI server, zeroconf, logging, time, credential storage |
| [sc_app](../components/sc_app/README.md) | the application: start-up, `StreamManager`, settings, WiFi, device services, crash log, status LED, e-paper UI glue, web UI messages |
| [sc_spotify](../components/sc_spotify/README.md) | Spotify Connect (derived from cspot) |
| [sc_qobuz](../components/sc_qobuz/README.md) | Qobuz Connect |
| [sc_webstream](../components/sc_webstream/README.md) | internet radio (ICY / polled metadata, playlists) |
| [sc_sdfile](../components/sc_sdfile/README.md) | SD card player (gap-less, tags, seeking, playlists) |
| [sc_dlna](../components/sc_dlna/README.md) | DLNA / UPnP media renderer |
| [bell](../components/bell/README.md) | reduced feelfreelinux/bell: HTTP(S), sockets, utilities, nanopb, **VS1053 driver** |
| [sc_sdcard](../components/sc_sdcard/README.md) | SD card (SDMMC, hot plug) |
| [gdey027t91](../components/gdey027t91/README.md) | 2.7" e-paper driver |
| [ft6x36](../components/ft6x36/README.md) | touch controller |
| [bq27220](../components/bq27220/README.md) | battery fuel gauge |
| [i2c_bus](../components/i2c_bus/README.md) | shared I2C bus |
| [eink_ui](../components/eink_ui/README.md) | header-only e-paper UI toolkit |
| [eink_vg](../components/eink_vg/README.md) | vector icons (SVG → bytecode) |
| [Adafruit-GFX](../components/Adafruit-GFX/README.md) | graphics primitives and fonts |

Each source component has its own diagram in its README.

## The common stream interface

Every source derives from `StreamBase` (streamcore). The rest of the firmware
(web UI, e-paper UI, StreamManager) only uses that interface, so a new source
needs no UI code:

```mermaid
classDiagram
  class StreamBase {
    +sourceName()
    +capabilities() Capabilities
    +pause() / resume()
    +next() / previous()
    +seek(ms)
    +setShuffle(on) / setRepeat(mode)
    +setVolume(0..100)
    +getQueue() / playQueueItem(i)
    +nowPlaying() NowPlaying
    +nowPlayingJson() / queueJson()
    +handleCommand(json)
    #feed_ : FeedControl
  }
  StreamBase <|-- SpotifyStream
  StreamBase <|-- QobuzStream
  StreamBase <|-- WebStream
  StreamBase <|-- SDFileStream
  StreamBase <|-- DlnaStream
  StreamBase --> AudioControl : feedData()
  AudioControl --> VS1053 : streams, commands
```

A source that cannot do something says so in `capabilities()`; the UIs hide
or grey out that control.

## Audio path

All sources hand the *encoded* audio (MP3, AAC, FLAC, Ogg Vorbis, WAV, WMA,
MIDI) to the VS1053, which decodes it. There is no software decoder.

```mermaid
sequenceDiagram
  participant S as Source task
  participant F as AudioControl::FeedControl
  participant V as VS1053 sink (bell)
  participant C as VS1053 chip
  S->>F: feedData(bytes, trackId)
  F->>V: Stream::feed_data() (ring buffer, PSRAM)
  Note over V: new trackId = new stream:<br/>queued behind the current one (gap-less)
  V->>C: SDI chunks while DREQ is high (feeder task, DREQ interrupt)
  V-->>S: state_callback(0 start · 1/2 playing · 3 paused · 7 ended)
  S->>F: feedCommand(PAUSE / PLAY / FLUSH / DISC / VOLUME)
```

The state callback is how the sources know which track is audible (and
keep the position clock and "now playing" right with several tracks queued).

## Start-up

```mermaid
sequenceDiagram
  participant M as sc32_app_main
  participant H as hardware
  participant N as network
  M->>M: crash log, NVS, settings
  M->>H: status LED, display, I2C (touch, battery), SD card
  M->>H: VS1053 SPI bus, AudioControl
  M->>M: StreamManager, SD player, radio
  M->>M: device services (housekeeping task)
  M->>H: e-paper UI task (works offline)
  M->>N: WiFi (from NVS, else menuconfig default)
  N-->>M: connected
  M->>N: mDNS (device name), zeroconf, web UI (port 80), file manager
  M->>N: time sync
  M->>N: DLNA renderer, Qobuz Connect, Spotify Connect (if enabled)
  M->>M: becomes the settings writer task (NVS)
```

## Threads

| Task | Stack | Does |
|---|---|---|
| main → settings writer | internal | NVS writes (settings, WiFi, stations) — flash work needs an internal stack |
| VS1053 feeder | | SDI transfers, sink state, commands |
| Spotify / Qobuz / radio / SD / DLNA reader | mostly PSRAM | network or SD → `feedData()` |
| web server (civetweb) + WebSocket sender | PSRAM | web UI, file manager |
| e-paper UI | PSRAM | touch, drawing, polling the active stream |
| housekeeping | PSRAM | SD hot plug, crash log, heap / CPU statistics |
| dlna_net / dlna_evt | PSRAM | SSDP + HTTP, GENA events |
| status LED, battery, spectrum (UDP) | small | |

Tasks with a PSRAM stack must not write flash; they hand that work to the
settings task (`Settings::runLater`).
