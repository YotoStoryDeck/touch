#pragma once
// Hardware Abstraction Layer for Waveshare ESP32-S3-Touch-LCD-4.3B
// CH422G IO expander, GT911 touch controller, RGB LCD panel, LVGL display glue
#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"

#define PIN_I2C_SDA 8
#define PIN_I2C_SCL 9
#define PIN_TP_INT  4
#define LCD_H_RES   800
#define LCD_V_RES   480

// ---------------- CH422G expander ----------------
static constexpr uint8_t CH422G_WR_SET = 0x24;
static constexpr uint8_t CH422G_WR_IO  = 0x38;
static constexpr uint8_t EXIO_TP_RST  = 1;
static constexpr uint8_t EXIO_BL_DISP = 2;
static constexpr uint8_t EXIO_LCD_RST = 3;
static constexpr uint8_t EXIO_SD_CS   = 4;   // CH422G EXIO4 = MicroSD chip-select (active low)
static uint8_t s_io_shadow = 0;
static uint32_t s_bl_recoveries = 0;   // times the self-heal watchdog re-inited a reset expander
static void ch422g_write(uint8_t a, uint8_t v){ Wire.beginTransmission(a); Wire.write(v); Wire.endTransmission(); }
static void ch422g_flush(){ ch422g_write(CH422G_WR_IO, s_io_shadow); }
static void ch422g_set(uint8_t bit, bool hi){ if(hi) s_io_shadow|=(1u<<bit); else s_io_shadow&=~(1u<<bit); ch422g_flush(); }


// ---------------- GT911 touch ----------------
static uint8_t gt_addr = 0x5D;
static bool gt_read(uint16_t reg, uint8_t* buf, size_t len){
  Wire.beginTransmission(gt_addr); Wire.write((uint8_t)(reg>>8)); Wire.write((uint8_t)(reg&0xFF));
  if(Wire.endTransmission(false)!=0) return false;
  size_t n=Wire.requestFrom((int)gt_addr,(int)len);
  for(size_t i=0;i<len && Wire.available();++i) buf[i]=Wire.read();
  return n==len;
}
static bool gt_write(uint16_t reg, uint8_t val){
  Wire.beginTransmission(gt_addr); Wire.write((uint8_t)(reg>>8)); Wire.write((uint8_t)(reg&0xFF)); Wire.write(val);
  return Wire.endTransmission()==0;
}
static bool gt_init(){
  pinMode(PIN_TP_INT, OUTPUT); digitalWrite(PIN_TP_INT, LOW);
  ch422g_set(EXIO_TP_RST,false); delay(12);
  ch422g_set(EXIO_TP_RST,true);  delay(12);
  digitalWrite(PIN_TP_INT,LOW);  delay(60);
  pinMode(PIN_TP_INT, INPUT);
  for(uint8_t a : {(uint8_t)0x5D,(uint8_t)0x14}){ gt_addr=a; uint8_t id[4]={0}; if(gt_read(0x8140,id,4)) return true; }
  return false;
}
// returns: -1 error · -2 no NEW data ready (caller must HOLD last state, NOT release) ·
//          0 finger lifted · >0 pressed (x,y updated). Treating "no new data" as a lift
//          is what made scrolling stutter — the GT911 doesn't refresh every poll.
static int gt_poll(int& x, int& y){
  uint8_t st=0; if(!gt_read(0x814E,&st,1)) return -1;
  if(!(st&0x80)) return -2;
  int n=st&0x0F;
  if(n>0){ uint8_t p[8]; if(gt_read(0x8150,p,8)){ x=p[0]|(p[1]<<8); y=p[2]|(p[3]<<8); } }
  gt_write(0x814E,0);
  return n;
}

