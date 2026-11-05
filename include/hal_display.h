#pragma once
// Hardware Abstraction Layer for Waveshare ESP32-S3-Touch-LCD-4.3B & Wokwi Simulator
// CH422G IO expander, GT911 / FT6206 / Web touch controller, RGB LCD / ILI9341 SPI panel, LVGL display glue
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <lvgl.h>
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"

#define PIN_I2C_SDA 8
#define PIN_I2C_SCL 9
#define PIN_TP_INT  4
#define LCD_H_RES   800
#define LCD_V_RES   480

// ---------------- Virtual / Hardware Framebuffer & Web Touch Injection ----------------
static uint16_t* s_sim_fb = nullptr;             // 800x480 RGB565 virtual framebuffer in PSRAM (Wokwi or mirror)
static volatile int  s_web_tx = -1;
static volatile int  s_web_ty = -1;
static volatile bool s_web_touch_down = false;
static volatile uint32_t s_web_touch_ts = 0;

inline void hal_inject_web_touch(int x, int y, bool pressed){
  if(x < 0) x = 0; if(x >= LCD_H_RES) x = LCD_H_RES - 1;
  if(y < 0) y = 0; if(y >= LCD_V_RES) y = LCD_V_RES - 1;
  s_web_tx = x;
  s_web_ty = y;
  s_web_touch_down = pressed;
  s_web_touch_ts = millis();
}

// ---------------- CH422G expander ----------------
static constexpr uint8_t CH422G_WR_SET = 0x24;
static constexpr uint8_t CH422G_WR_IO  = 0x38;
static constexpr uint8_t EXIO_TP_RST  = 1;
static constexpr uint8_t EXIO_BL_DISP = 2;
static constexpr uint8_t EXIO_LCD_RST = 3;
static constexpr uint8_t EXIO_SD_CS   = 4;   // CH422G EXIO4 = MicroSD chip-select (active low)
static uint8_t s_io_shadow = 0;
static uint32_t s_bl_recoveries = 0;   // times the self-heal watchdog re-inited a reset expander
#ifdef WOKWI_SIMULATION
static void ch422g_write(uint8_t a, uint8_t v){ (void)a; (void)v; }
static void ch422g_flush(){}
static void ch422g_set(uint8_t bit, bool hi){ if(hi) s_io_shadow|=(1u<<bit); else s_io_shadow&=~(1u<<bit); }
#else
static void ch422g_write(uint8_t a, uint8_t v){ Wire.beginTransmission(a); Wire.write(v); Wire.endTransmission(); }
static void ch422g_flush(){ ch422g_write(CH422G_WR_IO, s_io_shadow); }
static void ch422g_set(uint8_t bit, bool hi){ if(hi) s_io_shadow|=(1u<<bit); else s_io_shadow&=~(1u<<bit); ch422g_flush(); }
#endif


// ---------------- Touch Controller (GT911 on Waveshare / FT6206 + Web in Wokwi) ----------------
static uint8_t gt_addr = 0x5D;
static bool gt_read(uint16_t reg, uint8_t* buf, size_t len){
#ifdef WOKWI_SIMULATION
  (void)reg;
  if(buf && len >= 4){ buf[0]='9'; buf[1]='1'; buf[2]='1'; buf[3]=0; }
  return true;
#else
  Wire.beginTransmission(gt_addr); Wire.write((uint8_t)(reg>>8)); Wire.write((uint8_t)(reg&0xFF));
  if(Wire.endTransmission(false)!=0) return false;
  size_t n=Wire.requestFrom((int)gt_addr,(int)len);
  for(size_t i=0;i<len && Wire.available();++i) buf[i]=Wire.read();
  return n==len;
#endif
}
static bool gt_write(uint16_t reg, uint8_t val){
#ifdef WOKWI_SIMULATION
  (void)reg; (void)val;
  return true;
#else
  Wire.beginTransmission(gt_addr); Wire.write((uint8_t)(reg>>8)); Wire.write((uint8_t)(reg&0xFF)); Wire.write(val);
  return Wire.endTransmission()==0;
#endif
}
static bool gt_init(){
#ifdef WOKWI_SIMULATION
  return true;
#else
  pinMode(PIN_TP_INT, OUTPUT); digitalWrite(PIN_TP_INT, LOW);
  ch422g_set(EXIO_TP_RST,false); delay(12);
  ch422g_set(EXIO_TP_RST,true);  delay(12);
  digitalWrite(PIN_TP_INT,LOW);  delay(60);
  pinMode(PIN_TP_INT, INPUT);
  for(uint8_t a : {(uint8_t)0x5D,(uint8_t)0x14}){ gt_addr=a; uint8_t id[4]={0}; if(gt_read(0x8140,id,4)) return true; }
  return false;
#endif
}

