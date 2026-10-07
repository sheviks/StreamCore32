#include <stdint.h>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>

#include "esp_log.h"
#include "i2c_bus.h"
#include "sdkconfig.h"
#include <esp_timer.h>

#ifndef FT6X36_H
#define FT6X36_H

#ifndef CONFIG_FT6X36_DEBUG
#define CONFIG_FT6X36_DEBUG 0
#endif

// I2C Constants
#define I2C_MASTER_TX_BUF_DISABLE 0                           /*!< I2C master doesn't need buffer */
#define I2C_MASTER_RX_BUF_DISABLE 0                           /*!< I2C master doesn't need buffer */

#define ACK_CHECK_EN 0x1                        /*!< I2C master will check ack from slave*/
#define ACK_CHECK_DIS 0x0                       /*!< I2C master will not check ack from slave */
#define ACK_VAL 0x0                             /*!< I2C ack value */
#define NACK_VAL 0x1                            /*!< I2C nack value */

#define FT6X36_ADDR						0x38

#define FT6X36_REG_DEVICE_MODE			0x00
#define FT6X36_REG_GESTURE_ID			0x01
#define FT6X36_REG_NUM_TOUCHES			0x02
#define FT6X36_REG_P1_XH				0x03
#define FT6X36_REG_P1_XL				0x04
#define FT6X36_REG_P1_YH				0x05
#define FT6X36_REG_P1_YL				0x06
#define FT6X36_REG_P1_WEIGHT			0x07
#define FT6X36_REG_P1_MISC				0x08
#define FT6X36_REG_P2_XH				0x09
#define FT6X36_REG_P2_XL				0x0A
#define FT6X36_REG_P2_YH				0x0B
#define FT6X36_REG_P2_YL				0x0C
#define FT6X36_REG_P2_WEIGHT			0x0D
#define FT6X36_REG_P2_MISC				0x0E
#define FT6X36_REG_THRESHHOLD			0x80
#define FT6X36_REG_FILTER_COEF			0x85
#define FT6X36_REG_CTRL					0x86
#define FT6X36_REG_TIME_ENTER_MONITOR	0x87
#define FT6X36_REG_TOUCHRATE_ACTIVE		0x88
#define FT6X36_REG_TOUCHRATE_MONITOR	0x89 // value in ms
#define FT6X36_REG_RADIAN_VALUE			0x91
#define FT6X36_REG_OFFSET_LEFT_RIGHT	0x92
#define FT6X36_REG_OFFSET_UP_DOWN		0x93
#define FT6X36_REG_DISTANCE_LEFT_RIGHT	0x94
#define FT6X36_REG_DISTANCE_UP_DOWN		0x95
#define FT6X36_REG_DISTANCE_ZOOM		0x96
#define FT6X36_REG_LIB_VERSION_H		0xA1
#define FT6X36_REG_LIB_VERSION_L		0xA2
#define FT6X36_REG_CHIPID				0xA3
#define FT6X36_REG_INTERRUPT_MODE		0xA4
#define FT6X36_REG_POWER_MODE			0xA5
#define FT6X36_REG_FIRMWARE_VERSION		0xA6
#define FT6X36_REG_PANEL_ID				0xA8
#define FT6X36_REG_RELEASE_CODE_ID     0xAF
#define FT6X36_REG_STATE				0xBC

#define FT6X36_PMODE_ACTIVE				0x00
#define FT6X36_PMODE_MONITOR			0x01
#define FT6X36_PMODE_STANDBY			0x02
#define FT6X36_PMODE_HIBERNATE			0x03

/* Possible values returned by FT6X36_GEST_ID_REG */
#define FT6X36_GEST_ID_NO_GESTURE       0x00
#define FT6X36_GEST_ID_MOVE_UP          0x10
#define FT6X36_GEST_ID_MOVE_RIGHT       0x14
#define FT6X36_GEST_ID_MOVE_DOWN        0x18
#define FT6X36_GEST_ID_MOVE_LEFT        0x1C
#define FT6X36_GEST_ID_ZOOM_IN          0x48
#define FT6X36_GEST_ID_ZOOM_OUT         0x49

#define FT6X36_VENDID					0x11
#define FT6206_CHIPID					0x06
#define FT6236_CHIPID					0x36
#define FT6336_CHIPID					0x64

#define FT6X36_DEFAULT_THRESHOLD		22

// From: https://github.com/lvgl/lv_port_esp32/blob/master/components/lvgl_esp32_drivers/lvgl_touch/ft6x36.h
#define FT6X36_MSB_MASK                 0x0F
#define FT6X36_LSB_MASK                 0xFF

enum class  TRawEvent : uint8_t
{
	PressDown,
	LiftUp,
	Contact,
	NoEvent
};

enum class TEvent
{
	None,
	TouchStart,
	TouchMove,
	TouchEnd,
	Tap,
	DragStart,
	DragMove,
	DragEnd
};

struct TPoint
{
	uint16_t x;
	uint16_t y;
	/**
	 * This is being used in the original library but I'm not using it in this implementation
	 */
	bool aboutEqual(const TPoint point)
	{
		return abs(x - point.x) <= 5 && abs(y - point.y) <= 5;
	}
};

