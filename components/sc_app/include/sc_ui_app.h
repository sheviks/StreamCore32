#pragma once
// ============================================================================
//  sc_ui_app.h — StreamCore32 user interface on einkui (eink_ui2).
//
//    Home (icon grid)
//     ├─ Player    now playing of the active stream; every StreamBase
//     │            control: play/pause, previous, next, seek (progress bar),
//     │            volume, shuffle, repeat, queue
//     ├─ Radio     saved stations (tap = play; next/prev step through them)
//     ├─ Files     SD card browser (tap a file = play the folder from there)
//     ├─ Queue     upcoming tracks of the active stream (tap = play)
//     ├─ Settings  WiFi (on-screen keyboard), Audio (volume, bass/treble),
//     │            SD card, System info, dark mode, display refresh
//     └─ Log       last log lines
//
//  Hardware independent: everything goes through scui::Backend.  The
//  App is NOT thread safe — the owner serialises build(), handleTouch(),
//  tick() and pushLog() (see sc_ui_device.cpp: one mutex + one UI task).
//  notifyPlayback() / notifyQueue() may be called from any task.
//
//  The includer provides the e-paper refresh policy:
//    void scui::epdFullUpdate(void* display);
//    void scui::epdWindowUpdate(void* display, int16_t x, int16_t y,
//                               int16_t w, int16_t h);
// ============================================================================
#include <stdint.h>

namespace scui {
void epdFullUpdate(void* display);
void epdWindowUpdate(void* display, int16_t x, int16_t y, int16_t w,
                     int16_t h);
}  // namespace scui

#ifndef GUI_EPD_UPDATE
#define GUI_EPD_UPDATE(ctx_d) ::scui::epdFullUpdate(ctx_d)
#endif
#ifndef GUI_EPD_UPDATE_WINDOW
#define GUI_EPD_UPDATE_WINDOW(ctx_d, x, y, w, h) \
  ::scui::epdWindowUpdate((ctx_d), (x), (y), (w), (h))
#endif

#include "einkui.h"
#include "elements/widgets.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "sc_ui_model.h"

namespace scui {

using namespace einkui;

// ============================================================================
//  helpers
// ============================================================================
/** Set to ask the refresh policy for one full (flashing) refresh. */
inline std::atomic<bool>& fullRefreshRequest() {
  static std::atomic<bool> f{false};
  return f;
}

inline std::string fmtTime(uint32_t ms) {
  char b[16];
  uint32_t s = ms / 1000;
  if (s >= 3600)
    snprintf(b, sizeof b, "%u:%02u:%02u", unsigned(s / 3600),
             unsigned(s / 60 % 60), unsigned(s % 60));
  else
    snprintf(b, sizeof b, "%u:%02u", unsigned(s / 60), unsigned(s % 60));
  return b;
}

inline std::string fmtBytes(uint64_t b) {
  char s[24];
  if (b >= (1ull << 30))
    snprintf(s, sizeof s, "%.1f GB", double(b) / double(1ull << 30));
  else if (b >= (1ull << 20))
    snprintf(s, sizeof s, "%.1f MB", double(b) / double(1ull << 20));
  else
    snprintf(s, sizeof s, "%u KB", unsigned(b >> 10));
  return s;
}

// "FLAC - 16-Bit / 44.1 kHz" -> "FLAC 16/44.1", "MP3 - 192 kbps / 48.0 kHz"
// -> "MP3 192k" (fits the player header)
inline std::string compactQuality(std::string q) {
  auto rep = [&q](const char* a, const char* b) {
    for (size_t p = q.find(a); p != std::string::npos; p = q.find(a, p + strlen(b)))
      q.replace(p, strlen(a), b);
  };
  rep("-Bit / ", "/");
  rep("-bit / ", "/");
  size_t cut = q.find(" / ");  // lossy: the bitrate is enough
  if (cut != std::string::npos)
    q.erase(cut);
  rep(" - ", " ");
  rep(" kbps", "k");
  rep(" kHz", "");
  return q;
}

inline std::string upper(std::string s) {
  for (auto& c : s)
    if (c >= 'a' && c <= 'z')
      c = char(c - 32);
  return s;
}

inline std::string urlHost(const std::string& url) {
  size_t a = url.find("://");
  a = a == std::string::npos ? 0 : a + 3;
  size_t b = url.find_first_of("/:?", a);
  return url.substr(a, b == std::string::npos ? std::string::npos : b - a);
}

// ============================================================================
//  TrackRow — two-line list row:   [icon]  Title ............ 3:41
//                                          Artist · Album
// ============================================================================
class TrackRow : public Element {
 public:
  static constexpr uint16_t kRowH = 34;

  TrackRow(size_t index, const std::string& title, const std::string& sub,
           const std::string& trailing, const char* icon, bool active)
      : Element(title.c_str(), STYLE_LIST_ROW, 0, kRowH),
        index_(index),
        sub_(sub),
        trailing_(trailing),
        icon_(icon ? icon : ""),
        active_(active) {
    margin_ = 0;
  }

  size_t index() const { return index_; }
  void cancelTouch() override { pressed_ = false; }

  Rect measure(DrawCtx& ctx, Rect cursor, Rect area, uint16_t pStyle) override {
    (void)ctx;
    if (style_ & STYLE_DISPLAY_FIXED)
      return Rect(x, y, w, h);
    placeAt(cursor, pStyle);
    w = resolveW(area);
    h = int16_t(height_);
    return Rect(x, y, w, h);
  }

  void draw(DrawCtx& ctx, Colors colors) override {
    if (style_ & STYLE_DISABLED)
      return;
    Colors c = resolveColors(colors);
    ctx.fillRect(ctx.d, x, y, w, h, c.bg);
    uint16_t fg = c.fg, bg = c.bg;
    if (pressed_) {
      ctx.fillRoundRect(ctx.d, x, y, w, int16_t(h - 1), ctx.th().radiusSm,
                        c.fg);
      std::swap(fg, bg);
    }
    const int16_t cy = int16_t(y + (h - 1) / 2);
    int16_t tx = int16_t(x + 4);
    const char* ic = active_ ? "ui_play" : icon_.c_str();
    if (*ic) {
      const int16_t isz = 14;
      if (active_) {  // playing entry: inverted disc behind the icon
        ctx.fillCircle(int16_t(tx + isz / 2), cy, int16_t(isz / 2 + 2), fg);
        ctx.icon(ic, int16_t(tx + isz / 2 + 1), cy, int16_t(isz - 4), bg, fg);
      } else {
        ctx.icon(ic, int16_t(tx + isz / 2), cy, isz, fg, bg);
      }
      tx = int16_t(tx + isz + 8);
    }
    int16_t right = int16_t(x + w - 4);
    if (!trailing_.empty()) {
      int16_t tw = ctx.textWidth(trailing_.c_str(), TEXT_CAPTION);
      ctx.textBox(int16_t(right - tw), y, tw, h, trailing_.c_str(),
                  TEXT_CAPTION, fg, TEXT_HALIGN_RIGHT);
      right = int16_t(right - tw - 6);
    }
    const int16_t bw = int16_t(right - tx);
    if (sub_.empty()) {
      ctx.textBox(tx, y, bw, h, label_.c_str(), TEXT_BOLD, fg);
    } else {
      DrawCtx::Metrics m1 = ctx.metrics(TEXT_BOLD), m2 = ctx.metrics(TEXT_CAPTION);
      int16_t total = int16_t(m1.cap + 5 + m2.cap);
      int16_t top = int16_t(y + (h - 1 - total) / 2);
      ctx.textBox(tx, top, bw, m1.cap, label_.c_str(), TEXT_BOLD, fg,
                  TEXT_HALIGN_LEFT, TEXT_VALIGN_TOP);
      ctx.textBox(tx, int16_t(top + m1.cap + 5), bw, m2.cap, sub_.c_str(),
                  TEXT_CAPTION, fg, TEXT_HALIGN_LEFT, TEXT_VALIGN_TOP);
    }
    if (!pressed_)
      drawRowDivider(ctx, c);
  }

