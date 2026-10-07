#include "FT6X36.h"

FT6X36 *FT6X36::_instance = nullptr;
static const char *TAG = "i2c-touch";

FT6X36::FT6X36(int8_t intPin, i2c_bus_t *i2c_bus, uint8_t addr_7bit)
{
	if (!i2c_bus) return;

	if (CONFIG_GPIO_TOUCH_RESET >= 0) {
		gpio_reset_pin((gpio_num_t)CONFIG_GPIO_TOUCH_RESET);
		gpio_set_direction((gpio_num_t)CONFIG_GPIO_TOUCH_RESET, GPIO_MODE_OUTPUT);
		gpio_set_level((gpio_num_t)CONFIG_GPIO_TOUCH_RESET, 0);
		vTaskDelay(100 / portTICK_PERIOD_MS);
		gpio_set_level((gpio_num_t)CONFIG_GPIO_TOUCH_RESET, 1);
	}
	vTaskDelay(300 / portTICK_PERIOD_MS);

	i2c_device_init(&i2cDev_, i2c_bus, addr_7bit, 1000);
	_instance = this;

	_intPin = intPin;
	_useInterrupt = false; // decided in begin()
}

FT6X36::~FT6X36()
{
	stopTask();
	if (_intPin >= 0) {
		gpio_isr_handler_remove((gpio_num_t)_intPin);
	}
}

bool FT6X36::begin(uint8_t threshold, uint16_t width, uint16_t height, bool start_task)
{
	_touch_width = width;
	_touch_height = height;
	if (width == 0 || height == 0) {
		ESP_LOGW(TAG, "begin() did not receive width/height -> rotation mapping disabled");
	}

	uint8_t data_panel_id = 0;
	i2c_reg_read_u8(&i2cDev_, FT6X36_REG_PANEL_ID, &data_panel_id, 1);
	if (data_panel_id != FT6X36_VENDID) {
		ESP_LOGE(TAG, "FT6X36_VENDID mismatch. Received:0x%02x Expected:0x%02x", data_panel_id, FT6X36_VENDID);
		return false;
	}
	ESP_LOGI(TAG, "\tDevice ID: 0x%02x", data_panel_id);

	uint8_t chip_id = 0;
	i2c_reg_read_u8(&i2cDev_, FT6X36_REG_CHIPID, &chip_id, 1);
	if (chip_id != FT6206_CHIPID && chip_id != FT6236_CHIPID && chip_id != FT6336_CHIPID) {
		ESP_LOGE(TAG, "Unsupported chip id: 0x%02x", chip_id);
		return false;
	}
	ESP_LOGI(TAG, "\tFound touch controller with Chip ID: 0x%02x", chip_id);

	// Configure interrupt pin (optional)
	_useInterrupt = (_intPin >= 0);
	if (_useInterrupt) {
		gpio_config_t io_conf{};
		io_conf.intr_type = GPIO_INTR_NEGEDGE;
		io_conf.pin_bit_mask = 1ULL << _intPin;
		io_conf.mode = GPIO_MODE_INPUT;
		io_conf.pull_down_en = (gpio_pulldown_t)0;
		io_conf.pull_up_en = (gpio_pullup_t)1;
		gpio_config(&io_conf);

		esp_err_t isr_service = gpio_install_isr_service(0);
		if (isr_service != ESP_OK && isr_service != ESP_ERR_INVALID_STATE) {
			ESP_LOGW(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(isr_service));
		}
		gpio_isr_handler_add((gpio_num_t)_intPin, isr, this);
	}

	// Put the device into normal working mode.
	write8(FT6X36_REG_DEVICE_MODE, 0x00);
	setThreshold(threshold);

	// Use interrupt trigger mode if INT pin is connected; otherwise polling.
	setInterruptMode(_useInterrupt ? 0x01 : 0x00);

	// Active mode report period in ms. 10ms ~= 100Hz (datasheet: up to 100Hz).
	setActivePeriodMs(10);

	if (start_task) {
		if (!startTask()) {
			ESP_LOGE(TAG, "Failed to start FT6X36 task");
			return false;
		}
	}
	return true;
}

