// Desktop preview + smoke test of the StreamCore32 e-paper UI (sc_ui_app.h).
//   ./build.sh && ./scui_sim out && python3 topng.py out 2
// Renders every page into out/*.pgm and drives the UI with simulated touches.
#include "FT6X36.h"
#include "fbdisplay.h"
#include "sc_ui_app.h"
#include "sim_backend.h"

#include <cassert>
#include <functional>

FBDisplay display(176, 264);

namespace scui {
void epdFullUpdate(void* d) {
  fullRefreshRequest().store(false);
  static_cast<FBDisplay*>(d)->update();
}
void epdWindowUpdate(void* d, int16_t x, int16_t y, int16_t w, int16_t h) {
  static_cast<FBDisplay*>(d)->updateWindow(x, y, w, h);
}

struct AppTestAccess {
  static PlayerPage* player(App& a) { return a.player_; }
  static Button* shuffle(App& a) { return a.shuffleBtn_; }
  static Button* repeat(App& a) { return a.repeatBtn_; }
  static Button* queueBtn(App& a) { return a.queueBtn_; }
  static Slider* bass(App& a) { return a.bassAmp_; }
  static Keyboard* kb(App& a) { return a.kb_; }
  static std::string ssid(App& a) { return a.ssid_; }
};
}  // namespace scui

using namespace einkui;
using A = scui::AppTestAccess;

static UI ui;
static DrawCtx ctx;
static sim::Backend be;
static scui::App* app;
static const char* OUT = "out";
static int fails = 0;

static void save(const char* n) {
  char p[256];
  snprintf(p, sizeof p, "%s/%s.pgm", OUT, n);
  display.save(p);
}
static void frame(int x, int y, int ev) {
  TTouchFrame f;
  f.touches = ev == 1 ? 0 : 1;
  f.p[0].x = uint16_t(x);
  f.p[0].y = uint16_t(y);
  f.p[0].event = (TRawEvent)ev;
  app->handleTouch(f);
}
static void tapXY(int x, int y) {
  frame(x, y, 0);
  frame(x, y, 1);
}
static void tap(Element* e) {
  assert(e);
  tapXY(e->x + e->w / 2, e->y + e->h / 2);
}
static Element* find(Element* e, const std::string& label) {
  if (!e)
    return nullptr;
  if (e->label() == label && !(e->style() & STYLE_DISABLED))
    return e;
  if (auto* p = dynamic_cast<Parent*>(e))
    for (auto& c : p->children())
      if (auto* r = find(c.get(), label))
        return r;
  return nullptr;
}
static Element* findOnPage(const std::string& label) {
  Element* e = find(ui.activePage(), label);
  if (!e) {
    printf("  !! element '%s' not found on page '%s'\n", label.c_str(),
           ui.activePage()->label().c_str());
    fails++;
  }
  return e;
}
static void expectPage(const char* what, const std::string& want) {
  bool ok = ui.activePage()->label() == want;
  if (!ok)
    fails++;
  printf("%-46s page=%-12s %s\n", what, ui.activePage()->label().c_str(),
         ok ? "OK" : ("FAIL (want " + want + ")").c_str());
}
static void expectLog(const char* what, const std::string& want) {
  bool ok = !be.log.empty() && be.log.back() == want;
  if (!ok)
    fails++;
  printf("%-46s last=%-22s %s\n", what, be.log.empty() ? "-" : be.log.back().c_str(),
         ok ? "OK" : ("FAIL (want " + want + ")").c_str());
}
static void run(uint32_t ms) {  // advance time, let the UI poll
  for (uint32_t t = 0; t < ms; t += 250) {
    be.nowMs += 250;
    app->tick(be.nowMs);
  }
}
static void tapLabel(const char* l) {
  if (Element* e = findOnPage(l))
    tap(e);
}