  bool onTouch(DrawCtx&, const TTouchFrame& tf,
               std::shared_ptr<Element>& focused) override {
    if (!hasCb())
      return false;
    uint8_t ev = (uint8_t)tf.p[0].event;
    if (ev == 0 && touchExpand())
      return false;  // rows are stacked edge to edge
    if (ev == 0 && hitTest(tf.p[0].x, tf.p[0].y)) {
      focused = std::shared_ptr<Element>(this, [](Element*) {});
      pressed_ = true;
      return true;
    }
    if (ev == 1 && focused.get() == this) {
      pressed_ = false;
      focused = nullptr;
      if (cb_->onTouchUp && releaseHit(tf.p[0].x, tf.p[0].y))
        cb_->onTouchUp(cb_->ctx, this);
      return true;
    }
    return false;
  }

 private:
  size_t index_;
  std::string sub_, trailing_, icon_;
  bool active_ = false;
  bool pressed_ = false;
};

// ============================================================================
//  ListPage — header (‹ Title  info [action]), paged TrackRow list, pager.
// ============================================================================
class ListPage {
 public:
  struct Item {
    std::string title, sub, trailing;
    const char* icon = nullptr;
    bool active = false;
  };

  std::function<void()> onBack;
  std::function<void()> onAction;          // header action button
  std::function<void(size_t)> onSelect;    // item tapped
  std::function<void()> onRefresh;         // content changed → redraw

  ListPage(const char* name, const char* title, const char* actionIcon = nullptr) {
    root_ = std::make_shared<Parent>(name, STYLE_DISPLAY_FLEX | STYLE_VERTICAL |
                                               STYLE_HIDE_LABEL);
    root_->setPadding(6).setSpacing(2);

    auto* hdr = root_->add(new Parent("hdr", STYLE_DISPLAY_FLEX)).get();
    hdr->setHeight(30).setSpacing(2).setPadding(0).setMargin(0);
    auto* back = hdr->add(new Button("&ui_back", 34, 30)).get();
    back->ghost().setIconSize(16).alignCenter();
    back->setMargin(0);
    back->cb().ctx = this;
    back->cb().onTouchUp = [](void* c, Element*) {
      auto* lp = static_cast<ListPage*>(c);
      if (lp->onBack)
        lp->onBack();
    };
    title_ = hdr->add(new TextDisplay(title)).get();
    title_->setTextSize(TEXT_TITLE).flexGrow().setHeight(30);
    title_->setPadding(2).setMargin(0);
    info_ = hdr->add(new TextDisplay("")).get();
    infoW_ = actionIcon ? 40 : 52;
    info_->setTextSize(TEXT_CAPTION).textRight().setWidth(infoW_).setHeight(30);
    info_->setPadding(2).setMargin(0);
    if (actionIcon) {
      std::string l = std::string("&") + actionIcon;
      action_ = hdr->add(new Button(l.c_str(), 30, 30)).get();
      action_->ghost().setIconSize(14).alignCenter();
      action_->setMargin(0);
      action_->cb().ctx = this;
      action_->cb().onTouchUp = [](void* c, Element*) {
        auto* lp = static_cast<ListPage*>(c);
        if (lp->onAction)
          lp->onAction();
      };
    }

    auto* la = new HookParent("list", STYLE_DISPLAY_FLEX | STYLE_VERTICAL,
                              [this](int16_t hgt) {
                                int16_t rows = std::max<int16_t>(
                                    1, int16_t(hgt / int16_t(TrackRow::kRowH)));
                                if (rows != visible_) {
                                  visible_ = rows;
                                  clampScroll();
                                  rebuild();
                                }
                              });
    list_ = la;
    root_->add(std::shared_ptr<Parent>(la));
    list_->flexGrow().setPadding(0).setSpacing(0);
    list_->setMargin(0);

    auto* foot = root_->add(new Parent("pager", STYLE_DISPLAY_FLEX)).get();
    foot->setHeight(30).setSpacing(4).setPadding(0).setMargin(0);
    up_ = foot->add(new Button("&ui_chevron_up", 44, 28)).get();
    up_->setIconSize(14);
    up_->setMargin(0);
    up_->cb().ctx = this;
    up_->cb().onTouchUp = [](void* c, Element*) {
      static_cast<ListPage*>(c)->page(-1);
    };
    pageLbl_ = foot->add(new TextDisplay("")).get();
    pageLbl_->setTextSize(TEXT_CAPTION).textCenter().flexGrow().setHeight(28);
    pageLbl_->setMargin(0);
    dn_ = foot->add(new Button("&ui_chevron_down", 44, 28)).get();
    dn_->setIconSize(14);
    dn_->setMargin(0);
    dn_->cb().ctx = this;
    dn_->cb().onTouchUp = [](void* c, Element*) {
      static_cast<ListPage*>(c)->page(+1);
    };
    setInfo("");
    rebuild();
  }

  std::shared_ptr<Parent> root() { return root_; }
  Parent* rootPtr() const { return root_.get(); }

  void setTitle(const std::string& t) { title_->setDynLabel(t); }
  // empty info text: the column collapses so the title gets the room
  void setInfo(const std::string& t) {
    info_->setDynLabel(t.empty() ? " " : t);
    if (t.empty())
      info_->setWidth(1).addStyle(STYLE_DISABLED);
    else
      info_->setWidth(infoW_).clearStyle(STYLE_DISABLED);
  }
  void setEmptyText(const std::string& t) { empty_ = t; }

  void setItems(std::vector<Item> items, bool keepScroll = false) {
    items_ = std::move(items);
    if (!keepScroll)
      scroll_ = 0;
    clampScroll();
    rebuild();
  }
  // scroll so that `index` is visible
  void reveal(size_t index) {
    if (index < items_.size() && visible_ > 0) {
      scroll_ = int16_t(index / size_t(visible_) * size_t(visible_));
      clampScroll();
      rebuild();
    }
  }
  size_t count() const { return items_.size(); }

 private:
  std::shared_ptr<Parent> root_;
  TextDisplay* title_ = nullptr;
  TextDisplay* info_ = nullptr;
  uint16_t infoW_ = 52;
  Button* action_ = nullptr;
  Parent* list_ = nullptr;
  Button* up_ = nullptr;
  Button* dn_ = nullptr;
  TextDisplay* pageLbl_ = nullptr;
  std::vector<Item> items_;
  std::string empty_ = "Nothing here";
  int16_t scroll_ = 0;
  int16_t visible_ = 5;

  void clampScroll() {
    int16_t maxS = std::max<int16_t>(0, int16_t(items_.size()) - visible_);
    if (scroll_ > maxS)
      scroll_ = maxS;
    if (scroll_ < 0)
      scroll_ = 0;
  }

  void page(int dir) {
    int16_t before = scroll_;
    scroll_ = int16_t(scroll_ + dir * visible_);
    clampScroll();
    if (scroll_ != before) {
      rebuild();
      if (onRefresh)
        onRefresh();
    }
  }