bool FT6X36::startTask(const char* name, uint32_t stack_words, UBaseType_t priority, BaseType_t core_id)
{
	if (taskHandle_ != nullptr) return true;

	BaseType_t ok = pdFAIL;
	if (core_id == tskNO_AFFINITY) {
		ok = xTaskCreate(touchTaskEntry, name, stack_words, this, priority, &taskHandle_);
	} else {
		// xTaskCreatePinnedToCore exists on ESP-IDF targets.
		ok = xTaskCreatePinnedToCore(touchTaskEntry, name, stack_words, this, priority, &taskHandle_, core_id);
	}

	if (ok != pdPASS) {
		taskHandle_ = nullptr;
		return false;
	}
	return true;
}

void FT6X36::stopTask()
{
	TaskHandle_t h = taskHandle_;
	taskHandle_ = nullptr;
	if (h) {
		vTaskDelete(h);
	}
}

void FT6X36::registerTouchHandler(void (*fn)(TPoint point, TEvent e))
{
	_touchHandler = fn;
	if (CONFIG_FT6X36_DEBUG) printf("Touch handler function registered\n");
}

void FT6X36::registerFrameHandler(void (*fn)(const TTouchFrame& frame))
{
	_frameHandler = fn;
}

uint8_t FT6X36::touched()
{
	uint8_t data_buf = 0;
	esp_err_t ret = i2c_reg_read_u8(&i2cDev_, FT6X36_REG_NUM_TOUCHES, &data_buf, 1);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Error reading from device: %s", esp_err_to_name(ret));
	}

	if (data_buf > 2) data_buf = 0;
	return data_buf;
}

void FT6X36::loop()
{
	processTouch();
}

void IRAM_ATTR FT6X36::isr(void* arg)
{
	FT6X36* self = static_cast<FT6X36*>(arg);
	if (!self) return;
	TaskHandle_t h = self->taskHandle_;
	if (!h) return;

	BaseType_t xHigherPriorityTaskWoken = pdFALSE;
	vTaskNotifyGiveFromISR(h, &xHigherPriorityTaskWoken);
	if (xHigherPriorityTaskWoken) {
		portYIELD_FROM_ISR();
	}
}

void FT6X36::touchTaskEntry(void* arg)
{
	FT6X36* self = static_cast<FT6X36*>(arg);
	if (self) {
		self->touchTaskLoop();
	}
	vTaskDelete(nullptr);
}

void FT6X36::touchTaskLoop()
{
	ESP_LOGI(TAG, "FT6X36 task started (interrupt=%d, poll=%ums)", (int)_useInterrupt, (unsigned)_pollPeriodMs);

	for (;;) {
		if (_useInterrupt) {
			// Wait for an interrupt; task notifications are counting, so bursts won't be lost.
			ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
			// Coalesce any additional queued IRQs so we do only one I2C read (latest state).
			while (ulTaskNotifyTake(pdTRUE, 0) > 0) {}
		} else {
			vTaskDelay(pdMS_TO_TICKS(_pollPeriodMs));
		}

		if (!readData()) continue;
		if (_frameHandler) {
			_frameHandler(_lastFrame);
		}
		processPrimaryPointEvent_();
	}
}

void FT6X36::processTouch()
{
	// If the dedicated task is running, do not do concurrent I2C reads from user code.
	if (taskHandle_ != nullptr) return;

	if (!readData()) return;
	if (_frameHandler) {
		_frameHandler(_lastFrame);
	}
	processPrimaryPointEvent_();
}

