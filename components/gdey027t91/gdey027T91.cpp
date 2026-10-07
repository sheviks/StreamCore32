// GOODISPLAY product https://www.good-display.com/product/432.html
#include "gdey027T91.h"
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_heap_caps.h"
#include "freertos/task.h"
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>

const epd_lut_159 Gdey027T91::lut_4_grays={
0x32, {
0x40,	0x48,	0x80,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x8,	0x48,	0x10,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x2,	0x48,	0x4,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x20,	0x48,	0x1,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
0xA,	0x19,	0x0,	0x3,	0x8,	0x0,	0x0,					
0x14,	0x1,	0x0,	0x14,	0x1,	0x0,	0x3,					
0xA,	0x3,	0x0,	0x8,	0x19,	0x0,	0x0,					
0x1,	0x0,	0x0,	0x0,	0x0,	0x0,	0x1,					
0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,					
0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,					
0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,					
0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,					
0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,					
0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,					
0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,					
0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,					
0x22,	0x22,	0x22,	0x22,	0x22,	0x22,	0x0,	0x0,	0x0,	 // -> till here	
0x22,	0x17,	0x41,	0x0,	0x32,	0x1C
},153};  // There are 159 but sends max 153 items

DRAM_ATTR const epd_init_3 Gdey027T91::GDOControl={
0x01,{(GDEY027T91_HEIGHT - 1) % 256, (GDEY027T91_HEIGHT - 1) / 256, 0x00},3
};

// Constructor
template <class F>
void Gdey027T91::_sendBytes(size_t total, F next) {
  constexpr size_t kChunk = 1024;
  static uint8_t* dma = nullptr;  // internal, DMA capable (allocated once)
  if (!dma)
    dma = (uint8_t*)heap_caps_malloc(kChunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!dma) {  // fallback: byte by byte
    for (size_t i = 0; i < total; i++)
      IO.data(next());
    return;
  }
  while (total) {
    size_t n = total < kChunk ? total : kChunk;
    for (size_t i = 0; i < n; i++)
      dma[i] = next();
    IO.data(dma, (int)n);
    total -= n;
  }
}

Gdey027T91::Gdey027T91(EpdSpi& dio): 
  Adafruit_GFX(GDEY027T91_WIDTH, GDEY027T91_HEIGHT),
  Epd(GDEY027T91_WIDTH, GDEY027T91_HEIGHT), IO(dio)
{
  printf("Gdey027T91() %d*%d\n",
  GDEY027T91_WIDTH, GDEY027T91_HEIGHT);
  mutex_ = xSemaphoreCreateMutex();
}

void Gdey027T91::initFullUpdate(){
    _wakeUp();
    //_PowerOn();
    if (debug_enabled) printf("initFullUpdate() LUT\n");
}



void Gdey027T91::initPartialUpdate(){
  _using_partial_mode = true;
  _wakeUp();
  _PowerOn();
  _setRamDataEntryMode(0x03);
  _SetRamArea(0x00, (GDEY027T91_WIDTH - 1) / 8,
              0x00, 0x00,
              (GDEY027T91_HEIGHT - 1) & 0xFF, (GDEY027T91_HEIGHT - 1) >> 8);
  _SetRamPointer(0x00, 0x00, 0x00);
  IO.cmd(0x26);
  _sendBytes(GDEY027T91_BUFFER_SIZE, [] { return (uint8_t)0xFF; });
}

//Initialize the display
void Gdey027T91::init(bool debug)
{
    debug_enabled = debug;
    if (debug_enabled) printf("Gdey027T91::init(%d)\n", debug);
    IO.init(4, debug); // 4MHz frequency

    printf("Free heap:%d\n", (int)xPortGetFreeHeapSize());
    fillScreen(EPD_WHITE);
    _mono_mode = 1;
    fillScreen(EPD_WHITE);
}

void Gdey027T91::fillScreen(uint16_t color)
{
  if (_mono_mode) {
    // 0xFF = 8 pixels black, 0x00 = 8 pix. white
    uint8_t data = (color == EPD_BLACK) ? GDEY027T91_8PIX_BLACK : GDEY027T91_8PIX_WHITE;
    for (uint16_t x = 0; x < sizeof(_mono_buffer); x++)
    {
      _mono_buffer[x] = data;
    }
  } else {
    // 4 Grays mode
    // This is to make faster black & white
    if (color == 255 || color == 0) {
      for(uint32_t i=0;i<GDEY027T91_BUFFER_SIZE;i++)
      {
        _buffer1[i] = (color == 0xFF) ? 0xFF : 0x00;
        _buffer2[i] = (color == 0xFF) ? 0xFF : 0x00;
      }
    return;
     }
   
    for (uint32_t y = 0; y < GDEY027T91_HEIGHT; y++)
    {
      for (uint32_t x = 0; x < GDEY027T91_WIDTH; x++)
      {
        drawPixel(x, y, color);
        if (x % 8 == 0)
          {
            vTaskDelay(pdMS_TO_TICKS(2));
          }
      }
    }
  }

  if (debug_enabled) printf("fillScreen(%d) _mono_buffer len:%d\n",color,sizeof(_mono_buffer));
}