  void rebuild() {
    list_->clearChildren();
    if (items_.empty()) {
      auto* e = list_->add(new TextDisplay(empty_.c_str())).get();
      e->setHeight(40).textCenter().setTextSize(TEXT_CAPTION);
    }
    int16_t end = std::min<int16_t>(int16_t(items_.size()), int16_t(scroll_ + visible_));
    for (int16_t i = scroll_; i < end; ++i) {
      const auto& it = items_[size_t(i)];
      auto* row = list_->add(new TrackRow(size_t(i), it.title, it.sub, it.trailing,
                                          it.icon, it.active)).get();
      row->cb().ctx = this;
      row->cb().onTouchUp = [](void* c, Element* el) {
        auto* lp = static_cast<ListPage*>(c);
        if (lp->onSelect)
          lp->onSelect(static_cast<TrackRow*>(el)->index());
      };
    }
    char buf[32];
    int16_t n = int16_t(items_.size());
    if (n == 0)
      buf[0] = 0;
    else
      snprintf(buf, sizeof buf, "%d-%d of %d", scroll_ + 1,
               std::min<int16_t>(n, int16_t(scroll_ + visible_)), n);
    pageLbl_->setDynLabel(buf);
    int16_t maxS = std::max<int16_t>(0, int16_t(n - visible_));
    if (scroll_ <= 0)
      up_->addStyle(STYLE_DISABLED);
    else
      up_->clearStyle(STYLE_DISABLED);
    if (scroll_ >= maxS)
      dn_->addStyle(STYLE_DISABLED);
    else
      dn_->clearStyle(STYLE_DISABLED);
  }
};

// ============================================================================
//  App
// ============================================================================
class App {
  friend struct AppTestAccess;  // desktop simulator / tests

 public:
  // refresh cadence (ms)
  static constexpr uint32_t kPollMs = 1000;      // ask the backend
  static constexpr uint32_t kTimeRowMs = 5000;   // progress bar while playing
  static constexpr uint32_t kStatusMs = 60000;   // status bar at least
  static constexpr uint32_t kLogMs = 3000;       // log page while visible

  App(Backend& be, UI& ui, DrawCtx& ctx) : be_(be), ui_(ui), ctx_(ctx) {}

  // ---------------------------------------------------------------- build --
  void build() {
    ctx_.theme = &webTheme();  // the look of the web UI
    buildStatusBar();
    buildHome();
    buildPlayer();
    buildLists();
    buildFiles();
    buildSettings();
    buildLog();
    buildKeyboard();
    if (be_.option("dark_mode"))
      ui_.setInverted(true);
    ui_.onGesture = [this](uint8_t g) {
      if (g == GESTURE_MOVE_RIGHT && ui_.activePage() != homeRoot_)
        goHome();
    };
  }

  /** First draw (full refresh). */
  void start() {
    readStatus();
    ui_.setPage(homeRoot_);
    ui_.draw(ctx_);
  }

  // --------------------------------------------------------------- events --
  /** Thread safe: a stream reported a playback change. */
  void notifyPlayback() { playbackDirty_.store(true); }
  /** Thread safe: the queue of the active stream changed. */
  void notifyQueue() { queueDirty_.store(true); }
  bool hasPendingWork() const { return playbackDirty_.load() || queueDirty_.load(); }

  void pushLog(const std::string& line) {
    log_->push(line.c_str());
    logDirty_ = true;
  }

  void handleTouch(const TTouchFrame& f) {
    ui_.onTouchFrame(ctx_, f);
    applyPendingSliders();
  }

  void tick(uint32_t now) {
    // never redraw under the finger (a press is still being handled)
    if (ui_.focused())
      return;
    const bool force = playbackDirty_.exchange(false);
    const bool qDirty = queueDirty_.exchange(false);
    if (qDirty) {
      queueStale_ = true;
      peekStale_ = true;
    }
    if (!force && now - lastPoll_ < kPollMs)
      return;
    lastPoll_ = now;

    Parent* pg = ui_.activePage();
    bool pageDrawn = false;
    if (pg == player_->root().get()) {
      pageDrawn = updatePlayer(now, force);
    } else if (pg == queue_->rootPtr() && queueStale_) {
      loadQueue(true);
      ui_.draw(ctx_);
      pageDrawn = true;
    } else if (pg == radio_->rootPtr()) {
      if (be_.currentStation() != radioCur_) {
        loadRadio(true);
        ui_.draw(ctx_);
        pageDrawn = true;
      }
    } else if (pg == logRoot_ && logDirty_ && now - lastLogDraw_ >= kLogMs) {
      logDirty_ = false;
      lastLogDraw_ = now;
      ui_.redraw(ctx_, log_, true);  // only the text area (no full flash)
    }
    updateStatusBar(now, pageDrawn);
  }

  void goHome() { show(homeRoot_); }

  /** Dark mode changed elsewhere (web UI): apply and redraw. */
  void setDarkMode(bool on) {
    if (darkToggle_)
      darkToggle_->setValue(on);
    ui_.setInverted(on);
    ui_.draw(ctx_);
  }
  /** Redraw the current page (full refresh). */
  void redrawAll() { ui_.draw(ctx_); }
  void showPlayer() {
    applyNowPlaying(be_.nowPlaying(), true);
    show(player_->root().get());
  }


 private:
  Backend& be_;
  UI& ui_;
  DrawCtx& ctx_;

  // ---- callback plumbing (einkui uses plain function pointers + ctx) ------
  struct Fn {
    std::function<void()> f;
  };
  struct FnV {
    std::function<void(Element*, const char*)> f;
  };
  std::vector<std::unique_ptr<Fn>> fns_;
  std::vector<std::unique_ptr<FnV>> fnvs_;

  void onTap(Element* e, std::function<void()> f) {
    fns_.push_back(std::make_unique<Fn>(Fn{std::move(f)}));
    e->cb().ctx = fns_.back().get();
    e->cb().onTouchUp = [](void* c, Element*) { static_cast<Fn*>(c)->f(); };
  }
  void onValue(Element* e, std::function<void(Element*, const char*)> f) {
    fnvs_.push_back(std::make_unique<FnV>(FnV{std::move(f)}));
    e->cb().ctx = fnvs_.back().get();
    e->cb().onChange = [](void* c, Element* s, const char* v) {
      static_cast<FnV*>(c)->f(s, v);
    };
  }

  void show(Parent* p) {
    if (p == nullptr)
      return;
    ui_.setPage(p);
    ui_.draw(ctx_);
    lastTimeDraw_ = lastPoll_;
  }

  // A page with "‹  Title" header; back goes to `back` (home if null)
  Parent* pageWithHeader(const char* name, const char* title,
                         std::function<void()> back) {
    auto* p = ui_.addPage(name);
    p->setPadding(6).setSpacing(4);
    auto* hdr = p->add(new Parent("hdr", STYLE_DISPLAY_FLEX)).get();
    hdr->setHeight(30).setPadding(0).setSpacing(2).setMargin(0);
    auto* b = hdr->add(new Button("&ui_back", 34, 30)).get();
    b->ghost().setIconSize(16).alignCenter();
    b->setMargin(0);
    onTap(b, back ? std::move(back) : std::function<void()>([this] { goHome(); }));
    auto* t = hdr->add(new TextDisplay(title)).get();
    t->setTextSize(TEXT_TITLE).flexGrow().setHeight(30);
    t->setMargin(0);
    return p;
  }

  // ================================================================ status ==
  StatusBar* status_ = nullptr;
  char clock_[8] = "";
  int stWifi_ = 0, stVol_ = 0, stBatt_ = 0;
  bool stCharge_ = false, stBattOk_ = false;
  uint32_t lastStatus_ = 0;

