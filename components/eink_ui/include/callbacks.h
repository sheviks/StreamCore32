#pragma once
// ============================================================================
//  einkui/include/callbacks.h
//
//  Lightweight callback bundle.  Heap-allocated only when any callback is set.
//  Raw function pointers + void* ctx = 5 × 4 bytes, no std::function overhead.
// ============================================================================
#include <stdint.h>

namespace einkui {

class Element;

using TouchFn  = void (*)(void* ctx, Element* sender);
using StringFn = void (*)(void* ctx, Element* sender, const char* value);

using GestureFn = void (*)(void* ctx, Element* sender, uint8_t gestureId);

// FT6X36 gesture IDs (from datasheet):
constexpr uint8_t GESTURE_NONE       = 0x00;
constexpr uint8_t GESTURE_MOVE_UP    = 0x10;
constexpr uint8_t GESTURE_MOVE_LEFT  = 0x14;
constexpr uint8_t GESTURE_MOVE_DOWN  = 0x18;
constexpr uint8_t GESTURE_MOVE_RIGHT = 0x1C;
constexpr uint8_t GESTURE_ZOOM_IN    = 0x48;
constexpr uint8_t GESTURE_ZOOM_OUT   = 0x49;

struct Callbacks {
    void*      ctx         = nullptr;
    TouchFn    onTouchDown = nullptr;
    TouchFn    onTouchUp   = nullptr;
    TouchFn    onTouchMove = nullptr;
    StringFn   onChange    = nullptr;
    GestureFn  onGesture   = nullptr;   // gesture on this element
};

} // namespace einkui