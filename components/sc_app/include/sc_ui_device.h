#pragma once
// ============================================================================
//  sc_ui_device — runs the einkui based UI (sc_ui_app.h) on the device.
//
//  • implements scui::Backend on top of StreamManager / StreamBase and the
//    device services (device.h)
//  • owns one UI task + one mutex; touch frames, periodic updates and log
//    lines are all serialised through it
//  • e-paper refresh policy: whole-page redraws use a full refresh, single
//    elements (buttons, sliders, progress, status bar) a partial one
//
//  Without the display (CONFIG_SC32_DISPLAY_EINK off) all functions are
//  empty inlines, so callers need no #if.
// ============================================================================
#include <string>

#include "sdkconfig.h"

class Gdey027T91;
class FT6X36;
class StreamManager;

namespace scui_device {

#if CONFIG_SC32_DISPLAY_EINK

struct Deps {
  Gdey027T91* display = nullptr;
  FT6X36* touch = nullptr;  // may be null (no touch panel)
  StreamManager* streams = nullptr;
};

/** Build the UI, draw the home page and start the UI task. */
void start(const Deps& deps);

/** Thread safe notifications (from stream callbacks, web UI, ...). */
void notifyPlayback();
void notifyQueue();

/** Thread safe: dark mode on/off (applied by the UI task). */
void setDarkMode(bool on);
/** Thread safe: one full refresh of the panel (clears ghosting). */
void requestFullRefresh();

/** Thread safe: append a line to the on-device log page. */
void log(const std::string& line);

#else  // headless build

inline void notifyPlayback() {}
inline void notifyQueue() {}
inline void setDarkMode(bool) {}
inline void requestFullRefresh() {}
inline void log(const std::string&) {}

#endif

}  // namespace scui_device