  static int wifiLevel(int rssi) {
    if (rssi == 0)
      return 0;
    return rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1;
  }
  static int volLevel(int v) { return v <= 0 ? 0 : v < 34 ? 1 : v < 67 ? 2 : 3; }

  void buildStatusBar() {
    status_ = new StatusBar(16);
    status_->setTimeGetter([this]() -> const char* { return clock_; });
    status_->setWifiGetter([this] { return stWifi_; });
    status_->setVolumeGetter([this] { return stVol_; });
    status_->setBatteryGetter([this] { return stBatt_; });
    status_->setChargingGetter([this] { return stCharge_; });
    ui_.setStatusBar(status_);
    readStatus();
  }

  // returns true if something visible changed
  bool readStatus() {
    std::string t = be_.clockText();
    int wl = be_.wifiConnected() ? std::max(1, wifiLevel(be_.wifiRssi())) : 0;
    int vl = volLevel(lastVolume_);
    Battery b = be_.battery();
    int bp = b.available ? b.percent : 0;
    bool ch = b.available && b.milliamps > 0;
    bool changed = t != clock_ || wl != stWifi_ || vl != stVol_ ||
                   bp != stBatt_ || ch != stCharge_;
    snprintf(clock_, sizeof clock_, "%s", t.c_str());
    stWifi_ = wl;
    stVol_ = vl;
    stBatt_ = bp;
    stCharge_ = ch;
    stBattOk_ = b.available;
    return changed;
  }

  void updateStatusBar(uint32_t now, bool pageJustDrawn) {
    bool changed = readStatus();
    if (pageJustDrawn) {  // the page draw included the bar
      lastStatus_ = now;
      return;
    }
    if (changed || now - lastStatus_ >= kStatusMs) {
      lastStatus_ = now;
      if (status_->w > 0)
        ui_.redraw(ctx_, status_, true);
    }
  }

  // ================================================================== home ==
  IconMenu* home_ = nullptr;
  Parent* homeRoot_ = nullptr;

  void buildHome() {
    home_ = new IconMenu(2, 3);
    home_->setTitle(be_.deviceName().c_str());
    home_->setUIContext(&ui_, &ctx_);
    home_->add(IconMenu::IconType::MUSIC, "Player", [this] { showPlayer(); });
    if (be_.has("radio"))
      home_->add(IconMenu::IconType::VG, "Radio", [this] { showRadio(); })
          .setVG("&radio", "Radio");
    if (be_.has("sd"))
      home_->add(IconMenu::IconType::FILES, "SD card", [this] { showFiles(); });
    home_->add(IconMenu::IconType::VG, "Queue", [this] { showQueue(); })
        .setVG("&list", "Queue");
    home_->add(IconMenu::IconType::SETTINGS, "Settings",
               [this] { show(settings_->root().get()); });
    home_->add(IconMenu::IconType::DEBUG, "Log", [this] {
      logDirty_ = false;
      show(logRoot_);
    });
    auto r = home_->root();
    r->setLabel("home");
    homeRoot_ = r.get();
    ui_.addPage(r);
  }

  // ================================================================ player ==
  PlayerPage* player_ = nullptr;
  Button* shuffleBtn_ = nullptr;
  Button* repeatBtn_ = nullptr;
  QueueBar* queueBtn_ = nullptr;
  std::string peekKey_;  // what the "up next" bar shows
  bool peekStale_ = true;

  NowPlaying np_;          // last state shown
  std::string metaKey_;    // title/artist/album/caption
  uint16_t capsKey_ = 0xFFFF;
  int stateKey_ = -1;
  uint8_t lastVolume_ = 0;
  uint32_t lastTimeDraw_ = 0;
  uint32_t lastPoll_ = 0;
  uint32_t shownPos_ = 0;   // position the progress row shows
  uint32_t shownAt_ = 0;    // when it was drawn

  // slider values applied when the finger is lifted
  bool seekPending_ = false, volPending_ = false;
  float seekValue_ = 0.f;
  uint8_t volValue_ = 0;
  Slider* audioVol_ = nullptr;

  enum : unsigned {
    kPage = 1, kTransport = 2, kModes = 4, kProgress = 8, kVolume = 16,
    kProgressNow = 32, kQueue = 64
  };

  void buildPlayer() {
    player_ = new PlayerPage();
    ui_.addPage(player_->root());
    onTap(player_->btnBack, [this] { goHome(); });
    onTap(player_->btnPlay, [this] {
      bool playing = np_.state == PlayState::Playing ||
                     np_.state == PlayState::Buffering;
      be_.togglePause();
      // optimistic: the button is redrawn on release with the new icon
      np_.state = playing ? PlayState::Paused : PlayState::Playing;
      stateKey_ = int(np_.state);
      player_->setPlaying(!playing);
      playbackDirty_.store(true);
    });
    onTap(player_->btnPrev, [this] {
      be_.previous();
      playbackDirty_.store(true);
    });
    onTap(player_->btnNext, [this] {
      be_.next();
      playbackDirty_.store(true);
    });
    onValue(player_->progress, [this](Element* s, const char*) {
      seekValue_ = static_cast<Slider*>(s)->value();
      seekPending_ = true;
    });
    onValue(player_->volume, [this](Element* s, const char*) {
      volValue_ = uint8_t(lroundf(static_cast<Slider*>(s)->value()));
      volPending_ = true;
    });

    // shuffle · repeat (in the transport row) · up next bar
    shuffleBtn_ = player_->btnShuffle;
    onTap(shuffleBtn_, [this] {
      bool on = !np_.shuffle;
      if (be_.setShuffle(on)) {
        np_.shuffle = on;
        styleModes();
      }
      playbackDirty_.store(true);
    });
    repeatBtn_ = player_->btnRepeat;
    onTap(repeatBtn_, [this] {
      Repeat r = np_.repeat == Repeat::Off   ? Repeat::All
                 : np_.repeat == Repeat::All ? Repeat::One
                                             : Repeat::Off;
      if (be_.setRepeat(r)) {
        np_.repeat = r;
        styleModes();
      }
      playbackDirty_.store(true);
    });
    queueBtn_ = player_->upNext;
    onTap(queueBtn_, [this] { showQueue(); });

    applyNowPlaying(be_.nowPlaying(), true);
  }

  // off = dimmed (the web UI's low opacity), on = solid
  void styleModes() {
    shuffleBtn_->dim(!np_.shuffle);
    repeatBtn_->setLabel(np_.repeat == Repeat::One ? "&repeat 1" : "&repeat");
    repeatBtn_->dim(np_.repeat == Repeat::Off);
  }

  static void setVisible(Element* e, bool v) {
    if (v)
      e->clearStyle(STYLE_DISABLED);
    else
      e->addStyle(STYLE_DISABLED);
  }

  static uint16_t capsBits(const NowPlaying& n) {
    const Caps& c = n.caps;
    return uint16_t(c.pause | c.seek << 1 | c.next << 2 | c.previous << 3 |
                    c.shuffle << 4 | c.repeat << 5 | c.queue << 6 |
                    n.shuffle << 7 | int(n.repeat) << 8 | n.hasSource << 10);
  }

