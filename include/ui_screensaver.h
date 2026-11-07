#pragma once
// Night-clock screensaver + OK-to-wake state machine with Orbital Pixel-Shifting & Date Display
#include <Arduino.h>
#include <lvgl.h>
#include <time.h>
#include "app_config.h"
#include "hal_display.h"

void go_home();  // fwd reference

static lv_obj_t* s_scr_saver     = nullptr;
static lv_obj_t* s_sv_seg[4][7];            // big 7-segment clock: 4 digits x 7 bars
static lv_obj_t* s_sv_colon[2];
static lv_obj_t* s_sv_cont       = nullptr; // holds all the bars; slides to keep the time centered
static lv_obj_t* s_sv_date_lbl   = nullptr; // centered date label beneath clock
static int       s_sv_x4         = 0;       // container x for 4 visible cells (10-12)
static int       s_sv_x3         = 0;       // container x for 3 visible cells (1-9)
static int       s_sv_base_y     = 0;       // vertical base coordinate
static int       s_sv_date_y     = 0;       // vertical coordinate for date label
static int       s_sv_last4      = -1;      // last layout used (-1 = unset)
static int       s_sv_lastmin    = -999;    // last painted hh*60+mm (-999 = force repaint)
static uint32_t  s_sv_lastcol    = 0;       // last painted color
static int       s_sv_last_shift = -1;      // last orbital shift index

// 8-point subtle orbital shift (radius ~4-6 pixels) to prevent static pixel retention
static const int8_t SHIFT_X[8] = { 0,  4,  6,  4,  0, -4, -6, -4};
static const int8_t SHIFT_Y[8] = {-4, -3,  0,  3,  4,  3,  0, -3};

static const char* const W_DAYS[7] = {
  "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};
static const char* const MONTHS[12] = {
  "January", "February", "March", "April", "May", "June",
  "July", "August", "September", "October", "November", "December"
};

// ---------------- night-clock screensaver + OK-to-wake ----------------
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
  uint32_t col = night ? 0xE5352B : morning ? 0x00FF33 : 0xF2EDE0;

  // Orbital pixel-shift calculation (shifts every 10 minutes to rotate across 8 positions)
  int shift_idx = ok ? (((tmv.tm_hour * 60 + tmv.tm_min) / 10) % 8) : 0;
  int off_x = SHIFT_X[shift_idx];
  int off_y = SHIFT_Y[shift_idx];

  int hh = ok ? ((tmv.tm_hour + 11) % 12) + 1 : -1;
  int four = (!ok || hh >= 10) ? 1 : 0;    // dashes + 10-12 o'clock use all 4 cells

  // Slide position on layout change (3 vs 4 digits) or periodic orbital shift
  if(four != s_sv_last4 || shift_idx != s_sv_last_shift){
    s_sv_last4 = four;
    s_sv_last_shift = shift_idx;
    int base_x = four ? s_sv_x4 : s_sv_x3;
    if(s_sv_cont){
      lv_obj_set_pos(s_sv_cont, base_x + off_x, s_sv_base_y + off_y);
    }
    if(s_sv_date_lbl){
      lv_obj_set_pos(s_sv_date_lbl, off_x, s_sv_date_y + off_y);
    }
  }

  if(now_m == s_sv_lastmin && col == s_sv_lastcol) return;   // only repaint digits & colors on change
  s_sv_lastmin = now_m; s_sv_lastcol = col;

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

  // Update date string (e.g. "Friday, November 6, 2026")
  if(s_sv_date_lbl){
    if(ok){
      char date_buf[64];
      snprintf(date_buf, sizeof(date_buf), "%s, %s %d, %d",
               W_DAYS[tmv.tm_wday % 7], MONTHS[tmv.tm_mon % 12], tmv.tm_mday, 1900 + tmv.tm_year);
      lv_label_set_text(s_sv_date_lbl, date_buf);
    } else {
      lv_label_set_text(s_sv_date_lbl, "");
    }
    lv_obj_set_style_text_color(s_sv_date_lbl, c, 0);
  }
}

static lv_obj_t* sv_bar(lv_obj_t* par, int x, int y, int w, int h){
  lv_obj_t* o = lv_obj_create(par);
  lv_obj_set_pos(o, x, y); lv_obj_set_size(o, w, h);
  lv_obj_set_style_radius(o, (w < h ? w : h) / 2, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
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

  // Total composition: Digits (height H) + 20px gap + Date label (~24px)
  int comp_h = H + 20 + 24;
  s_sv_base_y = (LCD_V_RES - comp_h) / 2;
  s_sv_date_y = s_sv_base_y + H + 20;

  s_sv_x4 = (LCD_H_RES - total) / 2;                              // 10:00-12:59 and ----
  s_sv_x3 = (LCD_H_RES - (total - W - gap)) / 2 - (W + gap);      // 1:00-9:59
  lv_obj_t* cont = lv_obj_create(scr);
  lv_obj_set_size(cont, total, H);
  lv_obj_set_pos(cont, s_sv_x4, s_sv_base_y);
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

  // Date label beneath the clock
  lv_obj_t* dl = lv_label_create(scr);
  s_sv_date_lbl = dl;
  lv_obj_set_width(dl, LCD_H_RES);
  lv_obj_set_pos(dl, 0, s_sv_date_y);
  lv_obj_set_style_text_align(dl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(dl, &lv_font_montserrat_20, 0);
  lv_obj_set_style_bg_opa(dl, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(dl, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

  s_saver_prev_scr = lv_scr_act();
  s_sv_lastmin = -999; s_sv_lastcol = 0; s_sv_last4 = -1; s_sv_last_shift = -1;
  s_scr_saver = scr;
  saver_tick();
  lv_scr_load(scr);
}

static void saver_close(){
  if(!s_scr_saver) return;
  lv_obj_t* s = s_scr_saver;
  s_scr_saver   = nullptr;
  s_sv_cont     = nullptr;
  s_sv_date_lbl = nullptr;
  go_home();
  s_saver_prev_scr = nullptr;
  lv_obj_del_async(s);
}