void FT6X36::processPrimaryPointEvent_()
{
	uint8_t n = 0;
	TRawEvent event = (TRawEvent)_touchEvent[n];
	TPoint point{_touchX[n], _touchY[n]};

	switch (event) {

		case TRawEvent::PressDown:
			_points[0] = point;
			_dragMode = false;
			_touchStartTime = esp_timer_get_time() / 1000;
			fireEvent(point, TEvent::TouchStart);
			break;

		case TRawEvent::Contact:
			if (!_dragMode &&
				(abs(lastX - _touchX[n]) <= maxDeviation || abs(lastY - _touchY[n]) <= maxDeviation) &&
				esp_timer_get_time() / 1000 - _touchStartTime > 300) {
				_dragMode = true;
				fireEvent(point, TEvent::DragStart);
#if defined(CONFIG_FT6X36_DEBUG_EVENTS) && CONFIG_FT6X36_DEBUG_EVENTS==1
				printf("EV: DragStart\n");
#endif
			} else if (_dragMode) {
				fireEvent(point, TEvent::DragMove);
#if defined(CONFIG_FT6X36_DEBUG_EVENTS) && CONFIG_FT6X36_DEBUG_EVENTS==1
				printf("EV: DragMove\n");
#endif
			}
			fireEvent(point, TEvent::TouchMove);

			_touchStartTime = esp_timer_get_time() / 1000;
			break;

		case TRawEvent::LiftUp:
			_points[9] = point;
			_touchEndTime = esp_timer_get_time() / 1000;

			fireEvent(point, TEvent::TouchEnd);
			if (_dragMode) {
				fireEvent(point, TEvent::DragEnd);
#if defined(CONFIG_FT6X36_DEBUG_EVENTS) && CONFIG_FT6X36_DEBUG_EVENTS==1
				printf("EV: DragEnd\n");
#endif
				_dragMode = false;
			}

			if (_touchEndTime - _touchStartTime <= 900) {
				fireEvent(point, TEvent::Tap);
				_points[0] = {0, 0};
				_touchStartTime = 0;

#if defined(CONFIG_FT6X36_DEBUG_EVENTS) && CONFIG_FT6X36_DEBUG_EVENTS==1
				printf("EV: Tap\n");
#endif
				_dragMode = false;
			}
			break;

		case TRawEvent::NoEvent:
#if defined(CONFIG_FT6X36_DEBUG_EVENTS) && CONFIG_FT6X36_DEBUG_EVENTS==1
			printf("EV: NoEvent\n");
#endif
			break;
	}

	// Store last event / point for drag detection.
	lastEvent = (int)event;
	lastX = _touchX[0];
	lastY = _touchY[0];
}

esp_err_t FT6X36::readBytes(uint8_t reg, uint8_t* out, size_t len)
{
	return i2c_reg_read_u8(&i2cDev_, reg, out, len);
}

esp_err_t FT6X36::write8(uint8_t reg, uint8_t val)
{
	return i2c_reg_write_u8(&i2cDev_, reg, val);
}

uint8_t FT6X36::read8(uint8_t regName)
{
	uint8_t buf = 0;
	if (readBytes(regName, &buf, 1) != ESP_OK) return 0;
	return buf;
}

void FT6X36::applyRotation(uint16_t& x, uint16_t& y)
{
	if (_touch_width == 0 || _touch_height == 0) return;
	switch (_rotation)
	{
		case 1:
			swap(x, y);
			y = _touch_width - y - 1;
			break;
		case 2:
			x = _touch_width - x - 1;
			y = _touch_height - y - 1;
			break;
		case 3:
			swap(x, y);
			x = _touch_height - x - 1;
			break;
		default:
			break;
	}
}

bool FT6X36::readFrame(TTouchFrame& out)
{
	// Read a full "working mode" frame in a single burst. The register map
	// defines all touch point data as a contiguous block starting at 0x00.
	uint8_t raw[0x0F] = {0}; // 0x00..0x0E
	esp_err_t ret = readBytes(0x00, raw, sizeof(raw));
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "I2C read failed: %s", esp_err_to_name(ret));
		return false;
	}

	out.deviceMode = raw[FT6X36_REG_DEVICE_MODE];
	out.gestureId = raw[FT6X36_REG_GESTURE_ID];
	out.touches = std::min<uint8_t>(raw[FT6X36_REG_NUM_TOUCHES] & 0x0F, 2);
	out.timestampMs = esp_timer_get_time() / 1000;

	for (uint8_t i = 0; i < 2; i++) {
		const uint8_t base = FT6X36_REG_P1_XH + (6 * i);
		const uint8_t xh = raw[base + 0];
		const uint8_t xl = raw[base + 1];
		const uint8_t yh = raw[base + 2];
		const uint8_t yl = raw[base + 3];
		const uint8_t w  = raw[base + 4];
		const uint8_t mi = raw[base + 5];

		uint16_t x = (uint16_t)(xh & 0x0F) << 8 | xl;
		uint16_t y = (uint16_t)(yh & 0x0F) << 8 | yl;
		applyRotation(x, y);

		out.p[i].x = x;
		out.p[i].y = y;
		out.p[i].event = (TRawEvent)((xh >> 6) & 0x03);
		out.p[i].id = (yh >> 4) & 0x0F;
		out.p[i].weight = w;
		out.p[i].area = (mi >> 4) & 0x0F;
	}
	return true;
}