// ---------------- RGB panel ----------------
// Single framebuffer + bounce buffer = the proven rock-solid config. (A double-fb
// direct-render experiment scrolled smoother but introduced a static-screen flicker that
// scaled with PSRAM use, so we reverted it. Smoother+tear-free is a future anti-tearing
// pass that needs serial-monitored iteration.)
static esp_lcd_panel_handle_t s_panel=nullptr;
static int rgb_panel_init(){
  esp_lcd_rgb_panel_config_t cfg={};
  cfg.clk_src=LCD_CLK_SRC_DEFAULT;
  cfg.timings.pclk_hz=16*1000*1000; cfg.timings.h_res=LCD_H_RES; cfg.timings.v_res=LCD_V_RES;
  // hsync_back_porch was 8 — far below this panel's reference (ESPHome 30, Arduino_GFX 88) at the
  // same 16MHz pclk. Too small a back porch starts clocking active pixels before horizontal retrace
  // finishes, so the first ~N px of every line render garbage => a persistent vertical band at the
  // LEFT edge on a static screen (the reported symptom). Raised 8 -> 30 to give the panel retrace time.
  cfg.timings.hsync_pulse_width=4; cfg.timings.hsync_back_porch=30; cfg.timings.hsync_front_porch=8;
  cfg.timings.vsync_pulse_width=4; cfg.timings.vsync_back_porch=8; cfg.timings.vsync_front_porch=8;
  cfg.timings.flags.pclk_active_neg=1;
  cfg.data_width=16; cfg.bits_per_pixel=16; cfg.num_fbs=1;
  cfg.bounce_buffer_size_px=LCD_H_RES*10;          // 8000px bounce buffer -> fixes the PSRAM-contention drift
  cfg.dma_burst_size=64;                            // bigger PSRAM read bursts -> bounce refill stays ahead of
                                                    // the scanline (combats the left-edge underrun flicker)
  cfg.flags.bb_invalidate_cache=1;                  // free DCache after each bounce copy -> less cache pressure;
                                                    // safe while content is static (no concurrent FB writes)
  cfg.hsync_gpio_num=46; cfg.vsync_gpio_num=3; cfg.de_gpio_num=5; cfg.pclk_gpio_num=7; cfg.disp_gpio_num=-1;
  const int dp[16]={14,38,18,17,10, 39,0,45,48,47,21, 1,2,42,41,40};
  for(int i=0;i<16;++i) cfg.data_gpio_nums[i]=dp[i];
  cfg.flags.fb_in_psram=1;
  if(esp_lcd_new_rgb_panel(&cfg,&s_panel)!=ESP_OK) return 1;
  if(esp_lcd_panel_reset(s_panel)!=ESP_OK) return 2;
  if(esp_lcd_panel_init(s_panel)!=ESP_OK) return 3;
  return 0;
}

// ---------------- LVGL glue ----------------
// Draw buffer in FAST internal RAM (not PSRAM): LVGL renders into SRAM ~2x faster, which
// is the main lever for scroll framerate. Falls back to PSRAM if internal alloc fails.
// Was 40 lines (64KB), but linking in the OTA stack (WebServer+ElegantOTA+ArduinoOTA) ate
// enough internal RAM that the Yoto TLS handshake (~40-50KB) could no longer allocate at
// boot -> "Library HTTP -1  heap=60160". 24 lines (38KB) frees ~26KB and restores the
// handshake headroom while keeping a buffer >2x the LVGL default. Tune up if scroll suffers
// AND free heap allows (watch the heap= readout on the library-error path).
#define DRAW_LINES 16
static lv_disp_draw_buf_t s_draw_buf;
static lv_color_t* s_buf1=nullptr;
static void flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* px){
  esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2+1, area->y2+1, px);
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
    // n==-2 (no new data) / n==-1 (error): HOLD to bridge GT911 refresh gaps so scrolling
    // doesn't stutter — but force release after 100ms so a missing lift event can't stick
    // pressed (a stuck press keeps the screen redrawing -> flicker on the single fb).
    if(s_tstate==LV_INDEV_STATE_PR && millis()-s_tlast > 100) s_tstate=LV_INDEV_STATE_REL;
  }
  data->state=s_tstate;
  data->point.x=s_lx; data->point.y=s_ly;
}

// ---------------- display-flicker diagnostic: on-demand PSRAM bus load ----------------
// Hypothesis: the static-screen flicker confined to the LEFT ~1/10 (the START of each scanline)
// is RGB-DMA underrun — the bounce buffer can't refill the line in time because the framebuffer
// (in PSRAM) is losing the bus to other PSRAM traffic (mbedTLS bufs, MQTT JSON, covers). This
// task streams a 2MB read+write across PSRAM on demand so we can watch the left edge degrade and
// recover LIVE (curl /flick?stress=SECONDS) — proving or refuting the cause with NO reflash.
// Pinned to core 0 (off LVGL's core 1) so any change is attributable to BUS contention, not CPU
// starvation of the UI. Idle cost is zero until /flick?stress is hit (task is created lazily).
static volatile uint32_t s_flick_until = 0;       // millis() deadline; load runs while now < this
static TaskHandle_t      s_flick_task  = nullptr;
static void flick_stress_task(void*){
  const size_t SZ = 2*1024*1024;                  // >> the PSRAM cache, so every pass really hits the bus
  uint8_t* buf = (uint8_t*)heap_caps_malloc(SZ, MALLOC_CAP_SPIRAM);
  for(;;){
    if(buf && (int32_t)(s_flick_until - millis()) > 0){
      memcpy(buf, buf + SZ/2, SZ/2);              // 1MB read + 1MB write per pass = heavy PSRAM bus load
      vTaskDelay(1);                              // yield 1 tick each pass so core-0 IDLE/WDT runs
                                                  // (a no-yield 20s hog tripped the task watchdog -> reset)
    } else {
      vTaskDelay(pdMS_TO_TICKS(20));              // idle: yield, generate nothing
    }
  }
}