  // Copies `np` into the player page; returns what has to be redrawn.
  unsigned applyNowPlaying(const NowPlaying& np, bool force) {
    unsigned what = 0;
    // ---- texts ----
    std::string title, artist, album, src, qual;
    if (np.hasSource) {
      title = upper(np.track.title.empty() ? np.source : np.track.title);
      artist = np.track.artist;
      album = np.track.album;
      src = np.source;
      qual = compactQuality(np.quality);
    } else {
      title = "NOTHING PLAYING";
      artist = "Pick Radio or SD card";
      album = "or play from Spotify / Qobuz / DLNA";
    }
    std::string key = title + '\x1f' + artist + '\x1f' + album + '\x1f' + src + '\x1f' + qual;
    if (force || key != metaKey_) {
      metaKey_ = key;
      player_->setMeta(artist.c_str(), title.c_str(), album.c_str());
      player_->setSource(src.c_str(), qual.c_str());
      what |= kPage;
    }
    // ---- transport state ----
    int st = int(np.state);
    if (force || st != stateKey_) {
      stateKey_ = st;
      player_->setPlaying(np.state == PlayState::Playing ||
                          np.state == PlayState::Buffering);
      what |= kTransport;
    }
    // ---- capabilities / modes ----
    uint16_t ck = capsBits(np);
    if (force || ck != capsKey_) {
      capsKey_ = ck;
      setVisible(player_->btnPrev, np.caps.previous);
      setVisible(player_->btnNext, np.caps.next);
      setVisible(shuffleBtn_, np.caps.shuffle);
      setVisible(repeatBtn_, np.caps.repeat);
      setVisible(queueBtn_, np.hasSource && np.caps.queue);
      np_.shuffle = np.shuffle;
      np_.repeat = np.repeat;
      styleModes();
      what |= kTransport | kModes | kProgressNow;
    }
    // ---- progress (not while the finger is on the bar) ----
    if (ui_.focused().get() != player_->progress) {
      const uint32_t dur = np.track.durationMs;
      std::string el, tot;
      if (dur) {
        player_->setProgress(std::min(1.f, float(np.positionMs) / float(dur)));
        el = fmtTime(np.positionMs);
        tot = fmtTime(dur);
      } else {
        player_->setProgress(0.f);
        el = np.hasSource && np.positionMs ? fmtTime(np.positionMs) : "";
        tot = np.hasSource && np.state == PlayState::Playing ? "LIVE" : "";
      }
      setVisible(player_->progress, dur > 0);
      // TextDisplay falls back to its initial label for "", so use a blank
      player_->setTimes(el.empty() ? " " : el.c_str(), tot.empty() ? " " : tot.c_str());
      // a jump (seek, new track) is drawn at once, normal progress lazily
      int64_t expected = int64_t(shownPos_) +
                         (np.state == PlayState::Playing ? int64_t(lastPoll_ - shownAt_) : 0);
      if (llabs(int64_t(np.positionMs) - expected) > 3000)
        what |= kProgressNow;
      what |= kProgress;
    }
    // ---- volume ----
    if (ui_.focused().get() != player_->volume && !volPending_ &&
        (force || np.volume != lastVolume_)) {
      lastVolume_ = np.volume;
      player_->setVolume(np.volume);
      if (audioVol_)
        audioVol_->setValue(np.volume);
      what |= kVolume;
    }
    // ---- up next ----
    if (force || (what & (kPage | kModes)) || peekStale_) {
      peekStale_ = false;
      std::string peek;
      if (np.hasSource && np.caps.queue) {
        auto q = be_.queue(50);
        if (!q.empty()) {
          peek = q[0].title.empty() ? q[0].id : q[0].title;
          if (q.size() > 1)
            peek += " (+" + std::to_string(q.size() - 1) + ")";
        } else {
          peek = "-";
        }
      }
      if (peek != peekKey_ || force) {
        peekKey_ = peek.empty() ? " " : peek;
        queueBtn_->setPeek(peek);
        what |= kQueue;
      }
    }
    np_.state = np.state;
    np_.caps = np.caps;
    np_.hasSource = np.hasSource;
    np_.track = np.track;
    np_.positionMs = np.positionMs;
    return what;
  }

  void markProgressShown(uint32_t now) {
    shownPos_ = np_.positionMs;
    shownAt_ = now;
    lastTimeDraw_ = now;
  }

  // returns true if the whole page was drawn
  bool updatePlayer(uint32_t now, bool force) {
    unsigned what = applyNowPlaying(be_.nowPlaying(), false);
    if (what & kPage) {
      ui_.draw(ctx_);
      markProgressShown(now);
      return true;
    }
    if (what & kTransport)
      ui_.redraw(ctx_, player_->transportRow, true);
    if (what & kModes)
      ui_.redraw(ctx_, player_->transportRow, true);
    if (what & kQueue)
      ui_.redraw(ctx_, queueBtn_, true);
    if (what & kVolume)
      ui_.redraw(ctx_, player_->volumeRow, true);
    const bool playing = np_.state == PlayState::Playing;
    if ((what & kProgressNow) ||
        ((what & kProgress) && (force || (playing && now - lastTimeDraw_ >= kTimeRowMs)))) {
      ui_.redraw(ctx_, player_->progressRow, true);
      markProgressShown(now);
    }
    return false;
  }

  // slider values are applied once the finger is lifted (or the touch got
  // lost), so a drag does not seek / send the volume on every move
  void applyPendingSliders() {
    auto foc = ui_.focused().get();
    if (seekPending_ && foc != player_->progress) {
      seekPending_ = false;
      if (np_.track.durationMs) {
        uint32_t ms = uint32_t(seekValue_ * float(np_.track.durationMs));
        if (be_.seek(ms)) {
          np_.positionMs = ms;
          shownPos_ = ms;
          shownAt_ = lastPoll_;
        }
        playbackDirty_.store(true);
      }
    }
    if (volPending_ && foc != player_->volume && foc != audioVol_) {
      volPending_ = false;
      be_.setVolume(volValue_);
      lastVolume_ = volValue_;
      player_->setVolume(volValue_);
      if (audioVol_)
        audioVol_->setValue(volValue_);
    }
  }

  // ================================================================= lists ==
  ListPage* queue_ = nullptr;
  ListPage* radio_ = nullptr;
  int radioCur_ = -2;
  bool queueStale_ = true;

  void buildLists() {
    queue_ = new ListPage("queue", "Queue", "refresh_cw");
    queue_->setEmptyText("The queue is empty");
    queue_->onBack = [this] { showPlayer(); };
    queue_->onAction = [this] {
      loadQueue(false);
      ui_.markDirty();
    };
    queue_->onRefresh = [this] { ui_.markDirty(); };
    queue_->onSelect = [this](size_t i) {
      if (be_.playQueueItem(i)) {
        playbackDirty_.store(true);
        showPlayer();
      }
    };
    ui_.addPage(queue_->root());

    radio_ = new ListPage("radio", "Radio");
    radio_->setEmptyText("No stations - add them in the web UI");
    radio_->onBack = [this] { goHome(); };
    radio_->onRefresh = [this] { ui_.markDirty(); };
    radio_->onSelect = [this](size_t i) {
      if (be_.playStation(i)) {
        playbackDirty_.store(true);
        showPlayer();
      }
    };
    ui_.addPage(radio_->root());
  }

