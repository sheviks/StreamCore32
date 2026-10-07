#pragma once
// ============================================================================
//  einkui/include/style.h
// ============================================================================
#include <stdint.h>

namespace einkui {

// ---- style (uint16_t) -------------------------------------------------------
constexpr uint16_t STYLE_INVERTED        = 1u << 0;
constexpr uint16_t STYLE_BORDER          = 1u << 1;
constexpr uint16_t STYLE_DISABLED        = 1u << 2;
constexpr uint16_t STYLE_LIST_ROW        = 1u << 3;   // row in a list: hairline divider at the bottom, no box
constexpr uint16_t STYLE_VERTICAL        = 1u << 4;
constexpr uint16_t STYLE_HIDE_LABEL      = 1u << 5;
constexpr uint16_t STYLE_HIDE_VALUE      = 1u << 6;
constexpr uint16_t STYLE_DISPLAY_BLOCK   = 1u << 7;
constexpr uint16_t STYLE_DISPLAY_FLEX    = 1u << 8;
constexpr uint16_t STYLE_DISPLAY_FIXED   = 1u << 9;
constexpr uint16_t STYLE_NO_FILL         = 1u << 10;
constexpr uint16_t STYLE_ROUND_CORNER    = 1u << 11;
constexpr uint16_t STYLE_DRAGGABLE       = 1u << 12;
constexpr uint16_t STYLE_FLEX_WRAP       = 1u << 13;
// justify-content  00=start  01=center  10=end  11=space-between
constexpr uint16_t STYLE_JUSTIFY_CENTER  = 1u << 14;
constexpr uint16_t STYLE_JUSTIFY_END     = 1u << 15;
constexpr uint16_t STYLE_JUSTIFY_SB      = STYLE_JUSTIFY_CENTER | STYLE_JUSTIFY_END;

// ---- style2 (uint8_t) -------------------------------------------------------
//
//  bits 0-1  ALIGN_SELF        cross-axis flex alignment of this child
//  bit  2    STYLE2_FLEX_GROW  element grows on the flex main axis
//  bits 3-4  TEXT_HALIGN       horizontal text alignment within element
//              00 = left (default)
//              01 = center
//              10 = right
//  bits 5-6  TEXT_VALIGN       vertical text alignment within element
//              00 = middle (default, matches old behaviour)
//              01 = top
//              10 = bottom
//  bit  7    STYLE2_TEXT_WRAP  (reserved, not yet implemented)

constexpr uint8_t ALIGN_SELF_MASK    = 0x03;
constexpr uint8_t ALIGN_SELF_START   = 0;
constexpr uint8_t ALIGN_SELF_CENTER  = 1;
constexpr uint8_t ALIGN_SELF_END     = 2;
constexpr uint8_t ALIGN_SELF_STRETCH = 3;

constexpr uint8_t STYLE2_FLEX_GROW   = 1u << 2;

// Horizontal alignment (bits 3-4)
constexpr uint8_t TEXT_HALIGN_MASK   = 0x03u << 3;  // 0b00011000
constexpr uint8_t TEXT_HALIGN_LEFT   = 0x00u << 3;  // default
constexpr uint8_t TEXT_HALIGN_CENTER = 0x01u << 3;
constexpr uint8_t TEXT_HALIGN_RIGHT  = 0x02u << 3;

// Vertical alignment (bits 5-6)
constexpr uint8_t TEXT_VALIGN_MASK   = 0x03u << 5;  // 0b01100000
constexpr uint8_t TEXT_VALIGN_MIDDLE = 0x00u << 5;  // default
constexpr uint8_t TEXT_VALIGN_TOP    = 0x01u << 5;
constexpr uint8_t TEXT_VALIGN_BOTTOM = 0x02u << 5;

constexpr uint8_t STYLE2_TEXT_WRAP   = 1u << 7;

// Back-compat aliases (old names still compile)
constexpr uint8_t STYLE2_TEXT_CENTER = TEXT_HALIGN_CENTER;

// ---- text roles (Element::setTextSize) ---------------------------------------
//  With the default theme each role maps to a proportional Inter font.
//  Without fonts (EINKUI_NO_FONTS) they fall back to the classic GFX font sizes,
//  so old code using setTextSize(1)/setTextSize(2) keeps working.
constexpr uint8_t TEXT_CAPTION = 0;   // small secondary text
constexpr uint8_t TEXT_BODY    = 1;   // default
constexpr uint8_t TEXT_TITLE   = 2;   // page / section titles
constexpr uint8_t TEXT_DISPLAY = 3;   // big hero text (track title, clock …)
constexpr uint8_t TEXT_BOLD    = 4;   // emphasised body text
constexpr uint8_t TEXT_MONO    = 5;   // logs, code
constexpr uint8_t TEXT_ROLES   = 6;

} // namespace einkui