int main(int argc, char** argv) {
  if (argc > 1)
    OUT = argv[1];
  display.setRotation(0);
  ctx = makeDrawCtx(display);
  scui::App a(be, ui, ctx);
  app = &a;
  a.build();
  a.start();
  save("01_home");
  expectPage("start", "home");

  tapLabel("Player");
  expectPage("home -> Player", "Player");
  save("02_player_empty");
  tap(A::player(a)->btnBack);
  expectPage("player back", "home");

  tapLabel("&radio");
  expectPage("home -> Radio", "radio");
  save("03_radio");
  tapLabel("Radio Swiss Jazz");
  expectPage("tap station", "Player");
  expectLog("station started", "play station Radio Swiss Jazz");
  run(3000);
  save("04_player_radio");
  tap(A::player(a)->btnNext);
  expectLog("radio next", "next");
  run(1500);
  save("05_player_radio_next");
  tap(A::player(a)->btnPlay);
  expectLog("radio pause", "pause");

  a.goHome();
  tapLabel("SD card");
  expectPage("home -> SD card", "files");
  save("06_files");
  tapLabel("Music");
  tapLabel("Daft Punk - Discovery");
  save("07_files_album");
  tapLabel("03 - Digital Love.flac");
  expectPage("tap file", "Player");
  expectLog("file started", "play file /sdcard/Music/Daft Punk - Discovery/03 - Digital Love.flac");
  run(12000);
  save("08_player_sd");

  tap(A::shuffle(a));
  expectLog("shuffle", "shuffle on");
  tap(A::repeat(a));
  tap(A::repeat(a));
  expectLog("repeat one", "repeat 2");
  run(1000);
  save("09_player_modes");

  // drag the progress bar from 20 % to 60 % and lift: one seek
  {
    Slider* s = A::player(a)->progress;
    int y = s->y + s->h / 2;
    size_t before = be.log.size();
    frame(s->x + s->w / 5, y, 0);
    frame(s->x + s->w * 2 / 5, y, 2);
    frame(s->x + s->w * 3 / 5, y, 2);
    frame(s->x + s->w * 3 / 5, y, 1);
    bool one = be.log.size() == before + 1 && be.log.back().rfind("seek ", 0) == 0;
    printf("%-46s %-27s %s\n", "progress drag -> exactly one seek",
           be.log.back().c_str(), one ? "OK" : "FAIL");
    if (!one)
      fails++;
    run(1000);
    save("10_player_seek");
  }
  // volume drag: one volume command on release
  {
    Slider* v = A::player(a)->volume;
    int y = v->y + v->h / 2;
    size_t before = be.log.size();
    frame(v->x + v->w / 2, y, 0);
    frame(v->x + v->w / 3, y, 2);
    frame(v->x + v->w / 4, y, 1);
    bool one = be.log.size() == before + 1 && be.log.back().rfind("volume ", 0) == 0;
    printf("%-46s %-27s %s\n", "volume drag -> exactly one volume cmd",
           be.log.back().c_str(), one ? "OK" : "FAIL");
    if (!one)
      fails++;
  }

  tap(A::queueBtn(a));
  expectPage("queue button", "queue");
  save("11_queue");
  tapLabel("Superheroes");
  expectPage("queue item -> player", "Player");
  run(2000);
  save("12_player_after_queue");

  a.goHome();
  tapLabel("Settings");
  expectPage("home -> Settings", "Settings");
  save("13_settings");
  tapLabel("WiFi");
  expectPage("settings -> WiFi", "WiFi");
  save("14_wifi");
  tapLabel("Network");
  expectPage("WiFi -> keyboard", "kb");
  save("15_keyboard");
  {
    // type "x" and confirm with OK (bottom right key)
    Keyboard* k = A::kb(a);
    int okX = k->x + k->w - 20, okY = k->y + k->h - 12;
    tapXY(okX, okY);
    expectPage("keyboard OK", "WiFi");
  }
  tapLabel("&wifi Save & connect");
  expectLog("wifi connect", "wifi connect Guyer-Net");
  tap(find(ui.activePage(), "&ui_back"));
  expectPage("WiFi back", "Settings");
  tapLabel("Audio");
  expectPage("settings -> Audio", "Audio");
  save("16_audio");
  tapLabel("Bass & Treble");
  expectPage("Audio -> Bass & Treble", "Bass & Treble");
  {
    Slider* b = A::bass(a);
    tapXY(b->x + b->w * 2 / 3, b->y + b->h - 8);
    printf("%-46s bass=%d dB %s\n", "bass slider", be.toneV.bassDb,
           be.toneV.bassDb > 0 ? "OK" : "FAIL");
    if (be.toneV.bassDb <= 0)
      fails++;
  }
  save("17_tone");
  tap(find(ui.activePage(), "&ui_back"));
  tap(find(ui.activePage(), "&ui_back"));
  expectPage("back twice", "Settings");
  tapLabel("SD card");
  expectPage("settings -> SD card", "sdcard");
  save("18_sdcard");
  tap(find(ui.activePage(), "&ui_back"));
  tapLabel("System");
  expectPage("settings -> System", "system");
  save("19_system");
  tap(find(ui.activePage(), "&ui_back"));
  // ---- Streaming: quality cycles, service toggle -> restart hint
  tapLabel("Streaming");
  expectPage("settings -> Streaming", "Streaming");
  tapLabel("Qobuz quality");
  printf("%-46s qobuz_format=%d %s\n", "qobuz quality cycles", be.opts["qobuz_format"],
         be.opts["qobuz_format"] == 27 ? "OK" : "FAIL");
  if (be.opts["qobuz_format"] != 27) fails++;
  tapLabel("Spotify quality");
  printf("%-46s spotify_format=%d %s\n", "spotify quality cycles", be.opts["spotify_format"],
         be.opts["spotify_format"] == 2 ? "OK" : "FAIL");
  if (be.opts["spotify_format"] != 2) fails++;
  tapLabel("Qobuz Connect");
  printf("%-46s qobuz_enabled=%d restart=%d %s\n", "qobuz off", be.opts["qobuz_enabled"],
         be.restartNeeded, be.opts["qobuz_enabled"] == 0 && be.restartNeeded ? "OK" : "FAIL");
  if (!(be.opts["qobuz_enabled"] == 0 && be.restartNeeded)) fails++;
  save("24a_streaming");
  tap(find(ui.activePage(), "&ui_back"));
  // ---- Device: name via keyboard, dark mode
  tapLabel("Device");
  expectPage("settings -> Device", "Device");
  save("24b_device");
  tapLabel("Name");
  expectPage("Device -> keyboard", "kb");
  {
    Keyboard* k = A::kb(a);
    tapXY(k->x + k->w - 20, k->y + k->h - 12);  // OK
    expectPage("name keyboard OK", "Device");
  }
  tapLabel("Status LED");
  printf("%-46s led_mode=%d %s\n", "status LED mode cycles", be.opts["led_mode"],
         be.opts["led_mode"] == 0 ? "OK" : "FAIL");
  if (be.opts["led_mode"] != 0) fails++;
  tapLabel("LED brightness");
  printf("%-46s led_brightness=%d %s\n", "LED brightness steps", be.opts["led_brightness"],
         be.opts["led_brightness"] == 40 ? "OK" : "FAIL");
  if (be.opts["led_brightness"] != 40) fails++;
  save("24c_device_led");
  tapLabel("Dark mode");
  printf("%-46s dark=%d %s\n", "dark mode saved", be.opts["dark_mode"],
         be.opts["dark_mode"] == 1 ? "OK" : "FAIL");
  if (be.opts["dark_mode"] != 1) fails++;
  save("20_settings_dark");
  a.showPlayer();
  save("21_player_dark");
  a.setDarkMode(false);  // as if switched off in the web UI
  a.goHome();
  tapLabel("Settings");
  tapLabel("Device");
  tapLabel("&power Restart");
  expectLog("restart", "restart");

  a.goHome();
  for (auto& l : be.log)
    a.pushLog(l);
  tapLabel("Log");
  expectPage("home -> Log", "log");
  save("22_log");

  be.sdMounted = false;
  a.goHome();
  tapLabel("SD card");
  expectPage("SD card without card", "nosd");
  save("23_nosd");

  // Spotify-like source with a queue
  be.src = sim::Backend::Spotify;
  be.startTrack();
  a.notifyPlayback();
  a.showPlayer();
  run(30000);
  save("24_player_spotify");
  tap(A::queueBtn(a));
  save("25_queue_spotify");

  printf("\nrefreshes: full=%d partial=%d\n", display.updates, display.partials);
  printf("%s (%d failures)\n", fails ? "FAILED" : "ALL PASSED", fails);
  return fails;
}