// Now redefined as 4 gray mode
void Gdey027T91::_wakeUp4Gray(){
  IO.reset(10);
  IO.cmd(0x12); // SWRESET
  _waitBusy("SW reset");

  IO.cmd(0x74); //set analog block control       
	IO.data(0x54);
	IO.cmd(0x7E); //set digital block control          
	IO.data(0x3B);

	IO.cmd(0x01); //Driver output control      
	IO.data(0x07);
	IO.data(0x01);
	IO.data(0x00);

	IO.cmd(0x11); //data entry mode       
	IO.data(0x01);

	IO.cmd(0x44); //set Ram-X address start/end position   
	IO.data(0x00);
	IO.data(0x15);    //0x15-->(21+1)*8=176

	IO.cmd(0x45); //set Ram-Y address start/end position          
	IO.data(0x07);   //0x0107-->(263+1)=264
	IO.data(0x01);
	IO.data(0x00);
	IO.data(0x00); 

	IO.cmd(0x3C); //BorderWavefrom
	IO.data(0x00);

	IO.cmd(0x2C);     //VCOM Voltage
	IO.data(lut_4_grays.data[158]);    //0x1C

	IO.cmd(0x3F); //EOPQ    
	IO.data(lut_4_grays.data[153]);
	
	IO.cmd(0x03); //VGH      
	IO.data(lut_4_grays.data[154]);

	IO.cmd(0x04); //      
	IO.data(lut_4_grays.data[155]); //VSH1   
	IO.data(lut_4_grays.data[156]); //VSH2   
	IO.data(lut_4_grays.data[157]); //VSL

  // LUT init table for 4 gray. Check if it's needed!
  IO.cmd(lut_4_grays.cmd);     // boost
  IO.data(lut_4_grays.data, lut_4_grays.databytes);

  IO.cmd(0x4E);   // set RAM x address count to 0;
	IO.data(0x00);
	IO.cmd(0x4F);   // set RAM y address count to 0X199;    
	IO.data(0x07);
	IO.data(0x01);
  _waitBusy("4gray");
}

void Gdey027T91::_wakeUp() {
  IO.reset(10);
  IO.cmd(0x12); // SWRESET
  // Theoretically this display could be driven without RST pin connected
  _waitBusy("SWRESET");
  IO.cmd(0x18);
  IO.data(0x80);
  IO.cmd(0x22);   //Load Temperature and waveform setting.
  IO.data(0XB1);
  IO.cmd(0x20);
  _waitBusy("Load temp.");
  IO.cmd(0x1A); // Write to temperature register
  IO.data(0x64);    
  IO.data(0x00);  
            
  IO.cmd(0x22); // Load temperature value
  IO.data(0x91);    
  IO.cmd(0x20); 
  _waitBusy("_wake");
}