bool FT6X36::readData(void)
{
	TTouchFrame frame{};
	if (!readFrame(frame)) return false;
	_lastFrame = frame;
	_touches = frame.touches;
	_gestureId = frame.gestureId;
	for (uint8_t i = 0; i < 2; i++) {
		_touchX[i] = frame.p[i].x;
		_touchY[i] = frame.p[i].y;
		_touchEvent[i] = (uint16_t)frame.p[i].event;
		_touchId[i] = frame.p[i].id;
		_touchWeight[i] = frame.p[i].weight;
		_touchArea[i] = frame.p[i].area;
	}

	if (CONFIG_FT6X36_DEBUG) {
		ESP_LOGI(TAG, "GEST=0x%02X touches=%u | P1: ev=%u id=%u x=%u y=%u w=%u a=%u | P2: ev=%u id=%u x=%u y=%u w=%u a=%u",
				 _gestureId, _touches,
				 (unsigned)frame.p[0].event, frame.p[0].id, frame.p[0].x, frame.p[0].y, frame.p[0].weight, frame.p[0].area,
				 (unsigned)frame.p[1].event, frame.p[1].id, frame.p[1].x, frame.p[1].y, frame.p[1].weight, frame.p[1].area);
	}
	return true;
}

void FT6X36::fireEvent(TPoint point, TEvent e)
{
	if (_touchHandler)
		_touchHandler(point, e);
}

void FT6X36::debugInfo()
{
	printf("            TH_DIFF: %d             CTRL: %d\n", read8(FT6X36_REG_FILTER_COEF), read8(FT6X36_REG_CTRL));
	printf("   TIMEENTERMONITOR: %d     PERIODACTIVE: %d\n", read8(FT6X36_REG_TIME_ENTER_MONITOR), read8(FT6X36_REG_TOUCHRATE_ACTIVE));
	printf("      PERIODMONITOR: %d     RADIAN_VALUE: %d\n", read8(FT6X36_REG_TOUCHRATE_MONITOR), read8(FT6X36_REG_RADIAN_VALUE));
	printf("  OFFSET_LEFT_RIGHT: %d   OFFSET_UP_DOWN: %d\n", read8(FT6X36_REG_OFFSET_LEFT_RIGHT), read8(FT6X36_REG_OFFSET_UP_DOWN));
	printf("DISTANCE_LEFT_RIGHT: %d DISTANCE_UP_DOWN: %d\n", read8(FT6X36_REG_DISTANCE_LEFT_RIGHT), read8(FT6X36_REG_DISTANCE_UP_DOWN));
	printf("      DISTANCE_ZOOM: %d           CIPHER: %d\n", read8(FT6X36_REG_DISTANCE_ZOOM), read8(FT6X36_REG_CHIPID));
	printf("             G_MODE: %d         PWR_MODE: %d\n", read8(FT6X36_REG_INTERRUPT_MODE), read8(FT6X36_REG_POWER_MODE));
	printf("             FIRMID: %d     FOCALTECH_ID: %d  RELEASE: %d  STATE: %d\n", read8(FT6X36_REG_FIRMWARE_VERSION), read8(FT6X36_REG_PANEL_ID), read8(FT6X36_REG_RELEASE_CODE_ID), read8(FT6X36_REG_STATE));
}