// returns: -1 error · -2 no NEW data ready (caller must HOLD last state, NOT release) ·
//          0 finger lifted · >0 pressed (x,y updated). Treating "no new data" as a lift
//          is what made scrolling stutter — the GT911 doesn't refresh every poll.
static int gt_poll(int& x, int& y){
  // 1. Check remote web-viewer touch injection first (works on both Wokwi and physical hardware!)
  if(s_web_touch_ts > 0 && (millis() - s_web_touch_ts) < 2500){
    if(s_web_touch_down){
      x = s_web_tx;
      y = s_web_ty;
      return 1;
    } else if((millis() - s_web_touch_ts) < 250){
      return 0;
    }
  }
#ifdef WOKWI_SIMULATION
  // 2. Wokwi board-ili9341-cap-touch integrated FT6206 capacitive touch at I2C 0x38
  // Native panel is 240x320 portrait; rotated landscape (320x240) scales cleanly to 800x480
  Wire.beginTransmission(0x38);
  Wire.write(0x02); // TD_STATUS
  if(Wire.endTransmission(false) != 0) return 0;
  if(Wire.requestFrom(0x38, 5) == 5){
    uint8_t touches = Wire.read() & 0x0F;
    uint8_t xh = Wire.read();
    uint8_t xl = Wire.read();
    uint8_t yh = Wire.read();
    uint8_t yl = Wire.read();
    if(touches > 0){
      uint16_t raw_x = ((xh & 0x0F) << 8) | xl; // 0..239
      uint16_t raw_y = ((yh & 0x0F) << 8) | yl; // 0..319
      // Map FT6206 portrait (240x320) to landscape (800x480) matching MADCTL=0x28:
      int lx = (int)raw_y * LCD_H_RES / 320;
      int ly = (int)(240 - 1 - (int)raw_x) * LCD_V_RES / 240;
      if(lx < 0) lx = 0; if(lx >= LCD_H_RES) lx = LCD_H_RES - 1;
      if(ly < 0) ly = 0; if(ly >= LCD_V_RES) ly = LCD_V_RES - 1;
      x = lx;
      y = ly;
      return 1;
    }
  }
  return 0;
#else
  uint8_t st=0; if(!gt_read(0x814E,&st,1)) return -1;
  if(!(st&0x80)) return -2;
  int n=st&0x0F;
  if(n>0){ uint8_t p[8]; if(gt_read(0x8150,p,8)){ x=p[0]|(p[1]<<8); y=p[2]|(p[3]<<8); } }
  gt_write(0x814E,0);
  return n;
#endif
}

// ---------------- Display Panel (RGB on Waveshare / Virtual 800x480 + SPI ILI9341 on Wokwi) ----------------
static esp_lcd_panel_handle_t s_panel=nullptr;

#ifdef WOKWI_SIMULATION
#define WOKWI_TFT_CS   15
#define WOKWI_TFT_DC   2
#define WOKWI_TFT_RST  4
#define WOKWI_TFT_MOSI 11
#define WOKWI_TFT_SCLK 12
#define WOKWI_TFT_MISO 13
static SPIClass s_wokwi_spi(FSPI);

static inline void wokwi_tft_cmd(uint8_t cmd){
  digitalWrite(WOKWI_TFT_DC, LOW);
  s_wokwi_spi.transfer(cmd);
  digitalWrite(WOKWI_TFT_DC, HIGH);
}
static inline void wokwi_tft_data(uint8_t d){
  s_wokwi_spi.transfer(d);
}
static void wokwi_tft_init(){
  pinMode(WOKWI_TFT_CS, OUTPUT);  digitalWrite(WOKWI_TFT_CS, HIGH);
  pinMode(WOKWI_TFT_DC, OUTPUT);  digitalWrite(WOKWI_TFT_DC, HIGH);
  pinMode(WOKWI_TFT_RST, OUTPUT); digitalWrite(WOKWI_TFT_RST, HIGH);
  delay(5);
  digitalWrite(WOKWI_TFT_RST, LOW);  delay(20);
  digitalWrite(WOKWI_TFT_RST, HIGH); delay(120);

  s_wokwi_spi.begin(WOKWI_TFT_SCLK, WOKWI_TFT_MISO, WOKWI_TFT_MOSI, WOKWI_TFT_CS);
  s_wokwi_spi.beginTransaction(SPISettings(40000000, MSBFIRST, SPI_MODE0));
  digitalWrite(WOKWI_TFT_CS, LOW);

  wokwi_tft_cmd(0x01); delay(120); // SWRESET
  wokwi_tft_cmd(0x11); delay(120); // SLPOUT
  wokwi_tft_cmd(0x3A); wokwi_tft_data(0x55); // PIXFMT 16-bit RGB565
  wokwi_tft_cmd(0x36); wokwi_tft_data(0x28); // MADCTL: MV=1, BGR=1 (320x240 landscape)
  wokwi_tft_cmd(0x29); delay(20);  // DISPON

  digitalWrite(WOKWI_TFT_CS, HIGH);
  s_wokwi_spi.endTransaction();
}

