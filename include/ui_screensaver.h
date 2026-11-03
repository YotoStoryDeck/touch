#pragma once
// Night-clock screensaver + OK-to-wake state machine
#include <Arduino.h>
#include <lvgl.h>
#include <time.h>
#include "app_config.h"
#include "hal_display.h"

void go_home();  // fwd reference

static lv_obj_t* s_scr_saver=nullptr;
static lv_obj_t* s_sv_seg[4][7];            // big 7-segment clock: 4 digits x 7 bars
static lv_obj_t* s_sv_colon[2];
static lv_obj_t* s_sv_cont=nullptr;         // holds all the bars; slides to keep the time centered
static int      s_sv_x4=0, s_sv_x3=0;       // container x for 4 visible cells vs 3 (hour 1-9)
static int      s_sv_last4=-1;              // last layout used (-1 = unset)
static int      s_sv_lastmin=-999;          // last painted hh*60+mm (-999 = force repaint)
static uint32_t s_sv_lastcol=0;             // last painted color

// ---------------- night-clock screensaver + OK-to-wake ----------------
// After cfg::saver_min idle minutes the panel becomes a bedside clock: huge 7-segment
// digits drawn from plain lv_objs (LVGL's biggest built-in font is 48px — far too small
// to fill an 800x480 panel, and a 300px bitmap font would bloat flash; bars scale free).
// RED from bedtime to wake time ("stay in bed"), GREEN for two hours from wake time (the
// classic kids' "OK to get up" signal), dim cream the rest of the day. No words — the
// color IS the message. Tap anywhere to get the cards back.
static void saver_close(); // fwd
static void saver_close_cb(lv_event_t*){ saver_close(); }

// segment bit order: A top, B top-right, C bottom-right, D bottom, E bottom-left,
// F top-left, G middle (the standard 7-seg encoding)
static const uint8_t SEG_MAP[10] = {0x3F,0x06,0x5B,0x4F,0x66,0x6D,0x7D,0x07,0x7F,0x6F};

static void sv_set_digit(int d, int v){   // v: 0-9 · -1 = dash (NTP not synced) · -2 = blank
  uint8_t m = v >= 0 ? SEG_MAP[v] : (v == -1 ? 0x40 : 0x00);
  for(int s = 0; s < 7; ++s){
    if(m & (1u << s)) lv_obj_clear_flag(s_sv_seg[d][s], LV_OBJ_FLAG_HIDDEN);
    else              lv_obj_add_flag(s_sv_seg[d][s], LV_OBJ_FLAG_HIDDEN);
  }
}

static void saver_tick(){
  if(!s_scr_saver) return;
  struct tm tmv;
  bool ok    = getLocalTime(&tmv, 0);
  int now_m  = ok ? tmv.tm_hour * 60 + tmv.tm_min : -1;
  int wake_t  = cfg::wake_h  * 60 + cfg::wake_m;
  int night_t = cfg::night_h * 60 + cfg::night_m;
  bool night   = ok && ((now_m >= night_t) || (now_m < wake_t));
  bool morning = ok && !night && (now_m < wake_t + 120);
  uint32_t col = night ? 0xE5352B : morning ? 0x3AB06A : 0xF2EDE0;
  if(now_m == s_sv_lastmin && col == s_sv_lastcol) return;   // only repaint on change —
  s_sv_lastmin = now_m; s_sv_lastcol = col;                  // this ticks at 1 Hz
  int hh = ok ? ((tmv.tm_hour + 11) % 12) + 1 : -1;
  int four = (!ok || hh >= 10) ? 1 : 0;    // dashes + 10-12 o'clock use all 4 cells
  if(four != s_sv_last4){                  // slide so the VISIBLE cells stay centered
    s_sv_last4 = four;
    lv_obj_set_x(s_sv_cont, four ? s_sv_x4 : s_sv_x3);
  }
  if(ok){
    sv_set_digit(0, hh >= 10 ? 1 : -2);    // blank leading digit for 1:00-9:59
    sv_set_digit(1, hh % 10);
    sv_set_digit(2, tmv.tm_min / 10);
    sv_set_digit(3, tmv.tm_min % 10);
  } else {
    for(int d = 0; d < 4; ++d) sv_set_digit(d, -1);          // ----  until NTP syncs
  }
  lv_color_t c = lv_color_hex(col);
  for(int d = 0; d < 4; ++d)
    for(int s = 0; s < 7; ++s) lv_obj_set_style_bg_color(s_sv_seg[d][s], c, 0);
  lv_obj_set_style_bg_color(s_sv_colon[0], c, 0);
  lv_obj_set_style_bg_color(s_sv_colon[1], c, 0);
}

