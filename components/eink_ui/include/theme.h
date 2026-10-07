#pragma once
// ============================================================================
//  einkui/include/theme.h
//
//  Visual theme: fonts per text role + a few geometry tokens.
//  Every element reads these through DrawCtx::theme, so the whole UI can be
//  re-skinned in one place:
//
//    einkui::Theme t = einkui::defaultTheme();
//    t.shadow = 0;          // flat look, no hard shadows
//    t.radius = 4;          // squarer cards
//    ctx.theme = &t;        // t must outlive ctx
//
//  webTheme(): monospaced, flat and square (the look of the web UI).
//
//  Define EINKUI_NO_FONTS before including einkui.h to drop the bundled
//  Inter fonts (~24 KB flash) and use the classic 5x7 GFX font instead.
// ============================================================================
#include <stdint.h>
#include <string>
#include "gfxfont.h"
#include "style.h"

#ifndef EINKUI_NO_FONTS
#  include "../fonts/einkui_caption.h"
#  include "../fonts/einkui_body.h"
#  include "../fonts/einkui_bold.h"
#  include "../fonts/einkui_title.h"
#  include "../fonts/einkui_display.h"
#  include "../fonts/einkui_mono.h"
#  include "../fonts/einkui_web_caption.h"
#  include "../fonts/einkui_web_body.h"
#  include "../fonts/einkui_web_bold.h"
#  include "../fonts/einkui_web_title.h"
#  include "../fonts/einkui_web_display.h"
#  include "../fonts/einkui_web_mono.h"
#endif

namespace einkui {

struct Theme {
    // Font per text role (TEXT_CAPTION … TEXT_MONO). nullptr = classic font.
    const GFXfont* font[TEXT_ROLES] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
    // Classic-font text size per role (used when font[role] == nullptr).
    uint8_t legacySize[TEXT_ROLES]  = { 1, 1, 2, 3, 1, 1 };

    uint8_t radius   = 8;   // cards, tiles, big buttons
    uint8_t radiusSm = 5;   // fields, small buttons, keys
    uint8_t shadow   = 2;   // hard drop-shadow offset in px (0 = flat)
    uint8_t rowH     = 30;  // default list-row height
    uint8_t gap      = 6;   // default spacing between blocks
    bool    upperTitles = false;  // TEXT_TITLE in capitals (page / card titles)
};

// "Hörspiele" -> "HÖRSPIELE" (ASCII and UTF-8 Latin-1 letters)
inline std::string upperText(const char* s) {
    std::string o(s ? s : "");
    for (size_t i = 0; i < o.size(); ++i) {
        unsigned char c = (unsigned char)o[i];
        if (c >= 'a' && c <= 'z') o[i] = char(c - 32);
        else if (c == 0xC3 && i + 1 < o.size()) {
            unsigned char n = (unsigned char)o[i + 1];
            if (n >= 0xA0 && n <= 0xBE && n != 0xB7) o[i + 1] = char(n - 0x20);
            ++i;
        }
    }
    return o;
}

inline const Theme& defaultTheme() {
    static Theme t = []{
        Theme th;
#ifndef EINKUI_NO_FONTS
        th.font[TEXT_CAPTION] = &einkui_caption;
        th.font[TEXT_BODY]    = &einkui_body;
        th.font[TEXT_TITLE]   = &einkui_title;
        th.font[TEXT_DISPLAY] = &einkui_display;
        th.font[TEXT_BOLD]    = &einkui_bold;
        th.font[TEXT_MONO]    = &einkui_mono;
#endif
        return th;
    }();
    return t;
}

// "Web" look: the StreamCore32 web UI on paper — monospaced type (DejaVu
// Sans Mono, the origin of Menlo / SF Mono), square corners, no shadows,
// hairlines.  ctx.theme = &einkui::webTheme();
inline const Theme& webTheme() {
    static Theme t = []{
        Theme th;
#ifndef EINKUI_NO_FONTS
        th.font[TEXT_CAPTION] = &einkui_web_caption;
        th.font[TEXT_BODY]    = &einkui_web_body;
        th.font[TEXT_TITLE]   = &einkui_web_title;
        th.font[TEXT_DISPLAY] = &einkui_web_display;
        th.font[TEXT_BOLD]    = &einkui_web_bold;
        th.font[TEXT_MONO]    = &einkui_web_mono;
#endif
        th.radius   = 0;
        th.radiusSm = 0;
        th.shadow   = 0;
        th.upperTitles = true;
        return th;
    }();
    return t;
}

} // namespace einkui