// Downscale dirty 800x480 region from s_sim_fb to Wokwi's 320x240 ILI9341 SPI screen
static void wokwi_tft_blit_region(int x1, int y1, int x2, int y2){
  if(!s_sim_fb) return;
  int sx1 = (x1 * 320) / LCD_H_RES;
  int sy1 = (y1 * 240) / LCD_V_RES;
  int sx2 = (x2 * 320) / LCD_H_RES;
  int sy2 = (y2 * 240) / LCD_V_RES;
  if(sx1 < 0) sx1 = 0; if(sy1 < 0) sy1 = 0;
  if(sx2 >= 320) sx2 = 319; if(sy2 >= 240) sy2 = 239;
  if(sx1 > sx2 || sy1 > sy2) return;

  s_wokwi_spi.beginTransaction(SPISettings(40000000, MSBFIRST, SPI_MODE0));
  digitalWrite(WOKWI_TFT_CS, LOW);

  wokwi_tft_cmd(0x2A); // CASET
  wokwi_tft_data(sx1 >> 8); wokwi_tft_data(sx1 & 0xFF);
  wokwi_tft_data(sx2 >> 8); wokwi_tft_data(sx2 & 0xFF);

  wokwi_tft_cmd(0x2B); // PASET
  wokwi_tft_data(sy1 >> 8); wokwi_tft_data(sy1 & 0xFF);
  wokwi_tft_data(sy2 >> 8); wokwi_tft_data(sy2 & 0xFF);

  wokwi_tft_cmd(0x2C); // RAMWR
  static uint16_t line_swapped[320];
  int w = sx2 - sx1 + 1;
  for(int sy = sy1; sy <= sy2; ++sy){
    int src_y = (sy * LCD_V_RES + (LCD_V_RES / 2)) / 240;
    if(src_y >= LCD_V_RES) src_y = LCD_V_RES - 1;
    const uint16_t* src_row = s_sim_fb + (src_y * LCD_H_RES);
    for(int sx = sx1; sx <= sx2; ++sx){
      int src_x = (sx * LCD_H_RES + (LCD_H_RES / 2)) / 320;
      if(src_x >= LCD_H_RES) src_x = LCD_H_RES - 1;
      uint16_t px = src_row[src_x];
      line_swapped[sx - sx1] = (uint16_t)((px >> 8) | (px << 8)); // big-endian SPI RGB565
    }
    s_wokwi_spi.writeBytes((const uint8_t*)line_swapped, w * 2);
  }

  digitalWrite(WOKWI_TFT_CS, HIGH);
  s_wokwi_spi.endTransaction();
}
#endif