static lv_obj_t* sv_bar(lv_obj_t* par, int x, int y, int w, int h){
  lv_obj_t* o = lv_obj_create(par);
  lv_obj_set_pos(o, x, y); lv_obj_set_size(o, w, h);
  lv_obj_set_style_radius(o, (w < h ? w : h) / 2, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);  // taps fall through to the screen
  return o;
}

static void saver_open(){
  lv_obj_t* scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x0A0E1A), 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(scr, saver_close_cb, LV_EVENT_CLICKED, NULL);

  // digit cell geometry from the configured size; Large fills most of the panel
  static const int DIG_H[3] = {200, 260, 320};
  int H = DIG_H[cfg::saver_size < 0 ? 0 : cfg::saver_size > 2 ? 2 : cfg::saver_size];
  int t = H / 8, W = H / 2, gap = H / 16;
  int vlen = (H - 3 * t) / 2;                      // vertical bar length (B,C,E,F)
  int total = 4 * W + t + 4 * gap;                 // 4 digits + colon column + gaps
  // all bars live in one transparent container that slides horizontally so the VISIBLE
  // time stays centered: hours 1-9 hide the leading digit, which would otherwise leave
  // the clock holding an empty cell on the left and sitting right of center all evening
  s_sv_x4 = (LCD_H_RES - total) / 2;                              // 10:00-12:59 and ----
  s_sv_x3 = (LCD_H_RES - (total - W - gap)) / 2 - (W + gap);      // 1:00-9:59
  lv_obj_t* cont = lv_obj_create(scr);
  lv_obj_set_size(cont, total, H);
  lv_obj_set_pos(cont, s_sv_x4, (LCD_V_RES - H) / 2);
  lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(cont, 0, 0);
  lv_obj_set_style_pad_all(cont, 0, 0);
  lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  s_sv_cont = cont;
  int xs[4]; xs[0] = 0; xs[1] = W + gap;
  int xc = xs[1] + W + gap;                        // colon column
  xs[2] = xc + t + gap; xs[3] = xs[2] + W + gap;
  for(int d = 0; d < 4; ++d){
    int X = xs[d];
    s_sv_seg[d][0] = sv_bar(cont, X + t,     0,             W - 2*t, t);  // A
    s_sv_seg[d][1] = sv_bar(cont, X + W - t, t,             t, vlen);     // B
    s_sv_seg[d][2] = sv_bar(cont, X + W - t, (H + t)/2,     t, vlen);     // C
    s_sv_seg[d][3] = sv_bar(cont, X + t,     H - t,         W - 2*t, t);  // D
    s_sv_seg[d][4] = sv_bar(cont, X,         (H + t)/2,     t, vlen);     // E
    s_sv_seg[d][5] = sv_bar(cont, X,         t,             t, vlen);     // F
    s_sv_seg[d][6] = sv_bar(cont, X + t,     (H - t)/2,     W - 2*t, t);  // G
  }
  s_sv_colon[0] = sv_bar(cont, xc, H/3   - t/2, t, t);
  s_sv_colon[1] = sv_bar(cont, xc, 2*H/3 - t/2, t, t);

  s_sv_lastmin = -999; s_sv_lastcol = 0; s_sv_last4 = -1;   // force the first paint
  s_scr_saver = scr;                       // before saver_tick — it guards on this
  saver_tick();
  lv_scr_load(scr);
}

static void saver_close(){
  if(!s_scr_saver) return;
  lv_obj_t* s = s_scr_saver;
  s_scr_saver = nullptr;       // saver_tick guards on this, so the seg pointers can't be used stale
  go_home();                   // also clears any detail/now/settings screens left behind it
  lv_obj_del_async(s);         // we're inside this screen's own click event — defer deletion
}