  void loadQueue(bool keepScroll) {
    queueStale_ = false;
    NowPlaying np = be_.nowPlaying();
    std::vector<ListPage::Item> items;
    if (np.hasSource && np.caps.queue) {
      size_t i = 0;
      for (auto& t : be_.queue(40)) {
        ListPage::Item it;
        it.title = t.title.empty() ? t.id : t.title;
        it.sub = t.artist;
        if (!t.album.empty())
          it.sub += (it.sub.empty() ? "" : " \xC2\xB7 ") + t.album;
        if (t.durationMs)
          it.trailing = fmtTime(t.durationMs);
        it.icon = np.source == "Radio" ? "radio" : "ui_note";
        items.push_back(std::move(it));
        ++i;
      }
    }
    queue_->setEmptyText(!np.hasSource ? "Nothing is playing"
                         : !np.caps.queue ? "This source has no queue"
                                          : "The queue is empty");

    queue_->setItems(std::move(items), keepScroll);
  }

  void loadRadio(bool keepScroll) {
    radioCur_ = be_.currentStation();
    std::vector<ListPage::Item> items;
    auto st = be_.stations();
    for (size_t i = 0; i < st.size(); i++) {
      ListPage::Item it;
      it.title = st[i].name;
      it.sub = urlHost(st[i].url);
      it.icon = "radio";
      it.active = int(i) == radioCur_;
      items.push_back(std::move(it));
    }
    char b[16];
    snprintf(b, sizeof b, "%u saved", unsigned(st.size()));
    radio_->setInfo(st.empty() ? "" : b);
    radio_->setItems(std::move(items), keepScroll);
    if (!keepScroll && radioCur_ >= 0)
      radio_->reveal(size_t(radioCur_));
  }

  void showQueue() {
    loadQueue(false);
    show(queue_->rootPtr());
  }
  void showRadio() {
    loadRadio(false);
    show(radio_->rootPtr());
  }

  // ================================================================= files ==
  FilePage* files_ = nullptr;
  Parent* noSd_ = nullptr;
  TextDisplay* noSdText_ = nullptr;

  void buildFiles() {
    files_ = new FilePage(be_.fileSystem(), be_.sdRoot());
    files_->setFilter({".mp3", ".mp2", ".flac", ".wav", ".ogg", ".oga",
                       ".aac", ".m4a", ".m4b", ".mp4", ".wma", ".mid", ".midi",
                       ".m3u", ".m3u8"});  // playlists play as a whole
    files_->cbCtx = this;
    files_->onExit = [](void* c, Element*) { static_cast<App*>(c)->goHome(); };
    files_->onRefresh = [this] { ui_.markDirty(); };
    files_->onFileSelected = [](void* c, Element*, const char* path) {
      auto* app = static_cast<App*>(c);
      if (app->be_.playFile(path)) {
        app->playbackDirty_.store(true);
        app->showPlayer();
      }
    };
    ui_.addPage(files_->root());

    noSd_ = pageWithHeader("nosd", "SD card", nullptr);
    noSd_->add(new Element("", STYLE_NO_FILL | STYLE_HIDE_LABEL, 0, 20));
    noSd_->add(new Icon("hard_drive", 44, 44))->alignCenter();
    noSdText_ = noSd_->add(new TextDisplay("No SD card found")).get();
    noSdText_->setTextSize(TEXT_BOLD).textCenter();
    auto* hint = noSd_->add(new TextDisplay("Insert a FAT32 card")).get();
    hint->setTextSize(TEXT_CAPTION).textCenter();
    noSd_->add(new Element("", STYLE_NO_FILL | STYLE_HIDE_LABEL, 0, 10));
    auto* retry = noSd_->add(new Button("&refresh_cw Try again", 0, 34)).get();
    retry->filled();
    onTap(retry, [this] {
      be_.sdRemount();
      showFiles();
    });
  }

  void showFiles() {
    SdInfo sd = be_.sdInfo();
    if (!sd.mounted && sd.present) {
      be_.sdRemount();
      sd = be_.sdInfo();
    }
    if (!sd.mounted) {
      noSdText_->setDynLabel(sd.present ? "SD card not readable"
                                        : "No SD card found");
      show(noSd_);
      return;
    }
    std::string cur = files_->currentPath();  // copy: navigateTo() overwrites it
    files_->navigateTo(cur.c_str());
    show(files_->root().get());
  }

  // ============================================================== settings ==
  SettingsPage* settings_ = nullptr;
  SettingsPage* wifiPg_ = nullptr;
  SettingsPage* audioPg_ = nullptr;
  SettingsPage* tonePg_ = nullptr;
  Button* wifiSsidRow_ = nullptr;
  Button* wifiPassRow_ = nullptr;
  TextDisplay* wifiStatus_ = nullptr;
  Slider* bassAmp_ = nullptr;
  Slider* bassFreq_ = nullptr;
  Slider* trebAmp_ = nullptr;
  Slider* trebFreq_ = nullptr;
  Parent* sdPage_ = nullptr;
  Parent* sdRows_ = nullptr;
  Parent* sysPage_ = nullptr;
  Parent* sysRows_ = nullptr;
  std::string ssid_, pass_;
  Tone tone_;