void Gdey027T91::update()
{
  xSemaphoreTake(mutex_, portMAX_DELAY);
  uint8_t xLineBytes = GDEY027T91_WIDTH / 8;
  uint8_t x1buf[xLineBytes];
  if (_mono_mode) {
    _wakeUp();
    _PowerOn();
    _setRamDataEntryMode(0x01);
    // rows bottom -> top (entry mode 0x01)
    auto frame = [&]() {
      int y = GDEY027T91_HEIGHT - 1;
      uint16_t x = 0;
      return [this, y, x, xLineBytes]() mutable -> uint8_t {
        uint32_t idx = (uint32_t)y * xLineBytes + x;
        uint8_t v = (idx < sizeof(_mono_buffer)) ? (uint8_t)~_mono_buffer[idx] : 0xFF;
        if (++x == xLineBytes) {
          x = 0;
          y--;
        }
        return v;
      };
    };
    IO.cmd(0x24);        // send framebuffer
    _sendBytes((size_t)GDEY027T91_HEIGHT * xLineBytes, frame());
    IO.cmd(0x26);        // send framebuffer
    _sendBytes((size_t)GDEY027T91_HEIGHT * xLineBytes, frame());

  } else {
    // 4 gray mode!
    _wakeUp4Gray();
    printf("buffer size: %d", sizeof(_buffer1));

    IO.cmd(0x24); // RAM1
    for (int16_t y = (int16_t)GDEY027T91_HEIGHT - 1; y >= 0; --y)
      {
        for (uint16_t x = 0; x < xLineBytes; x++)
        {
          uint32_t idx = (uint32_t)y * xLineBytes + x;  
          x1buf[x] = (idx < sizeof(_buffer1)) ? (uint8_t)~_buffer1[idx] : 0xFF;
        }
        // Flush the X line buffer to SPI
        IO.data(x1buf, sizeof(x1buf));
      }
    IO.cmd(0x26); // RAM2
    for (int16_t y = (int16_t)GDEY027T91_HEIGHT - 1; y >= 0; --y)
      {
        for (uint16_t x = 0; x < xLineBytes; x++)
        {
          uint32_t idx = (uint32_t)y * xLineBytes + x;  
          x1buf[x] = (idx < sizeof(_buffer2)) ? (uint8_t)~_buffer2[idx] : 0xFF;
        }
        // Flush the X line buffer to SPI
        IO.data(x1buf, sizeof(x1buf));
      }
  }
  uint64_t endTime = esp_timer_get_time();
  IO.cmd(0x22);
  IO.data(0xC4);
  // NOTE: Using F7 as in the GD example the display turns black into gray at the end. With C4 is fine
  IO.cmd(0x20);
  _waitBusy("_Update_Full", 1200);
  _sleep();
  _using_partial_mode = false;
  xSemaphoreGive(mutex_);
}

void Gdey027T91::_setRamDataEntryMode(uint8_t em)
{
  const uint16_t xPixelsPar = GDEY027T91_WIDTH - 1;
  const uint16_t yPixelsPar = GDEY027T91_HEIGHT - 1;
  em = gx_uint16_min(em, 0x03);
  IO.cmd(0x11);
  IO.data(em);
  switch (em)
  {
    case 0x00: // x decrease, y decrease
      _SetRamArea(xPixelsPar / 8, 0x00, yPixelsPar % 256, yPixelsPar / 256, 0x00, 0x00);  // X-source area,Y-gate area
      _SetRamPointer(xPixelsPar / 8, yPixelsPar % 256, yPixelsPar / 256); // set ram
      break;
    case 0x01: // x increase, y decrease : as in demo code
      _SetRamArea(0x00, xPixelsPar / 8, yPixelsPar % 256, yPixelsPar / 256, 0x00, 0x00);  // X-source area,Y-gate area
      _SetRamPointer(0x00, yPixelsPar % 256, yPixelsPar / 256); // set ram
      break;
    case 0x02: // x decrease, y increase
      _SetRamArea(xPixelsPar / 8, 0x00, 0x00, 0x00, yPixelsPar % 256, yPixelsPar / 256);  // X-source area,Y-gate area
      _SetRamPointer(xPixelsPar / 8, 0x00, 0x00); // set ram
      break;
    case 0x03: // x increase, y increase : normal mode
      _SetRamArea(0x00, xPixelsPar / 8, 0x00, 0x00, yPixelsPar % 256, yPixelsPar / 256);  // X-source area,Y-gate area
      _SetRamPointer(0x00, 0x00, 0x00); // set ram
      break;
  }
}

void Gdey027T91::_SetRamArea(uint8_t Xstart, uint8_t Xend, uint8_t Ystart, uint8_t Ystart1, uint8_t Yend, uint8_t Yend1)
{
  IO.cmd(0x44);
  IO.data(Xstart);
  IO.data(Xend);
  IO.cmd(0x45);
  IO.data(Ystart);
  IO.data(Ystart1);
  IO.data(Yend);
  IO.data(Yend1);
}

void Gdey027T91::_SetRamPointer(uint8_t addrX, uint8_t addrY, uint8_t addrY1)
{
  IO.cmd(0x4e);
  IO.data(addrX);
  IO.cmd(0x4f);
  IO.data(addrY);
  IO.data(addrY1);
}

void Gdey027T91::_PowerOn(void)
{
  IO.cmd(0x22);
  IO.data(0xc0);
  IO.cmd(0x20);
  _waitBusy("_PowerOn");
}


static inline uint16_t align8_down(uint16_t v) { return v & ~7u; }
static inline uint16_t align8_up(uint16_t v)   { return (v + 7u) & ~7u; }