void FT6X36::dumpRegisters()
{
	ESP_LOGI(TAG, "---- FT6X36 register dump ----");
	ESP_LOGI(TAG, "DEV_MODE   (0x00): 0x%02X", read8(FT6X36_REG_DEVICE_MODE));
	ESP_LOGI(TAG, "GEST_ID    (0x01): 0x%02X", read8(FT6X36_REG_GESTURE_ID));
	ESP_LOGI(TAG, "TD_STATUS  (0x02): 0x%02X", read8(FT6X36_REG_NUM_TOUCHES));
	ESP_LOGI(TAG, "TH_GROUP   (0x80): 0x%02X", read8(FT6X36_REG_THRESHHOLD));
	ESP_LOGI(TAG, "TH_DIFF    (0x85): 0x%02X", read8(FT6X36_REG_FILTER_COEF));
	ESP_LOGI(TAG, "CTRL       (0x86): 0x%02X", read8(FT6X36_REG_CTRL));
	ESP_LOGI(TAG, "TIMEENTER  (0x87): 0x%02X", read8(FT6X36_REG_TIME_ENTER_MONITOR));
	ESP_LOGI(TAG, "PERIODACT  (0x88): 0x%02X", read8(FT6X36_REG_TOUCHRATE_ACTIVE));
	ESP_LOGI(TAG, "PERIODMON  (0x89): 0x%02X", read8(FT6X36_REG_TOUCHRATE_MONITOR));
	ESP_LOGI(TAG, "RADIAN     (0x91): 0x%02X", read8(FT6X36_REG_RADIAN_VALUE));
	ESP_LOGI(TAG, "OFF_LR     (0x92): 0x%02X", read8(FT6X36_REG_OFFSET_LEFT_RIGHT));
	ESP_LOGI(TAG, "OFF_UD     (0x93): 0x%02X", read8(FT6X36_REG_OFFSET_UP_DOWN));
	ESP_LOGI(TAG, "DIST_LR    (0x94): 0x%02X", read8(FT6X36_REG_DISTANCE_LEFT_RIGHT));
	ESP_LOGI(TAG, "DIST_UD    (0x95): 0x%02X", read8(FT6X36_REG_DISTANCE_UP_DOWN));
	ESP_LOGI(TAG, "DIST_ZOOM  (0x96): 0x%02X", read8(FT6X36_REG_DISTANCE_ZOOM));
	ESP_LOGI(TAG, "LIB_VER_H  (0xA1): 0x%02X", read8(FT6X36_REG_LIB_VERSION_H));
	ESP_LOGI(TAG, "LIB_VER_L  (0xA2): 0x%02X", read8(FT6X36_REG_LIB_VERSION_L));
	ESP_LOGI(TAG, "CHIPID     (0xA3): 0x%02X", read8(FT6X36_REG_CHIPID));
	ESP_LOGI(TAG, "G_MODE     (0xA4): 0x%02X", read8(FT6X36_REG_INTERRUPT_MODE));
	ESP_LOGI(TAG, "PWR_MODE   (0xA5): 0x%02X", read8(FT6X36_REG_POWER_MODE));
	ESP_LOGI(TAG, "FIRMID     (0xA6): 0x%02X", read8(FT6X36_REG_FIRMWARE_VERSION));
	ESP_LOGI(TAG, "PANEL_ID   (0xA8): 0x%02X", read8(FT6X36_REG_PANEL_ID));
	ESP_LOGI(TAG, "RELEASE    (0xAF): 0x%02X", read8(FT6X36_REG_RELEASE_CODE_ID));
	ESP_LOGI(TAG, "STATE      (0xBC): 0x%02X", read8(FT6X36_REG_STATE));
}

bool FT6X36::setThreshold(uint8_t threshold)
{
	return write8(FT6X36_REG_THRESHHOLD, threshold) == ESP_OK;
}

uint8_t FT6X36::getThreshold()
{
	return read8(FT6X36_REG_THRESHHOLD);
}

bool FT6X36::setInterruptMode(uint8_t g_mode)
{
	bool ok = (write8(FT6X36_REG_INTERRUPT_MODE, g_mode) == ESP_OK);
	if (ok) {
		if (g_mode == 0x00) {
			_useInterrupt = false;
		} else if (_intPin >= 0) {
			_useInterrupt = true;
		}
		// If the task is blocked waiting for an IRQ, wake it so mode changes take effect quickly.
		if (taskHandle_ != nullptr) {
			xTaskNotifyGive(taskHandle_);
		}
	}
	return ok;
}


uint8_t FT6X36::getInterruptMode()
{
	return read8(FT6X36_REG_INTERRUPT_MODE);
}

bool FT6X36::setPowerMode(uint8_t p_mode)
{
	return write8(FT6X36_REG_POWER_MODE, p_mode) == ESP_OK;
}

uint8_t FT6X36::getPowerMode()
{
	return read8(FT6X36_REG_POWER_MODE);
}

bool FT6X36::setActivePeriodMs(uint8_t periodMs)
{
	return write8(FT6X36_REG_TOUCHRATE_ACTIVE, periodMs) == ESP_OK;
}

uint8_t FT6X36::getActivePeriodMs()
{
	return read8(FT6X36_REG_TOUCHRATE_ACTIVE);
}

uint8_t FT6X36::getGestureId()
{
	return _gestureId;
}

void FT6X36::setRotation(uint8_t rotation)
{
	_rotation = rotation;
}

void FT6X36::setTouchWidth(uint16_t width)
{
	_touch_width = width;
}

void FT6X36::setTouchHeight(uint16_t height)
{
	_touch_height = height;
}

void FT6X36::setXoffset(uint16_t x_offset)
{
	(void)x_offset;
}

void FT6X36::setYoffset(uint16_t y_offset)
{
	(void)y_offset;
}
