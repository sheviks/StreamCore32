# sc_qobuz

Qobuz Connect receiver: the device shows up in the Qobuz apps as a speaker
and plays MP3 or FLAC up to 24 bit / 192 kHz (decoded by the VS1053).

> Without a paid Qobuz subscription Qobuz only plays 30 second previews.

Enable: menuconfig → StreamCore32 → Streaming sources → **Qobuz Connect**.

## How it works

```mermaid
sequenceDiagram
  participant App as Qobuz app
  participant ZC as ZeroConf (streamcore)
  participant QS as QobuzStream
  participant API as Qobuz API
  participant WS as Qobuz Connect (WebSocket)
  participant Q as QobuzQueue
  participant P as QobuzPlayer
  Note over QS: first start: QobuzConfig reads app id /<br/>secret from play.qobuz.com (kept in NVS)
  App->>ZC: mDNS _qobuz-connect, get-display-info, get-connect-info
  App->>ZC: connect-to-qconnect (session id, JWTs)
  ZC->>QS: tokens, start
  QS->>WS: WsManager: connect, authenticate, register renderer
  WS-->>QS: queue state, play / pause / seek / volume / loop / shuffle
  QS->>Q: queue updates (add, insert, remove, shuffle, autoplay)
  Q->>API: track metadata, getFileUrl (signed request)
  P->>API: streaming URL (segmented FLAC or file)
  P->>P: read, feedData → VS1053
  P-->>WS: renderer state (position, playing, buffer)
  Note over QS: Heartbeat: refresh tokens, report volume
```

```mermaid
flowchart LR
  QS["QobuzStream<br/>StreamBase API,<br/>zeroconf, login, tokens"] --> WSM["WsManager<br/>batches, auth,<br/>reconnect"] --> WSC["WebSocketClient<br/>TLS, frames, ping"]
  QS --> Q["QobuzQueue<br/>queue state, shuffle,<br/>autoplay, metadata cache"]
  QS --> P["QobuzPlayer<br/>reader, FLAC probe,<br/>segments, seek"]
  P --> Q
  QS --> CFG["QobuzConfig<br/>app id / secrets"]
  Q & P --> SIGN["QobuzSign<br/>request_sig (MD5)"]
  WSM -. protobuf .- PB["protobuf/<br/>qconnect_*.proto"]
```

## Files

| File | |
|---|---|
| `QobuzStream.h/.cpp` | The source: zeroconf endpoints, tokens, Qobuz Connect messages (`WSDecodeMessage`), `StreamBase` control API (pause, seek, next / previous, shuffle and repeat as controller commands so all apps stay in sync, volume, queue). |
| `QobuzQueue.h`, `QubuzQueue.cpp` | Mirror of the Qobuz Connect queue (incl. shuffle order and autoplay), track metadata, file URLs, upcoming tracks for the UIs. |
| `QobuzPlayer.h/.cpp` | Reads the audio (whole file or segmented stream), keeps position, reports the renderer state. |
| `QobuzTrack.h` | Track / album / artist data, segment URL helper. |
| `QobuzConfig.h/.cpp` | Finds the web player's app id and secrets (bundle on play.qobuz.com); runs once, the result is stored. |
| `QobuzSign.h/.cpp` | `request_sig` for signed API calls. |
| `WsManager.h`, `WebSocketClient.h/.cpp` | WebSocket client (TLS) with keep-alive, batching and token refresh. |
| `protobuf/` | Qobuz Connect message definitions (nanopb, generated at build time). |

## Configuration

| Option | |
|---|---|
| Default audio quality | MP3 320 · FLAC CD · FLAC hi-res ≤ 96 kHz · FLAC hi-res ≤ 192 kHz (also in the settings, applied from the next track) |

Qobuz Connect can be switched off in the settings (saves RAM).
