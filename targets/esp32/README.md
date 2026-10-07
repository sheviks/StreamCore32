# targets/esp32

The ESP-IDF project. It contains no application code: `main/main.cpp` calls
`sc32_app_main()` from [sc_app](../../components/sc_app/README.md), and
`CMakeLists.txt` adds `../../components` to the component path.

| File | |
|---|---|
| `CMakeLists.txt` | project, component path |
| `main/` | `app_main()` → `sc32_app_main()` |
| `sdkconfig.defaults` | minimal configuration for an **ESP32-S3 N16R8** (16 MB flash, 8 MB octal PSRAM): PSRAM allocation, WiFi / lwIP buffers, mbedTLS, core dump to flash, ... StreamCore32's own options come from the Kconfig defaults. |
| `partitions.csv` | NVS 24 KB · 2 × 6 MB app · 3 MB storage · 128 KB core dump |

```shell
idf.py set-target esp32s3      # once
idf.py menuconfig              # StreamCore32 → ...
idf.py build flash monitor
```

Your `sdkconfig` (with WiFi password and Spotify client secret) is local and
ignored by git. After an update that changes options, delete it and run
`idf.py reconfigure` (your values have to be entered again).

Other boards: everything board specific (pins, which hardware exists) is in
menuconfig → StreamCore32 → Hardware. Other ESP32 variants need PSRAM and
probably their own `sdkconfig.defaults`.
