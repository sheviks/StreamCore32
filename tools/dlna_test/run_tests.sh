#!/bin/sh
# Builds and runs all DLNA tests. Needs: g++, python3, ffmpeg,
# pip install async-upnp-client
set -e
cd "$(dirname "$0")"
./build.sh
M=$(mktemp -d)
for f in flac mp3 ogg m4a wav; do
  ffmpeg -loglevel error -f lavfi -i "sine=frequency=440:duration=30" -ac 2 -ar 44100 \
    -metadata title="Test Tone" -metadata artist="Gen" "$M/t.$f"
done
./dlna_test --unit
python3 media_server.py "$M" 8765 2>/dev/null & S=$!
./dlna_test 49152 --ssdp > /dev/null 2>&1 & R=$!
sleep 0.5
rc=0
./http_test "$M" 8765 | tail -1 || rc=1
python3 test_ssdp.py | tail -1 || rc=1
python3 test_dlna.py 49152 | tail -1 || rc=1
kill $S $R
rm -rf "$M"
exit $rc