void Gdey027T91::updateWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool using_rotation)
{
  // Partial window update is only implemented for mono buffer in this driver.
  // (Check before taking the mutex: update() takes it itself — taking it twice
  //  from the same task would dead-lock.)
  if (!_mono_mode) {
    update();
    return;
  }
  if (using_rotation) {
    // _rotate() works on unsigned values: clip the window to the logical
    // screen first so it can never wrap around to huge coordinates.
    int32_t lx0 = x, ly0 = y, lx1 = (int32_t)x + w, ly1 = (int32_t)y + h;
    if (lx0 < 0) lx0 = 0;
    if (ly0 < 0) ly0 = 0;
    if (lx1 > width())  lx1 = width();
    if (ly1 > height()) ly1 = height();
    if (lx1 <= lx0 || ly1 <= ly0) return;
    x = (uint16_t)lx0; y = (uint16_t)ly0; w = (uint16_t)(lx1 - lx0); h = (uint16_t)(ly1 - ly0);
    _rotate(x, y, w, h);
  }
  if (x >= GDEY027T91_WIDTH || y >= GDEY027T91_HEIGHT) return;
  xSemaphoreTake(mutex_, portMAX_DELAY);

  // --- Align window to byte boundary on X ---
  uint16_t x0 = align8_down(x);
  uint16_t x1 = std::min<uint16_t>(GDEY027T91_WIDTH,  x + w);
  uint16_t y1 = std::min<uint16_t>(GDEY027T91_HEIGHT, y + h);
  x1 = align8_up(x1);
  if (x1 > GDEY027T91_WIDTH) x1 = GDEY027T91_WIDTH;

  if (x1 <= x0 || y1 <= y) { xSemaphoreGive(mutex_); return; }

  const uint16_t xe = (uint16_t)(x1 - 1);
  const uint16_t ye = (uint16_t)(y1 - 1);

  const uint16_t xs_d8 = x0 / 8;
  const uint16_t xe_d8 = xe / 8;

  // --- Enter partial mode once (wake + optional RAM2 clear) ---
  if (!_using_partial_mode) {
    _using_partial_mode = true;
    _wakeUp();
    _PowerOn();
  }
  // (no hardware reset here: the controller must keep its RAM and state
  //  between partial refreshes; _wakeUp() above resets once per session)
  _setRamDataEntryMode(0x03);
  _SetRamArea(xs_d8, xe_d8, y % 256, y / 256, ye % 256, ye / 256); // X-source area,Y-gate area
  _SetRamPointer(xs_d8, y % 256, y / 256); // set ram

  IO.cmd(0x24);
  {
    uint16_t yy = y, xx = xs_d8;
    _sendBytes((size_t)(ye - y + 1) * (xe_d8 - xs_d8 + 1), [&]() -> uint8_t {
      uint32_t idx = (uint32_t)yy * (GDEY027T91_WIDTH / 8) + xx;
      uint8_t data = (idx < sizeof(_mono_buffer)) ? _mono_buffer[idx] : 0xFF;
      if (++xx > xe_d8) {
        xx = xs_d8;
        yy++;
      }
      return (uint8_t)~data;
    });
  }

  IO.cmd(0x22);
  IO.data(0xFF);     // display with DISPLAY Mode 2 (no reload LUT/temp each time)
  IO.cmd(0x20);
  // BUSY rises a moment after 0x20: wait for it first, otherwise the wait
  // below may return at once and the next RAM write lands mid-refresh
  for (int i = 0; i < 20 && gpio_get_level((gpio_num_t)CONFIG_GPIO_EINK_BUSY) == 0; i++)
    esp_rom_delay_us(100);
  _waitBusy("updateWindow");

  // The partial waveform drives every pixel from "old" (RAM 0x26) to "new"
  // (RAM 0x24).  The controller does not copy new -> old by itself, so write
  // the same window into 0x26 as well; otherwise the next partial refresh
  // starts from a stale image and pixels flip back to the previous screen.
  _SetRamArea(xs_d8, xe_d8, y % 256, y / 256, ye % 256, ye / 256);
  _SetRamPointer(xs_d8, y % 256, y / 256);
  IO.cmd(0x26);
  {
    uint16_t yy = y, xx = xs_d8;
    _sendBytes((size_t)(ye - y + 1) * (xe_d8 - xs_d8 + 1), [&]() -> uint8_t {
      uint32_t idx = (uint32_t)yy * (GDEY027T91_WIDTH / 8) + xx;
      uint8_t data = (idx < sizeof(_mono_buffer)) ? _mono_buffer[idx] : 0xFF;
      if (++xx > xe_d8) {
        xx = xs_d8;
        yy++;
      }
      return (uint8_t)~data;
    });
  }

  xSemaphoreGive(mutex_);
}