  void buildSettings() {
    settings_ = new SettingsPage("Settings");
    settings_->onNavigate = [this](Parent* p) { navigateSettings(p); };
    settings_->setOnBack([this] { goHome(); });

    // ---- Device: name, display, restart ----
    devicePg_ = settings_->addSubPage("Device");
    nameRow_ = devicePg_->addButton("Name", nullptr);
    nameRow_->showChevron(true);
    onTap(nameRow_, [this] { openKeyboard(KbName); });
    darkToggle_ = devicePg_->addToggle("Dark mode", be_.option("dark_mode") != 0);
    onValue(darkToggle_, [this](Element*, const char* v) {
      bool on = v[0] == '1';
      be_.setOption("dark_mode", on);
      ui_.setInverted(on);  // full redraw follows (dirty flag)
    });
    if (be_.has("led")) {
      ledModeRow_ = devicePg_->addButton("Status LED", nullptr);
      ledModeRow_->showChevron(false);
      onTap(ledModeRow_, [this] {
        cycleOption("led_mode", ledModes());
        loadDevice();
        ui_.markDirty();
      });
      ledBriRow_ = devicePg_->addButton("LED brightness", nullptr);
      ledBriRow_->showChevron(false);
      onTap(ledBriRow_, [this] {
        // next step up (wraps): 5 10 20 40 70 100 %
        static const int steps[] = {5, 10, 20, 40, 70, 100};
        int cur = be_.option("led_brightness"), next = steps[0];
        for (int s : steps)
          if (s > cur) {
            next = s;
            break;
          }
        be_.setOption("led_brightness", next);
        loadDevice();
        ui_.markDirty();
      });
    }
    auto* refresh = devicePg_->addButton("&refresh_cw Refresh display", nullptr);
    onTap(refresh, [this] {
      fullRefreshRequest().store(true);
      ui_.markDirty();
    });
    restartRow_ = devicePg_->addButton("&power Restart", nullptr);
    onTap(restartRow_, [this] { be_.restart(); });

    // ---- WiFi ----
    wifiPg_ = settings_->addSubPage("WiFi");
    wifiSsidRow_ = wifiPg_->addButton("Network", nullptr);
    wifiSsidRow_->showChevron(true);
    onTap(wifiSsidRow_, [this] { openKeyboard(KbSsid); });
    wifiPassRow_ = wifiPg_->addButton("Password", nullptr);
    wifiPassRow_->showChevron(true);
    onTap(wifiPassRow_, [this] { openKeyboard(KbPass); });
    auto* conn = wifiPg_->addButton("&wifi Save & connect", nullptr);
    onTap(conn, [this] {
      if (ssid_.empty())
        return;
      be_.wifiConnect(ssid_, pass_);
      wifiStatus_->setDynLabel("Connecting to " + ssid_ + " ...");
      ui_.markDirty();
    });
    wifiStatus_ = wifiPg_->root()->add(new TextDisplay("")).get();
    wifiStatus_->setTextSize(TEXT_CAPTION).setHeight(20).setMargin(0);
    wifiStatus_->setPadding(4);

    // ---- Audio ----
    audioPg_ = settings_->addSubPage("Audio");
    audioVol_ = audioPg_->addSlider("Volume", 0.f, 100.f, 50.f, nullptr, nullptr,
                                    1.f, "%");
    onValue(audioVol_, [this](Element* s, const char*) {
      volValue_ = uint8_t(lroundf(static_cast<Slider*>(s)->value()));
      volPending_ = true;
    });
    restoreVolToggle_ = audioPg_->addToggle("Keep volume",
                                            be_.option("restore_volume") != 0);
    onValue(restoreVolToggle_, [this](Element*, const char* v) {
      be_.setOption("restore_volume", v[0] == '1');
    });
    tonePg_ = audioPg_->addSubPage("Bass & Treble");
    bassAmp_ = tonePg_->addSlider("Bass boost", 0.f, 15.f, 0.f, nullptr, nullptr,
                                  1.f, " dB");
    bassFreq_ = tonePg_->addSlider("Bass below", 20.f, 150.f, 20.f, nullptr,
                                   nullptr, 10.f, " Hz");
    trebAmp_ = tonePg_->addSlider("Treble", -12.f, 10.5f, 0.f, nullptr, nullptr,
                                  1.5f, " dB", 1);
    trebFreq_ = tonePg_->addSlider("Treble above", 1.f, 15.f, 1.f, nullptr,
                                   nullptr, 1.f, " kHz");
    auto toneChanged = [this](Element*, const char*) {
      tone_.bassDb = int(lroundf(bassAmp_->value()));
      tone_.bassFreqHz = int(lroundf(bassFreq_->value()));
      tone_.trebleDb = trebAmp_->value();
      tone_.trebleFreqKhz = int(lroundf(trebFreq_->value()));
      be_.setTone(tone_);
      updateToneRow();
    };
    onValue(bassAmp_, toneChanged);
    onValue(bassFreq_, toneChanged);
    onValue(trebAmp_, toneChanged);
    onValue(trebFreq_, toneChanged);
    auto* flat = tonePg_->addButton("&rotate_ccw Flat", nullptr);
    onTap(flat, [this] {
      tone_ = Tone();
      be_.setTone(tone_);
      loadTone();
      ui_.markDirty();
    });

    // ---- Streaming services ----
    streamPg_ = settings_->addSubPage("Streaming");
    if (be_.has("spotify")) {
      spotifyToggle_ = streamPg_->addToggle("Spotify Connect", be_.option("spotify_enabled") != 0);
      onValue(spotifyToggle_, [this](Element*, const char* v) {
        be_.setOption("spotify_enabled", v[0] == '1');
        loadStreaming();
        ui_.markDirty();
      });
      spotifyQRow_ = streamPg_->addButton("Spotify quality", nullptr);
      spotifyQRow_->showChevron(false);
      onTap(spotifyQRow_, [this] {
        cycleOption("spotify_format", spotifyFormats());
        loadStreaming();
        ui_.markDirty();
      });
    }
    if (be_.has("qobuz")) {
      qobuzToggle_ = streamPg_->addToggle("Qobuz Connect", be_.option("qobuz_enabled") != 0);
      onValue(qobuzToggle_, [this](Element*, const char* v) {
        be_.setOption("qobuz_enabled", v[0] == '1');
        loadStreaming();
        ui_.markDirty();
      });
      qobuzQRow_ = streamPg_->addButton("Qobuz quality", nullptr);
      qobuzQRow_->showChevron(false);
      onTap(qobuzQRow_, [this] {
        cycleOption("qobuz_format", qobuzFormats());
        loadStreaming();
        ui_.markDirty();
      });
    }
    if (be_.has("dlna")) {
      dlnaToggle_ = streamPg_->addToggle("DLNA renderer", be_.option("dlna_enabled") != 0);
      onValue(dlnaToggle_, [this](Element*, const char* v) {
        be_.setOption("dlna_enabled", v[0] == '1');
        loadStreaming();
        ui_.markDirty();
      });
    }
    streamHint_ = streamPg_->root()->add(new TextDisplay("")).get();
    streamHint_->setTextSize(TEXT_CAPTION).setHeight(20).setMargin(0);
    streamHint_->setPadding(4);

    // ---- SD card / System (own pages, rebuilt when opened) ----
    if (be_.has("sd")) {
      auto* sdRow = settings_->addButton("SD card", nullptr);
      sdRow->showChevron(true);
      onTap(sdRow, [this] { showSdPage(); });
    }
    auto* sysRow = settings_->addButton("System", nullptr);
    sysRow->showChevron(true);
    onTap(sysRow, [this] { showSystemPage(); });

    settings_->registerPages(ui_);

    sdPage_ = pageWithHeader("sdcard", "SD card",
                             [this] { show(settings_->root().get()); });
    sdPage_->setSpacing(0);
    sdRows_ = sdPage_->add(new Parent("rows", STYLE_DISPLAY_FLEX | STYLE_VERTICAL)).get();
    sdRows_->setPadding(0).setSpacing(0).setMargin(0);

    sysPage_ = pageWithHeader("system", "System",
                              [this] { show(settings_->root().get()); });
    sysPage_->setSpacing(0);
    sysRows_ = sysPage_->add(new Parent("rows", STYLE_DISPLAY_FLEX | STYLE_VERTICAL)).get();
    sysRows_->setPadding(0).setSpacing(0).setMargin(0);
  }

  SettingsPage* devicePg_ = nullptr;
  SettingsPage* streamPg_ = nullptr;
  Button* nameRow_ = nullptr;
  Button* restartRow_ = nullptr;
  Button* ledModeRow_ = nullptr;
  Button* ledBriRow_ = nullptr;
  char ledBriText_[8] = "";
  Toggle* darkToggle_ = nullptr;
  Toggle* restoreVolToggle_ = nullptr;
  Toggle* spotifyToggle_ = nullptr;
  Toggle* qobuzToggle_ = nullptr;
  Toggle* dlnaToggle_ = nullptr;
  Button* spotifyQRow_ = nullptr;
  Button* qobuzQRow_ = nullptr;
  TextDisplay* streamHint_ = nullptr;
  std::string nameText_;
  std::string spotifyQText_, qobuzQText_;

  using Options = std::vector<std::pair<int, const char*>>;
  static const Options& spotifyFormats() {
    static const Options o = {{0, "96 kbps"}, {1, "160 kbps"}, {2, "320 kbps"}};
    return o;
  }
  static const Options& qobuzFormats() {
    static const Options o = {
        {5, "MP3 320"}, {6, "CD 16/44"}, {7, "Hi-Res 96k"}, {27, "Hi-Res 192k"}};
    return o;
  }
  static const Options& ledModes() {  // colors: web UI
    static const Options o = {{0, "Off"}, {1, "WiFi start"}, {2, "WiFi only"}, {3, "All"}};
    return o;
  }
  static const char* optionLabel(const Options& o, int v) {
    for (auto& e : o)
      if (e.first == v)
        return e.second;
    return "?";
  }
  void cycleOption(const char* key, const Options& o) {
    int cur = be_.option(key);
    size_t i = 0;
    while (i < o.size() && o[i].first != cur)
      i++;
    be_.setOption(key, o[(i + 1) % o.size()].first);
  }

