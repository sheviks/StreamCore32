# dlna_test — DLNA renderer tests on a PC

The protocol part of [sc_dlna](../../components/sc_dlna/README.md) is
portable C++; these tests run it on a PC.

| Test | |
|---|---|
| `dlna_test --unit` | XML / DIDL-Lite / time parsing |
| `test_dlna.py` | a real control point ([async-upnp-client](https://github.com/StevenLooman/async_upnp_client), the DLNA library of Home Assistant) against the renderer with a fake player: description, subscriptions, events, SetAVTransportURI / SetNext / Play / Pause / Seek / volume / mute, error codes |
| `test_ssdp.py` | M-SEARCH answers (ssdp:all, device, service, uuid) |
| `http_test` + `media_server.py` | the HTTP reader (`DlnaHttp`) against a local server: Range, redirects, chunked, live, dropped connections; the file probe over HTTP for FLAC / MP3 / Ogg / M4A / WAV made with ffmpeg |

```shell
pip install async-upnp-client      # + g++, ffmpeg, python3
./run_tests.sh
```