void Gdey027T91::_waitBusy(const char* message, uint16_t busy_time){
  if (debug_enabled) {
    ESP_LOGI(TAG, "_waitBusy for %s", message);
  }
  int64_t time_since_boot = esp_timer_get_time();
  // On high is busy
  if (gpio_get_level((gpio_num_t)CONFIG_GPIO_EINK_BUSY) == 1) {
  while (1){
    if (gpio_get_level((gpio_num_t)CONFIG_GPIO_EINK_BUSY) == 0) break;
    vTaskDelay(1);
    if (esp_timer_get_time()-time_since_boot>7000000)
    {
      if (debug_enabled) ESP_LOGI(TAG, "Busy Timeout");
      break;
    }
  }
  } else {
    vTaskDelay(busy_time/portTICK_PERIOD_MS); 
  }
}

void Gdey027T91::_waitBusy(const char* message){
  if (debug_enabled) {
    ESP_LOGI(TAG, "_waitBusy for %s", message);
  }
  int64_t time_since_boot = esp_timer_get_time();

  while (1){
    // On low is not busy anymore
    if (gpio_get_level((gpio_num_t)CONFIG_GPIO_EINK_BUSY) == 0) break;
    vTaskDelay(1);
    if (esp_timer_get_time()-time_since_boot>7000000)
    {
      if (debug_enabled) ESP_LOGI(TAG, "Busy Timeout");
      break;
    }
  }
}

void Gdey027T91::_sleep(){
  IO.cmd(0x22); // power off display
  IO.data(0xc3);
  IO.cmd(0x20);
  _waitBusy("power_off");
}

void Gdey027T91::_rotate(uint16_t& x, uint16_t& y, uint16_t& w, uint16_t& h)
{
  switch (getRotation())
  {
    case 1:
      swap(x, y);
      swap(w, h);
      x = GDEY027T91_WIDTH - x - w;
      break;
    case 2:
      x = GDEY027T91_WIDTH - x - w;
      y = GDEY027T91_HEIGHT - y - h;
      break;
    case 3:
      swap(x, y);
      swap(w, h);
      y = GDEY027T91_HEIGHT - y - h;
      break;
  }
}


void Gdey027T91::drawPixel(int16_t x, int16_t y, uint16_t color) {
  if ((x < 0) || (x >= width()) || (y < 0) || (y >= height())) return;

  // check rotation, move pixel around if necessary
  switch (getRotation())
  {
    case 1:
      swap(x, y);
      x = GDEY027T91_WIDTH - x - 1;
      break;
    case 2:
      x = GDEY027T91_WIDTH - x - 1;
      y = GDEY027T91_HEIGHT - y - 1;
      break;
    case 3:
      swap(x, y);
      y = GDEY027T91_HEIGHT - y - 1;
      break;
  }
  uint16_t i = x / 8 + y * GDEY027T91_WIDTH / 8;

 if (_mono_mode) {
    // This is the trick to draw colors right. Genious Jean-Marc
    if (color) {
      _mono_buffer[i] = (_mono_buffer[i] & (0xFF ^ (1 << (7 - x % 8))));
      } else {
      _mono_buffer[i] = (_mono_buffer[i] | (1 << (7 - x % 8)));
      }
 } else {
  // 4 gray mode
  uint8_t mask = 0x80 >> (x & 7);

  color >>= 6; // Color is from 0 (black) to 255 (white)
      
    switch (color)
    {
      case 1:
        // Dark gray
        _buffer1[i] = _buffer1[i] & (0xFF ^ mask);
        _buffer2[i] = _buffer2[i] | mask;
        break;
      case 2:
        // Light gray
        _buffer1[i] = _buffer1[i] | mask;
        _buffer2[i] = _buffer2[i] & (0xFF ^ mask);
        break;
      case 3:
        // White
        _buffer1[i] = _buffer1[i] & (0xFF ^ mask);
        _buffer2[i] = _buffer2[i] & (0xFF ^ mask);
        break;
      default:
        // Black
        _buffer1[i] = _buffer1[i] | mask;
        _buffer2[i] = _buffer2[i] | mask;
        break;
      }
 }
}

void Gdey027T91::setMonoMode(bool mode) {
  _mono_mode = mode;
}