  void loadDevice() {
    nameText_ = be_.deviceName();
    nameRow_->setTrailing(nameText_.c_str());
    darkToggle_->setValue(be_.option("dark_mode") != 0);
    if (ledModeRow_) {
      ledModeRow_->setTrailing(optionLabel(ledModes(), be_.option("led_mode")));
      snprintf(ledBriText_, sizeof ledBriText_, "%d %%", be_.option("led_brightness"));
      ledBriRow_->setTrailing(ledBriText_);
    }
    restartRow_->setTrailing(be_.restartRequired() ? "needed" : "");
    if (devicePg_->entryRow())
      devicePg_->entryRow()->setTrailing(nameText_.c_str());
    home_->setTitle(nameText_.c_str());
  }

  void loadStreaming() {
    if (spotifyToggle_) {
      spotifyToggle_->setValue(be_.option("spotify_enabled") != 0);
      spotifyQText_ = optionLabel(spotifyFormats(), be_.option("spotify_format"));
      spotifyQRow_->setTrailing(spotifyQText_.c_str());
    }
    if (dlnaToggle_)
      dlnaToggle_->setValue(be_.option("dlna_enabled") != 0);
    if (qobuzToggle_) {
      qobuzToggle_->setValue(be_.option("qobuz_enabled") != 0);
      qobuzQText_ = optionLabel(qobuzFormats(), be_.option("qobuz_format"));
      qobuzQRow_->setTrailing(qobuzQText_.c_str());
    }
    streamHint_->setDynLabel(be_.restartRequired()
                                 ? "Restart needed: Device > Restart"
                                 : "Quality applies to the next track");
  }

  void updateToneRow() {
    if (!tonePg_->entryRow())
      return;
    char b[32];
    snprintf(b, sizeof b, "%+d / %+.1f dB", tone_.bassDb, double(tone_.trebleDb));
    tonePg_->entryRow()->setTrailing(tone_.bassDb == 0 && tone_.trebleDb == 0.f
                                         ? "Flat"
                                         : b);
  }

  void loadTone() {
    tone_ = be_.tone();
    bassAmp_->setValue(float(tone_.bassDb));
    bassFreq_->setValue(float(tone_.bassFreqHz));
    trebAmp_->setValue(tone_.trebleDb);
    trebFreq_->setValue(float(tone_.trebleFreqKhz));
    updateToneRow();
  }

  void loadWifi() {
    if (ssid_.empty())
      ssid_ = be_.wifiSsid();
    wifiSsidRow_->setTrailing(ssid_.empty() ? "not set" : ssid_.c_str());
    wifiPassRow_->setTrailing(pass_.empty() ? "unchanged" : "*****");
    if (be_.wifiConnected()) {
      char b[64];
      snprintf(b, sizeof b, "Connected: %s (%d dBm)", be_.ipAddress().c_str(),
               be_.wifiRssi());
      wifiStatus_->setDynLabel(b);
    } else {
      wifiStatus_->setDynLabel("Not connected");
    }
    if (wifiPg_->entryRow())
      wifiPg_->entryRow()->setTrailing(be_.wifiConnected() ? be_.wifiSsid().c_str()
                                                           : "off");
  }

  // Settings navigation: fill in live values, then show the page
  void navigateSettings(Parent* p) {
    if (p == wifiPg_->root().get() || p == settings_->root().get())
      loadWifi();
    if (p == devicePg_->root().get() || p == settings_->root().get())
      loadDevice();
    if (p == streamPg_->root().get())
      loadStreaming();
    if (p == audioPg_->root().get())
      restoreVolToggle_->setValue(be_.option("restore_volume") != 0);
    if (p == audioPg_->root().get() || p == tonePg_->root().get()) {
      audioVol_->setValue(lastVolume_);
      loadTone();
    }
    show(p);
  }

  void addInfoRow(Parent* rows, const std::string& k, const std::string& v) {
    auto* b = rows->add(new Button(k.c_str(), 0, 26)).get();
    b->asListRow().showChevron(false).setTrailing(v.c_str()).setMargin(0);
  }

  void showSdPage() {
    sdRows_->clearChildren();
    SdInfo sd = be_.sdInfo();
    addInfoRow(sdRows_, "Status", sd.mounted   ? "Mounted"
                                  : sd.present ? "Not readable"
                                               : "No card");
    if (sd.mounted) {
      addInfoRow(sdRows_, "Card", sd.name + (sd.type.empty() ? "" : " (" + sd.type + ")"));
      addInfoRow(sdRows_, "Size", fmtBytes(sd.totalBytes));
      addInfoRow(sdRows_, "Free", fmtBytes(sd.freeBytes));
    }
    sdRows_->add(new Element("", STYLE_NO_FILL | STYLE_HIDE_LABEL, 0, 6));
    auto* browse = sdRows_->add(new Button("&folder Browse files", 0, 30)).get();
    browse->asListRow().setMargin(0);
    onTap(browse, [this] { showFiles(); });
    auto* rem = sdRows_->add(new Button("&refresh_cw Remount", 0, 30)).get();
    rem->asListRow().showChevron(false).setMargin(0);
    onTap(rem, [this] {
      be_.sdRemount();
      showSdPage();
    });
    show(sdPage_);
  }

  void showSystemPage() {
    sysRows_->clearChildren();
    for (auto& kv : be_.systemInfo())
      addInfoRow(sysRows_, kv.first, kv.second);
    show(sysPage_);
  }

  // ============================================================== keyboard ==
  enum KbTarget { KbSsid, KbPass, KbName };
  KbTarget kbTarget_ = KbSsid;
  Keyboard* kb_ = nullptr;
  Parent* kbPage_ = nullptr;

  void buildKeyboard() {
    kbPage_ = ui_.addPage("kb");
    kbPage_->setPadding(2);
    kb_ = kbPage_->add(new Keyboard()).get();
    kb_->cbCtx = this;
    kb_->onConfirm = [](void* c, Element*, const char* text) {
      auto* app = static_cast<App*>(c);
      if (app->kbTarget_ == KbName) {
        if (text && *text)
          app->be_.setDeviceName(text);
        app->loadDevice();
        app->show(app->devicePg_->root().get());
        return;
      }
      if (app->kbTarget_ == KbSsid)
        app->ssid_ = text;
      else
        app->pass_ = text;
      app->loadWifi();
      app->show(app->wifiPg_->root().get());
    };
    kb_->onCancel = [](void* c, Element*) {
      auto* app = static_cast<App*>(c);
      app->show(app->kbTarget_ == KbName ? app->devicePg_->root().get()
                                         : app->wifiPg_->root().get());
    };
  }

  void openKeyboard(KbTarget t) {
    kbTarget_ = t;
    kb_->setPrompt(t == KbSsid ? "WiFi network" : t == KbPass ? "WiFi password" : "Device name");
    kb_->setText(t == KbSsid ? ssid_ : t == KbPass ? pass_ : be_.deviceName());
    show(kbPage_);
  }

  // =================================================================== log ==
  DebugPage* log_ = nullptr;
  Parent* logRoot_ = nullptr;
  bool logDirty_ = false;
  uint32_t lastLogDraw_ = 0;

  void buildLog() {
    log_ = new DebugPage();
    logRoot_ = pageWithHeader("log", "Log", nullptr);
    logRoot_->setSpacing(0);
    logRoot_->add(std::shared_ptr<DebugPage>(log_, [](DebugPage*) {}));
    log_->setTitle("");
  }

  // ================================================================ state ==
  std::atomic<bool> playbackDirty_{true};
  std::atomic<bool> queueDirty_{false};
};

}  // namespace scui
