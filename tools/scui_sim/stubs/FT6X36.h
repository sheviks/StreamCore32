#pragma once
#include <stdint.h>
enum class TRawEvent : uint8_t { PressDown, LiftUp, Contact, NoEvent };
struct TTouchPoint { uint16_t x=0, y=0; TRawEvent event=TRawEvent::NoEvent; uint8_t id=0x0F, weight=0, area=0; };
struct TTouchFrame { uint8_t deviceMode=0, gestureId=0, touches=0; TTouchPoint p[2]; uint32_t timestampMs=0; };
