#!/bin/sh
# PC builds of the DLNA tests (no ESP-IDF needed)
C=../../components
F="-std=gnu++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -I$C/sc_dlna/include -I$C/streamcore/include"
g++ $F fake_player.cpp $C/sc_dlna/src/DlnaRenderer.cpp $C/sc_dlna/src/DlnaXml.cpp -o dlna_test -lpthread &&
g++ $F http_test.cpp $C/sc_dlna/src/DlnaHttp.cpp $C/streamcore/src/AudioFileInfo.cpp -o http_test -lpthread &&
echo built ./dlna_test ./http_test
