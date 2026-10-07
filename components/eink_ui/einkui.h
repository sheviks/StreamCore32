#pragma once
// ============================================================================
//  einkui.h  —  single include for the whole einkui library
//
//  In your project:
//    #include "einkui/einkui.h"
//    DrawCtx ctx = makeDrawCtx(myDisplay);
//    UI ui;
//    ...
// ============================================================================

// Core
#include "include/rect.h"
#include "include/style.h"
#include "include/callbacks.h"
#include "include/element.h"
#include "include/ui.h"
#include "include/grid.h"

// Elements
#include "elements/button.h"
#include "elements/toggle.h"
#include "elements/slider.h"
#include "elements/text_display.h"
#include "elements/icon.h"
#include "elements/select.h"
#include "elements/cartesian.h"
#include "elements/keyboard.h"

// Pages
#include "pages/player_page.h"
#include "pages/settings_page.h"
#include "pages/debug_page.h"
#include "pages/file_page.h"
#include "pages/menu_page.h"