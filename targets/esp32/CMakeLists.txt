# StreamCore32 for ESP32-S3 (+ PSRAM, VS1053).  All code lives in
# ../../components; configure with `idf.py menuconfig` -> StreamCore32.
cmake_minimum_required(VERSION 3.16)

list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(StreamCore32-esp32)
