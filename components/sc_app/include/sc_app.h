#pragma once
// ============================================================================
//  sc_app — the StreamCore32 application (streams, web UI, settings, device
//  services, optional e-paper UI).  The firmware's app_main() only calls
//  sc32_app_main().
//
//  Which parts are built is set in menuconfig (StreamCore32 -> ...).
// ============================================================================
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif
/** Start everything; does not return (the task becomes the settings writer). */
void sc32_app_main(void);
#ifdef __cplusplus
}

namespace sc32 {
#ifdef CONFIG_SC32_SPOTIFY
constexpr bool kHasSpotify = true;
#else
constexpr bool kHasSpotify = false;
#endif
#ifdef CONFIG_SC32_QOBUZ
constexpr bool kHasQobuz = true;
#else
constexpr bool kHasQobuz = false;
#endif
#ifdef CONFIG_SC32_WEBSTREAM
constexpr bool kHasRadio = true;
#else
constexpr bool kHasRadio = false;
#endif
#ifdef CONFIG_SC32_DLNA
constexpr bool kHasDlna = true;
#else
constexpr bool kHasDlna = false;
#endif
#ifdef CONFIG_SC32_SDCARD
constexpr bool kHasSdCard = true;
#else
constexpr bool kHasSdCard = false;
#endif
#ifdef CONFIG_SC32_SDFILE
constexpr bool kHasSdPlayer = true;
#else
constexpr bool kHasSdPlayer = false;
#endif
#ifdef CONFIG_SC32_DISPLAY_EINK
constexpr bool kHasDisplay = true;
#else
constexpr bool kHasDisplay = false;
#endif
#ifdef CONFIG_SC32_STATUS_LED
constexpr bool kHasStatusLed = true;
#else
constexpr bool kHasStatusLed = false;
#endif
}  // namespace sc32
#endif