// Extended point information as described in the FT6X36 register map:
// - Event flag: bits[7:6] of Pn_XH
// - Touch ID:   bits[7:4] of Pn_YH
// - Weight:     Pn_WEIGHT
// - Area:       bits[7:4] of Pn_MISC
struct TTouchPoint
{
	uint16_t x = 0;
	uint16_t y = 0;
	TRawEvent event = TRawEvent::NoEvent;
	uint8_t id = 0x0F;       // 0x0F means invalid per datasheet
	uint8_t weight = 0;      // "pressure" in datasheet
	uint8_t area = 0;        // 4-bit area value
};

struct TTouchFrame
{
	uint8_t deviceMode = 0;
	uint8_t gestureId = 0;
	uint8_t touches = 0;
	TTouchPoint p[2];
	uint32_t timestampMs = 0;
};

class FT6X36
{
	static void IRAM_ATTR isr(void* arg);
	static void touchTaskEntry(void* arg);
public:
	FT6X36(int8_t intPin, i2c_bus_t *i2c_bus, uint8_t addr_7bit = FT6X36_ADDR);
	~FT6X36();
	// If start_task==true the driver creates a dedicated FreeRTOS task and waits for IRQs via task notifications.
	// If intPin < 0 the task will fall back to polling at _pollPeriodMs.
	bool begin(uint8_t threshold = FT6X36_DEFAULT_THRESHOLD, uint16_t width = 0, uint16_t height = 0, bool start_task = true);
	// Create / stop dedicated task (task notifications). Use this if you want custom stack/priority/core.
	bool startTask(const char* name = "ft6x36", uint32_t stack_words = 8 * 1024, UBaseType_t priority = 5, BaseType_t core_id = tskNO_AFFINITY);
	void stopTask();
	bool isTaskRunning() const { return taskHandle_ != nullptr; }
	// Polling period when no interrupt pin is present (or when using polling mode).
	void setPollPeriodMs(uint32_t ms) { _pollPeriodMs = (ms == 0) ? 1 : ms; }
	uint32_t getPollPeriodMs() const { return _pollPeriodMs; }

	void registerTouchHandler(void(*fn)(TPoint point, TEvent e));
	// Extended handler that receives full raw frame (gesture + per-point weight/area/id/event).
	void registerFrameHandler(void(*fn)(const TTouchFrame& frame));
	uint8_t touched();
	// Optional polling API. If the dedicated task is running, loop()/processTouch() do nothing.
	void loop();
	void processTouch();
	// Reads a full frame (single I2C burst). Returns true if read succeeded.
	bool readFrame(TTouchFrame& out);
	void debugInfo();
	// Dumps all known registers (useful to verify you've exercised the IC features)
	void dumpRegisters();

	// Small convenience wrappers for common configuration registers
	bool setThreshold(uint8_t threshold);
	uint8_t getThreshold();
	bool setInterruptMode(uint8_t g_mode);   // 0: polling, 1: trigger
	uint8_t getInterruptMode();
	bool setPowerMode(uint8_t p_mode);
	uint8_t getPowerMode();
	bool setActivePeriodMs(uint8_t periodMs);
	uint8_t getActivePeriodMs();
	uint8_t getGestureId();
	// Helper functions to make the touch display aware
	void setRotation(uint8_t rotation);
	void setTouchWidth(uint16_t width);
	void setTouchHeight(uint16_t height);
	// Pending implementation. How much x->touch y↓touch is placed (In case is smaller than display)
	void setXoffset(uint16_t x_offset);
	void setYoffset(uint16_t y_offset);
	// Smart template from EPD to swap x,y:
	template <typename T> static inline void
	swap(T& a, T& b)
	{
		T t = a;
		a = b;
		b = t;
	}
	void(*_touchHandler)(TPoint point, TEvent e) = nullptr;

private:
	void touchTaskLoop();
	bool readData(void);
	void processPrimaryPointEvent_();
	void fireEvent(TPoint point, TEvent e);
	uint8_t read8(uint8_t regName);
	esp_err_t readBytes(uint8_t reg, uint8_t* out, size_t len);
	esp_err_t write8(uint8_t reg, uint8_t val);
	void applyRotation(uint16_t& x, uint16_t& y);
	static FT6X36 * _instance;
	uint8_t _intPin = 0;
	bool _useInterrupt = false;
	TaskHandle_t taskHandle_ = nullptr;
	uint32_t _pollPeriodMs = 10;
	// Make touch rotation aware:
	uint8_t _rotation = 0;
	uint16_t _touch_width = 0;
	uint16_t _touch_height = 0;
	uint8_t _touches = 0;
	uint16_t _touchX[2]{0,0}, _touchY[2]{0,0}, _touchEvent[2]{0,0};
	uint8_t _touchId[2] = {0x0F, 0x0F};
	uint8_t _touchWeight[2] = {0, 0};
	uint8_t _touchArea[2] = {0, 0};
	uint8_t _gestureId = 0;
	TTouchFrame _lastFrame{};
	TPoint _points[10]{};
	uint8_t _pointIdx = 0;
	unsigned long _touchStartTime = 0;
	unsigned long _touchEndTime = 0;
	uint8_t lastEvent = 3; // No event
	uint16_t lastX = 0;
	uint16_t lastY = 0;
	bool _dragMode = false;
	const uint8_t maxDeviation = 5;
	i2c_device_t i2cDev_{};
	void(*_frameHandler)(const TTouchFrame& frame) = nullptr;
};

#endif // FT6X36_H