static int rgb_panel_init(){
#ifdef WOKWI_SIMULATION
  if(!s_sim_fb){
    s_sim_fb = (uint16_t*)heap_caps_malloc(LCD_H_RES * LCD_V_RES * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if(!s_sim_fb) s_sim_fb = (uint16_t*)malloc(LCD_H_RES * LCD_V_RES * sizeof(uint16_t));
    if(s_sim_fb){
      // Fill initial framebuffer with warm cream (0xFBF7F0 -> RGB565 0xFFBE) so Wokwi screen lights up immediately
      for(int i = 0; i < LCD_H_RES * LCD_V_RES; ++i) s_sim_fb[i] = 0xFFBE;
    }
  }
  wokwi_tft_init();
  if(s_sim_fb) wokwi_tft_blit_region(0, 0, LCD_H_RES - 1, LCD_V_RES - 1);
  return 0;
#else
  esp_lcd_rgb_panel_config_t cfg={};
  cfg.clk_src=LCD_CLK_SRC_DEFAULT;
  cfg.timings.pclk_hz=16*1000*1000; cfg.timings.h_res=LCD_H_RES; cfg.timings.v_res=LCD_V_RES;
  cfg.timings.hsync_pulse_width=4; cfg.timings.hsync_back_porch=40; cfg.timings.hsync_front_porch=8;
  cfg.timings.vsync_pulse_width=4; cfg.timings.vsync_back_porch=8; cfg.timings.vsync_front_porch=8;
  cfg.timings.flags.pclk_active_neg=1;
  cfg.data_width=16; cfg.bits_per_pixel=16; cfg.num_fbs=1;
  cfg.bounce_buffer_size_px=LCD_H_RES*12;
  cfg.dma_burst_size=64;
  cfg.flags.bb_invalidate_cache=1;
  cfg.hsync_gpio_num=46; cfg.vsync_gpio_num=3; cfg.de_gpio_num=5; cfg.pclk_gpio_num=7; cfg.disp_gpio_num=-1;
  const int dp[16]={14,38,18,17,10, 39,0,45,48,47,21, 1,2,42,41,40};
  for(int i=0;i<16;++i) cfg.data_gpio_nums[i]=dp[i];
  cfg.flags.fb_in_psram=1;
  if(esp_lcd_new_rgb_panel(&cfg,&s_panel)!=ESP_OK) return 1;
  if(esp_lcd_panel_reset(s_panel)!=ESP_OK) return 2;
  if(esp_lcd_panel_init(s_panel)!=ESP_OK) return 3;
  return 0;
#endif
}

// Obtain pointer to the live 800x480 RGB565 framebuffer (works in both Wokwi and physical hardware)
inline const uint16_t* hal_get_framebuffer(){
#ifdef WOKWI_SIMULATION
  return s_sim_fb;
#else
  if(!s_panel) return nullptr;
  void* fb0 = nullptr;
  if(esp_lcd_rgb_panel_get_frame_buffer(s_panel, 1, &fb0) == ESP_OK && fb0){
    return (const uint16_t*)fb0;
  }
  return nullptr;
#endif
}

// ---------------- LVGL glue ----------------
#define DRAW_LINES 24
static lv_disp_draw_buf_t s_draw_buf;
static lv_color_t* s_buf1=nullptr;
#ifdef WOKWI_SIMULATION
static int  s_dirty_x1 = LCD_H_RES, s_dirty_y1 = LCD_V_RES, s_dirty_x2 = -1, s_dirty_y2 = -1;
#endif
static void flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* px){
#ifdef WOKWI_SIMULATION
  if(s_sim_fb){
    int w = area->x2 - area->x1 + 1;
    const uint16_t* src = (const uint16_t*)px;
    for(int y = area->y1; y <= area->y2; ++y){
      if(y >= 0 && y < LCD_V_RES){
        memcpy(s_sim_fb + (y * LCD_H_RES) + area->x1, src, w * sizeof(uint16_t));
      }
      src += w;
    }
    if(area->x1 < s_dirty_x1) s_dirty_x1 = area->x1;
    if(area->y1 < s_dirty_y1) s_dirty_y1 = area->y1;
    if(area->x2 > s_dirty_x2) s_dirty_x2 = area->x2;
    if(area->y2 > s_dirty_y2) s_dirty_y2 = area->y2;
    if(lv_disp_flush_is_last(drv) && s_dirty_x2 >= s_dirty_x1){
      wokwi_tft_blit_region(s_dirty_x1, s_dirty_y1, s_dirty_x2, s_dirty_y2);
      s_dirty_x1 = LCD_H_RES; s_dirty_y1 = LCD_V_RES; s_dirty_x2 = -1; s_dirty_y2 = -1;
    }
  }
#else
  esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2+1, area->y2+1, px);
#endif
  lv_disp_flush_ready(drv);
}
static int s_lx=0, s_ly=0;
static lv_indev_state_t s_tstate=LV_INDEV_STATE_REL;
static uint32_t s_tlast=0;                 // millis() of the last fresh touch sample
static uint32_t s_touch_samples=0;         // total fresh PRESSED samples (diag: phantom-touch counter)
static void touch_cb(lv_indev_drv_t* drv, lv_indev_data_t* data){
  int x=-1,y=-1; int n=gt_poll(x,y);
  if(n>0 && x>=0 && y>=0 && x<LCD_H_RES && y<LCD_V_RES){ s_lx=x; s_ly=y; s_tstate=LV_INDEV_STATE_PR; s_tlast=millis(); s_touch_samples++; }
  else if(n==0) s_tstate=LV_INDEV_STATE_REL;             // genuine lift
  else {
    if(s_tstate==LV_INDEV_STATE_PR && millis()-s_tlast > 100) s_tstate=LV_INDEV_STATE_REL;
  }
  data->state=s_tstate;
  data->point.x=s_lx; data->point.y=s_ly;
}

// ---------------- display-flicker diagnostic: on-demand PSRAM bus load ----------------
static volatile uint32_t s_flick_until = 0;       // millis() deadline; load runs while now < this
static TaskHandle_t      s_flick_task  = nullptr;
static void flick_stress_task(void*){
  const size_t SZ = 2*1024*1024;
  uint8_t* buf = (uint8_t*)heap_caps_malloc(SZ, MALLOC_CAP_SPIRAM);
  for(;;){
    if(buf && (int32_t)(s_flick_until - millis()) > 0){
      memcpy(buf, buf + SZ/2, SZ/2);
      vTaskDelay(1);
    } else {
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }
}
