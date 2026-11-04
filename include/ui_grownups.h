#pragma once
// Grown-Ups Settings Screen and Parent PIN Keypad Modal (Full Web Kiosk Parity)
#include <Arduino.h>
#include <lvgl.h>
#include "app_config.h"
#include "core_api.h"
#include "hal_display.h"
#include "app_state.h"

extern int         s_sleep_sec;
extern int         s_vol;
extern lv_obj_t*   s_scr_home;
extern lv_obj_t*   s_scr_profile;
extern lv_obj_t*   s_scr_settings;
extern lv_obj_t*   s_scr_pin;
extern lv_obj_t*   s_scr_prev;
extern lv_obj_t*   s_scr_error;
extern int         s_card_count;
extern String      get_active_player_name();
extern lv_obj_t*   s_set_sleep_lbl;

extern void fmt_sleep(char* b, size_t n);
extern void np_volume(int v);
extern void go_home();
extern void build_grid();
extern void open_profile_picker();
extern void start_hotspot_cb(lv_event_t* e);

void open_settings();             // fwd
void open_pin_modal(bool for_settings_change); // fwd

static void sync_settings_if_core(){
  if(cfg::op_mode == "core" && cfg::core_url.length() > 0){
    core::sync_device_settings(cfg::core_url);
  }
}

// ---------------- grown-ups settings screen ----------------
static lv_obj_t* s_set_vol_lbl=nullptr;
static lv_obj_t* s_set_saver_lbl=nullptr;
static lv_obj_t* s_set_wake_lbl=nullptr;
static lv_obj_t* s_set_night_lbl=nullptr;
static lv_obj_t* s_set_size_lbl=nullptr;

static void fmt_wake(char* b, size_t n){
  int h12 = ((cfg::wake_h + 11) % 12) + 1;
  snprintf(b, n, "%d:%02d %s", h12, cfg::wake_m, cfg::wake_h < 12 ? "am" : "pm");
}

inline lv_obj_t* set_section(lv_obj_t* p, const char* t){
  lv_obj_t* l = lv_label_create(p);
  lv_label_set_text(l, t);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(0x3A6B50), 0);
  lv_obj_set_style_pad_top(l, 8, 0);
  return l;
}
inline lv_obj_t* set_row(lv_obj_t* p){
  lv_obj_t* r = lv_obj_create(p);
  lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(r, 0, 0);
  lv_obj_set_style_pad_all(r, 0, 0);
  lv_obj_set_style_pad_column(r, 12, 0);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
  return r;
}
inline lv_obj_t* pill_btn(lv_obj_t* row, const char* txt, int ud, lv_event_cb_t cb, bool active = false){
  lv_obj_t* b = lv_btn_create(row);
  lv_obj_set_size(b, 88, 50);
  lv_obj_set_style_radius(b, 14, 0);
  lv_obj_set_style_bg_color(b, active ? lv_color_hex(0xE5352B) : lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(0xF3E9D8), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_border_width(b, 1, 0);
  lv_obj_set_style_border_color(b, active ? lv_color_hex(0xE5352B) : lv_color_hex(0xE7E0D2), 0);
  lv_obj_set_user_data(b, (void*)(intptr_t)ud);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(l, active ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x23303A), 0);
  lv_obj_center(l);
  return b;
}

static void set_sleep_cb(lv_event_t* e){
  int mins = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
  cfg::sleep_min = mins;
  if(mins == 0){
    s_sleep_sec = 0;
  } else if(!cfg::sleep_bedtime || is_bedtime_now()){
    s_sleep_sec = 60 * mins;
  } else {
    s_sleep_sec = 0;
  }
  cfg::save();
  sync_settings_if_core();
  open_settings();
}
static void set_saver_cb(lv_event_t* e){
  cfg::saver_min = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
  cfg::save();
  sync_settings_if_core();
  open_settings();
}
static void set_wake_cb(lv_event_t* e){
  int t = cfg::wake_h * 60 + cfg::wake_m + (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
  if(t < 5*60)  t = 5*60;                    // sane bedroom range: 5:00 – 10:00 am
  if(t > 10*60) t = 10*60;
  cfg::wake_h = t / 60; cfg::wake_m = t % 60;
  cfg::save();
  sync_settings_if_core();
  if(s_set_wake_lbl){ char b[16]; fmt_wake(b, sizeof b); lv_label_set_text(s_set_wake_lbl, b); }
}
static void fmt_night(char* b, size_t n){
  int h12 = ((cfg::night_h + 11) % 12) + 1;
  snprintf(b, n, "%d:%02d %s", h12, cfg::night_m, cfg::night_h < 12 ? "am" : "pm");
}
static void set_night_cb(lv_event_t* e){
  int t = cfg::night_h * 60 + cfg::night_m + (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
  if(t < 17*60) t = 17*60;                   // sane bedtime range: 5:00 – 11:00 pm
  if(t > 23*60) t = 23*60;
  cfg::night_h = t / 60; cfg::night_m = t % 60;
  cfg::save();
  sync_settings_if_core();
  if(s_set_night_lbl){ char b[16]; fmt_night(b, sizeof b); lv_label_set_text(s_set_night_lbl, b); }
}
static const char* SV_SIZE_NAME[3] = {"Small", "Medium", "Large"};
static void set_size_cb(lv_event_t* e){
  int v = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
  cfg::saver_size = v < 0 ? 0 : v > 2 ? 2 : v;
  cfg::save();                               // applies the next time the clock comes on
  sync_settings_if_core();
  open_settings();
}
static void set_vol_drag_cb(lv_event_t* e){   // live readout while dragging
  if(s_set_vol_lbl) lv_label_set_text_fmt(s_set_vol_lbl, "%d", lv_slider_get_value(lv_event_get_target(e)));
}
static void set_vol_done_cb(lv_event_t* e){   // persist + apply on release
  int v = lv_slider_get_value(lv_event_get_target(e));
  cfg::vol_max = v; cfg::save();
  if(s_vol > v) np_volume(v);                 // pull the live volume under the new cap
}

// ---------------- Parent PIN Keypad Modal (pin.js 3-step parity) ----------------
enum PinFlowMode {
  PIN_MODE_UNLOCK = 0,
  PIN_MODE_VERIFY_OLD = 1,
  PIN_MODE_NEW_SETUP = 2,
  PIN_MODE_NEW_CONFIRM = 3
};
static PinFlowMode s_pin_mode = PIN_MODE_UNLOCK;
static String s_pin_entered = "";
static String s_pin_new_temp = "";
static int    s_pin_fails = 0;
static uint32_t s_pin_lockout_until = 0;
static lv_obj_t* s_pin_dots[4] = {nullptr, nullptr, nullptr, nullptr};
static lv_obj_t* s_pin_title_lbl = nullptr;
static lv_obj_t* s_pin_msg_lbl = nullptr;
static lv_obj_t* s_scr_prev_pin = nullptr;

static const char* pin_mode_title(PinFlowMode m){
  switch(m){
    case PIN_MODE_VERIFY_OLD:  return "Verify Current PIN";
    case PIN_MODE_NEW_SETUP:   return "Create Parent PIN";
    case PIN_MODE_NEW_CONFIRM: return "Confirm Parent PIN";
    default:                   return "Parent PIN Required";
  }
}
static const char* pin_mode_prompt(PinFlowMode m){
  switch(m){
    case PIN_MODE_VERIFY_OLD:  return "Enter current 4-digit PIN";
    case PIN_MODE_NEW_SETUP:   return "Choose a 4-digit PIN for Grown-ups settings";
    case PIN_MODE_NEW_CONFIRM: return "Re-enter your 4-digit PIN to confirm";
    default:                   return "Enter 4-digit PIN to access Grown-ups settings";
  }
}

static void update_pin_dots(){
  int len = s_pin_entered.length();
  for(int i = 0; i < 4; i++){
    if(s_pin_dots[i]){
      if(i < len){
        lv_obj_set_style_bg_color(s_pin_dots[i], lv_color_hex(0xE5352B), 0);
      } else {
        lv_obj_set_style_bg_color(s_pin_dots[i], lv_color_hex(0xE2DCD2), 0);
      }
    }
  }
}

static void close_pin_modal(){
  s_pin_entered = "";
  s_pin_new_temp = "";
  if(s_scr_pin){
    lv_obj_t* p = s_scr_pin;
    s_scr_pin = nullptr;
    if(s_scr_prev_pin){
      lv_scr_load(s_scr_prev_pin);
      s_scr_prev_pin = nullptr;
    } else if(s_scr_prev){
      lv_scr_load(s_scr_prev);
    } else if(s_scr_home){
      lv_scr_load(s_scr_home);
    } else if(s_scr_error){
      lv_scr_load(s_scr_error);
    }
    lv_obj_del_async(p);
  }
}

static void pin_digit_cb(lv_event_t* e){
  if(millis() < s_pin_lockout_until) return; // locked out
  const char* val = (const char*)lv_event_get_user_data(e);
  if(!val) return;

  if(strcmp(val, "C") == 0){
    s_pin_entered = "";
    update_pin_dots();
    if(s_pin_msg_lbl) lv_label_set_text(s_pin_msg_lbl, pin_mode_prompt(s_pin_mode));
    return;
  }
  if(strcmp(val, "<") == 0){
    if(s_pin_entered.length() > 0){
      s_pin_entered.remove(s_pin_entered.length() - 1);
      update_pin_dots();
    }
    return;
  }

  if(s_pin_entered.length() < 4){
    s_pin_entered += val[0];
    update_pin_dots();
  }

  // Auto-validate once 4 digits entered
  if(s_pin_entered.length() == 4){
    // Step 1 of Change PIN: Verify current PIN
    if(s_pin_mode == PIN_MODE_VERIFY_OLD){
      if(s_pin_entered == cfg::parent_pin){
        s_pin_fails = 0;
        s_pin_entered = "";
        s_pin_mode = PIN_MODE_NEW_SETUP;
        update_pin_dots();
        if(s_pin_title_lbl) lv_label_set_text(s_pin_title_lbl, pin_mode_title(s_pin_mode));
        if(s_pin_msg_lbl)   lv_label_set_text(s_pin_msg_lbl, pin_mode_prompt(s_pin_mode));
      } else {
        s_pin_fails++;
        s_pin_entered = "";
        update_pin_dots();
        if(s_pin_msg_lbl) lv_label_set_text(s_pin_msg_lbl, "Incorrect current PIN. Try again.");
      }
      return;
    }

    // Step 2 of Change PIN: Enter new 4-digit PIN
    if(s_pin_mode == PIN_MODE_NEW_SETUP){
      s_pin_new_temp = s_pin_entered;
      s_pin_entered = "";
      s_pin_mode = PIN_MODE_NEW_CONFIRM;
      update_pin_dots();
      if(s_pin_title_lbl) lv_label_set_text(s_pin_title_lbl, pin_mode_title(s_pin_mode));
      if(s_pin_msg_lbl)   lv_label_set_text(s_pin_msg_lbl, pin_mode_prompt(s_pin_mode));
      return;
    }

    // Step 3 of Change PIN: Confirm new 4-digit PIN
    if(s_pin_mode == PIN_MODE_NEW_CONFIRM){
      if(s_pin_entered == s_pin_new_temp){
        cfg::parent_pin = s_pin_entered;
        cfg::save();
        sync_settings_if_core();
        if(s_pin_msg_lbl) lv_label_set_text(s_pin_msg_lbl, "PIN changed successfully!");
        lv_refr_now(lv_disp_get_default());
        delay(500);
        close_pin_modal();
      } else {
        s_pin_entered = "";
        s_pin_new_temp = "";
        s_pin_mode = PIN_MODE_NEW_SETUP;
        update_pin_dots();
        if(s_pin_title_lbl) lv_label_set_text(s_pin_title_lbl, pin_mode_title(s_pin_mode));
        if(s_pin_msg_lbl)   lv_label_set_text(s_pin_msg_lbl, "PINs did not match. Enter new PIN again.");
      }
      return;
    }

    // Normal Unlock Mode
    if(s_pin_entered == cfg::parent_pin){
      s_pin_fails = 0;
      s_pin_entered = "";
      s_scr_prev_pin = nullptr;
      if(s_scr_pin){
        lv_obj_t* p = s_scr_pin;
        s_scr_pin = nullptr;
        open_settings();
        lv_obj_del_async(p);
      }
    } else {
      s_pin_fails++;
      s_pin_entered = "";
      update_pin_dots();
      if(s_pin_fails >= 3){
        s_pin_lockout_until = millis() + 10000;
        if(s_pin_msg_lbl) lv_label_set_text(s_pin_msg_lbl, "Too many attempts! Locked for 10s");
      } else {
        if(s_pin_msg_lbl) lv_label_set_text(s_pin_msg_lbl, "Incorrect PIN. Try again.");
      }
    }
  }
}

void open_pin_modal(bool for_settings_change){
  if(for_settings_change){
    s_scr_prev_pin = lv_scr_act();
    s_pin_mode = (cfg::parent_pin.length() > 0) ? PIN_MODE_VERIFY_OLD : PIN_MODE_NEW_SETUP;
  } else {
    s_scr_prev = lv_scr_act();
    s_scr_prev_pin = s_scr_prev;
    s_pin_mode = PIN_MODE_UNLOCK;
  }
  s_pin_entered = "";
  s_pin_new_temp = "";

  lv_obj_t* scr = lv_obj_create(NULL);
  s_scr_pin = scr;
  lv_obj_set_style_bg_color(scr, lv_color_hex(0xFBF7F0), 0);

  // Close / Cancel button (top-left)
  lv_obj_t* back = lv_btn_create(scr);
  lv_obj_set_size(back, 52, 52);
  lv_obj_set_pos(back, 14, 14);
  lv_obj_set_style_radius(back, 26, 0);
  lv_obj_set_style_bg_color(back, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_shadow_width(back, 8, 0);
  lv_obj_set_style_shadow_opa(back, LV_OPA_30, 0);
  lv_obj_add_event_cb(back, [](lv_event_t*){ close_pin_modal(); }, LV_EVENT_CLICKED, NULL);
  lv_obj_t* bk = lv_label_create(back);
  lv_label_set_text(bk, LV_SYMBOL_CLOSE);
  lv_obj_set_style_text_font(bk, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(bk, lv_color_hex(0x23303A), 0);
  lv_obj_center(bk);

  // Modal Title
  s_pin_title_lbl = lv_label_create(scr);
  lv_label_set_text(s_pin_title_lbl, pin_mode_title(s_pin_mode));
  lv_obj_set_style_text_font(s_pin_title_lbl, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(s_pin_title_lbl, lv_color_hex(0xE5352B), 0);
  lv_obj_align(s_pin_title_lbl, LV_ALIGN_TOP_MID, 0, 20);

  // Subtitle / message
  s_pin_msg_lbl = lv_label_create(scr);
  lv_label_set_text(s_pin_msg_lbl, pin_mode_prompt(s_pin_mode));
  lv_obj_set_style_text_font(s_pin_msg_lbl, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(s_pin_msg_lbl, lv_color_hex(0x7A7060), 0);
  lv_obj_align(s_pin_msg_lbl, LV_ALIGN_TOP_MID, 0, 60);

  // 4 PIN Dots indicator container
  lv_obj_t* dcont = lv_obj_create(scr);
  lv_obj_set_size(dcont, 160, 36);
  lv_obj_align(dcont, LV_ALIGN_TOP_MID, 0, 95);
  lv_obj_set_style_bg_opa(dcont, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(dcont, 0, 0);
  lv_obj_set_style_pad_all(dcont, 0, 0);
  lv_obj_set_flex_flow(dcont, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(dcont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(dcont, LV_OBJ_FLAG_SCROLLABLE);

  for(int i = 0; i < 4; i++){
    s_pin_dots[i] = lv_obj_create(dcont);
    lv_obj_set_size(s_pin_dots[i], 20, 20);
    lv_obj_set_style_radius(s_pin_dots[i], 10, 0);
    lv_obj_set_style_bg_color(s_pin_dots[i], lv_color_hex(0xE2DCD2), 0);
    lv_obj_set_style_border_width(s_pin_dots[i], 0, 0);
  }

  // 3x4 Keypad Container
  lv_obj_t* kpad = lv_obj_create(scr);
  lv_obj_set_size(kpad, 360, 310);
  lv_obj_align(kpad, LV_ALIGN_BOTTOM_MID, 0, -15);
  lv_obj_set_style_bg_opa(kpad, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(kpad, 0, 0);
  lv_obj_set_style_pad_all(kpad, 0, 0);
  lv_obj_set_style_pad_row(kpad, 10, 0);
  lv_obj_set_style_pad_column(kpad, 16, 0);
  lv_obj_set_flex_flow(kpad, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(kpad, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(kpad, LV_OBJ_FLAG_SCROLLABLE);

  static const char* keys[12] = {
    "1", "2", "3",
    "4", "5", "6",
    "7", "8", "9",
    "C", "0", "<"
  };

  for(int i = 0; i < 12; i++){
    lv_obj_t* btn = lv_btn_create(kpad);
    lv_obj_set_size(btn, 98, 62);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xF3E9D8), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xE7E0D2), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, pin_digit_cb, LV_EVENT_CLICKED, (void*)keys[i]);

    lv_obj_t* l = lv_label_create(btn);
    lv_label_set_text(l, keys[i]);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x23303A), 0);
    lv_obj_center(l);
  }

  lv_scr_load(scr);
  lv_refr_now(lv_disp_get_default());
}

enum SettingsTab { GU_TAB_SLEEP = 0, GU_TAB_TARGET = 1, GU_TAB_DISPLAY = 2, GU_TAB_NETWORK = 3 };
static SettingsTab s_gu_active_tab = GU_TAB_SLEEP;
static lv_obj_t* s_gu_tab_panels[4] = {nullptr, nullptr, nullptr, nullptr};
static lv_obj_t* s_gu_tab_btns[4]   = {nullptr, nullptr, nullptr, nullptr};

static void gu_tab_switch(SettingsTab tab){
  s_gu_active_tab = tab;
  for(int i = 0; i < 4; i++){
    if(s_gu_tab_panels[i]){
      if(i == (int)tab) lv_obj_clear_flag(s_gu_tab_panels[i], LV_OBJ_FLAG_HIDDEN);
      else              lv_obj_add_flag(s_gu_tab_panels[i], LV_OBJ_FLAG_HIDDEN);
    }
    if(s_gu_tab_btns[i]){
      bool sel = (i == (int)tab);
      lv_obj_set_style_bg_color(s_gu_tab_btns[i], sel ? lv_color_hex(0xE5352B) : lv_color_hex(0xFFFFFF), 0);
      lv_obj_t* lbl = lv_obj_get_child(s_gu_tab_btns[i], 0);
      if(lbl) lv_obj_set_style_text_color(lbl, sel ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x23303A), 0);
    }
  }
}

static void gu_tab_btn_cb(lv_event_t* e){
  int tab = (int)(intptr_t)lv_event_get_user_data(e);
  gu_tab_switch((SettingsTab)tab);
}

void open_settings(){
  lv_obj_t* scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0xFBF7F0), 0);

  // Header Back Button
  lv_obj_t* back = lv_btn_create(scr);
  lv_obj_set_size(back, 50, 50);
  lv_obj_set_pos(back, 14, 12);
  lv_obj_set_style_radius(back, 25, 0);
  lv_obj_set_style_bg_color(back, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_shadow_width(back, 6, 0);
  lv_obj_set_style_shadow_opa(back, LV_OPA_30, 0);
  lv_obj_add_event_cb(back, [](lv_event_t*){
    if(!s_lib_ok){
      s_set_sleep_lbl = nullptr;
      if(s_scr_settings){ lv_obj_del_async(s_scr_settings); s_scr_settings=nullptr; }
      s_scr_prev = nullptr;
      net_init_and_fetch();
      return;
    }
    if(s_scr_prev == s_scr_profile && s_scr_profile && cfg::op_mode == "core"){
      s_set_sleep_lbl = nullptr;
      if(s_scr_settings){ lv_obj_del_async(s_scr_settings); s_scr_settings=nullptr; }
      s_scr_prev = nullptr;
      open_profile_picker();
    } else {
      go_home();
    }
  }, LV_EVENT_CLICKED, NULL);
  lv_obj_t* bk = lv_label_create(back);
  lv_label_set_text(bk, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_font(bk, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(bk, lv_color_hex(0x23303A), 0);
  lv_obj_center(bk);

  // Title
  lv_obj_t* title = lv_label_create(scr);
  lv_label_set_text(title, "Grown-ups Settings");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xE5352B), 0);
  lv_obj_set_pos(title, 76, 22);

  // Change PIN button
  lv_obj_t* chg_pin = pill_btn(scr, "Change PIN", 0, [](lv_event_t*){ open_pin_modal(true); });
  lv_obj_set_size(chg_pin, 140, 42);
  lv_obj_set_pos(chg_pin, LCD_H_RES - 156, 16);
  lv_obj_set_style_text_font(lv_obj_get_child(chg_pin, 0), &lv_font_montserrat_18, 0);

  // 4-Tab Navigation Bar (grownups.js parity)
  lv_obj_t* nav = lv_obj_create(scr);
  lv_obj_set_size(nav, LCD_H_RES - 28, 48);
  lv_obj_set_pos(nav, 14, 68);
  lv_obj_set_style_bg_color(nav, lv_color_hex(0xF4EDE0), 0);
  lv_obj_set_style_border_width(nav, 0, 0);
  lv_obj_set_style_pad_all(nav, 4, 0);
  lv_obj_set_style_pad_column(nav, 8, 0);
  lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(nav, LV_OBJ_FLAG_SCROLLABLE);

  static const char* tab_titles[4] = {"Sleep / Bed", "Audio Target", "Display / Dim", "Network"};
  for(int t = 0; t < 4; t++){
    lv_obj_t* tb = lv_btn_create(nav);
    lv_obj_set_size(tb, 180, 40);
    lv_obj_set_style_radius(tb, 20, 0);
    bool sel = (t == (int)s_gu_active_tab);
    lv_obj_set_style_bg_color(tb, sel ? lv_color_hex(0xE5352B) : lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(tb, 1, 0);
    lv_obj_set_style_border_color(tb, lv_color_hex(0xE7E0D2), 0);
    lv_obj_set_style_shadow_width(tb, 0, 0);
    lv_obj_add_event_cb(tb, gu_tab_btn_cb, LV_EVENT_CLICKED, (void*)(intptr_t)t);

    lv_obj_t* tl = lv_label_create(tb);
    lv_label_set_text(tl, tab_titles[t]);
    lv_obj_set_style_text_font(tl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(tl, sel ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x23303A), 0);
    lv_obj_center(tl);

    s_gu_tab_btns[t] = tb;
  }

  // Common content area below tab bar
  const int cont_y = 122;
  const int cont_h = LCD_V_RES - cont_y - 10;

  for(int i = 0; i < 4; i++){
    lv_obj_t* panel = lv_obj_create(scr);
    lv_obj_set_size(panel, LCD_H_RES - 28, cont_h);
    lv_obj_set_pos(panel, 14, cont_y);
    lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 6, 0);
    lv_obj_set_style_pad_row(panel, 8, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    s_gu_tab_panels[i] = panel;
  }

  // ==========================================
  // TAB 0: 🌙 SLEEP / BED (grownups.js parity)
  // ==========================================
  lv_obj_t* p0 = s_gu_tab_panels[0];

  set_section(p0, "Sleep Timer  (stops player when elapsed)");
  lv_obj_t* r_sleep = set_row(p0);
  pill_btn(r_sleep, "Off", 0,  set_sleep_cb, cfg::sleep_min == 0);
  pill_btn(r_sleep, "15m", 15, set_sleep_cb, cfg::sleep_min == 15);
  pill_btn(r_sleep, "30m", 30, set_sleep_cb, cfg::sleep_min == 30);
  pill_btn(r_sleep, "45m", 45, set_sleep_cb, cfg::sleep_min == 45);
  pill_btn(r_sleep, "60m", 60, set_sleep_cb, cfg::sleep_min == 60);
  s_set_sleep_lbl = lv_label_create(r_sleep);
  { char b[24]; fmt_sleep(b, sizeof b); lv_label_set_text(s_set_sleep_lbl, b); }
  lv_obj_set_style_text_font(s_set_sleep_lbl, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(s_set_sleep_lbl, lv_color_hex(0x9A9384), 0);

  if(cfg::sleep_min > 0){
    set_section(p0, "Sleep Timer Scope");
    lv_obj_t* r_smode = set_row(p0);
    lv_obj_t* b_sbed = pill_btn(r_smode, "Bedtime Hours Only", 1, [](lv_event_t*){
      cfg::sleep_bedtime = true;
      if(!is_bedtime_now()) s_sleep_sec = 0;
      else if(s_sleep_sec == 0) s_sleep_sec = cfg::sleep_min * 60;
      cfg::save();
      sync_settings_if_core();
      open_settings();
    }, cfg::sleep_bedtime);
    lv_obj_set_size(b_sbed, 220, 48);

    lv_obj_t* b_sall = pill_btn(r_smode, "All the Time", 0, [](lv_event_t*){
      cfg::sleep_bedtime = false;
      if(s_sleep_sec == 0) s_sleep_sec = cfg::sleep_min * 60;
      cfg::save();
      sync_settings_if_core();
      open_settings();
    }, !cfg::sleep_bedtime);
    lv_obj_set_size(b_sall, 180, 48);
  }

  set_section(p0, "Bedtime Curfew & OK-to-Wake Schedule");
  lv_obj_t* r_sched = set_row(p0);
  lv_obj_t* nl = lv_label_create(r_sched);
  lv_label_set_text(nl, "Bed:");
  lv_obj_set_style_text_font(nl, &lv_font_montserrat_18, 0);
  pill_btn(r_sched, "-15m", -15, set_night_cb);
  s_set_night_lbl = lv_label_create(r_sched);
  { char b[16]; fmt_night(b, sizeof b); lv_label_set_text(s_set_night_lbl, b); }
  lv_obj_set_style_text_font(s_set_night_lbl, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(s_set_night_lbl, lv_color_hex(0x23303A), 0);
  pill_btn(r_sched, "+15m", 15, set_night_cb);

  lv_obj_t* wl = lv_label_create(r_sched);
  lv_label_set_text(wl, "  Wake:");
  lv_obj_set_style_text_font(wl, &lv_font_montserrat_18, 0);
  pill_btn(r_sched, "-15m", -15, set_wake_cb);
  s_set_wake_lbl = lv_label_create(r_sched);
  { char b[16]; fmt_wake(b, sizeof b); lv_label_set_text(s_set_wake_lbl, b); }
  lv_obj_set_style_text_font(s_set_wake_lbl, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(s_set_wake_lbl, lv_color_hex(0x23303A), 0);
  pill_btn(r_sched, "+15m", 15, set_wake_cb);

  set_section(p0, "Travel & Away Player Protection");
  lv_obj_t* r_away = set_row(p0);
  lv_obj_t* b_blk = pill_btn(r_away, "Block Away Players", 1, [](lv_event_t*){
    cfg::block_remote = true;
    cfg::save();
    sync_settings_if_core();
    open_settings();
  }, cfg::block_remote);
  lv_obj_set_size(b_blk, 220, 48);

  lv_obj_t* b_alw = pill_btn(r_away, "Allow Away Players", 0, [](lv_event_t*){
    cfg::block_remote = false;
    cfg::save();
    sync_settings_if_core();
    open_settings();
  }, !cfg::block_remote);
  lv_obj_set_size(b_alw, 220, 48);

  // ==========================================
  // TAB 1: 🎯 AUDIO TARGET & ROAMING
  // ==========================================
  lv_obj_t* p1 = s_gu_tab_panels[1];

  bool is_core_mode = (cfg::op_mode == "core");
  set_section(p1, is_core_mode ? "Target Player & Profile Roaming" : "Target Player Assignment (Standalone)");
  lv_obj_t* r_lock = set_row(p1);
  lv_obj_t* b_roam = pill_btn(r_lock, is_core_mode ? "Profile Roaming" : "Roam Players", 0, [](lv_event_t*){
    cfg::lock_player = false;
    cfg::locked_dev_id = "";
    if(cfg::op_mode == "core"){
      cfg::device_id = core::resolve_active_device_id(core::s_active_profile_idx);
    } else {
      cfg::device_id = core::resolve_active_device_id(-1);
    }
    cfg::save();
    sync_settings_if_core();
    if(s_scr_home) build_grid();
    open_settings();
  }, !cfg::lock_player);
  lv_obj_set_size(b_roam, 185, 50);

  lv_obj_t* b_lock = pill_btn(r_lock, "Lock to Player", 1, [](lv_event_t*){
    cfg::lock_player = true;
    if(cfg::locked_dev_id.length() == 0) cfg::locked_dev_id = cfg::device_id;
    cfg::device_id = cfg::locked_dev_id;
    cfg::save();
    sync_settings_if_core();
    if(s_scr_home) build_grid();
    open_settings();
  }, cfg::lock_player);
  lv_obj_set_size(b_lock, 185, 50);

  // ONLY show the Locked Target Player selector when "Lock to Player" is selected!
  if(cfg::lock_player){
    lv_obj_t* r_drop = set_row(p1);
    lv_obj_t* dd_lbl = lv_label_create(r_drop);
    lv_label_set_text(dd_lbl, "Locked Player:");
    lv_obj_set_style_text_font(dd_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(dd_lbl, lv_color_hex(0x23303A), 0);

    lv_obj_t* dd = lv_dropdown_create(r_drop);
    lv_obj_set_size(dd, 360, 48);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_16, 0);

    String opt_str = "";
    int cur_sel = 0;
    int opt_idx = 0;
    String seen_ids[16];
    int seen_count = 0;
    String target_lock_id = cfg::locked_dev_id.length() ? cfg::locked_dev_id : cfg::device_id;

    auto add_opt = [&](const String& pid, const String& pname) {
      if(pid.length() == 0 || seen_count >= 16) return;
      for(int s = 0; s < seen_count; s++) if(seen_ids[s] == pid) return;
      seen_ids[seen_count++] = pid;
      if(opt_str.length() > 0) opt_str += "\n";
      opt_str += (pname.length() > 0 ? pname : String("Player ") + pid);
      if(target_lock_id == pid) cur_sel = opt_idx;
      opt_idx++;
    };

    if(cfg::op_mode == "core"){
      for(int p = 0; p < core::s_profile_count; p++){
        for(int k = 0; k < core::s_profiles[p].player_count; k++){
          add_opt(core::s_profiles[p].players[k].id, core::s_profiles[p].players[k].name);
        }
        add_opt(core::s_profiles[p].default_player_id, core::s_profiles[p].default_player_name);
      }
    } else {
      for(int k = 0; k < core::s_standalone_player_count; k++){
        add_opt(core::s_standalone_players[k].id, core::s_standalone_players[k].name);
      }
    }
    if(cfg::device_id.length() > 0){
      add_opt(cfg::device_id, get_active_player_name());
    }
    if(opt_str.length() == 0) opt_str = "No players discovered";

    lv_dropdown_set_options(dd, opt_str.c_str());
    lv_dropdown_set_selected(dd, cur_sel);
    lv_obj_add_event_cb(dd, [](lv_event_t* e){
      lv_obj_t* d = lv_event_get_target(e);
      uint16_t sel = lv_dropdown_get_selected(d);
      String opts[16];
      int o_cnt = 0;
      auto reg_id = [&](const String& pid){
        if(pid.length() == 0 || o_cnt >= 16) return;
        for(int s = 0; s < o_cnt; s++) if(opts[s] == pid) return;
        opts[o_cnt++] = pid;
      };
      if(cfg::op_mode == "core"){
        for(int p = 0; p < core::s_profile_count; p++){
          for(int k = 0; k < core::s_profiles[p].player_count; k++){
            reg_id(core::s_profiles[p].players[k].id);
          }
          reg_id(core::s_profiles[p].default_player_id);
        }
      } else {
        for(int k = 0; k < core::s_standalone_player_count; k++){
          reg_id(core::s_standalone_players[k].id);
        }
      }
      if(cfg::device_id.length() > 0) reg_id(cfg::device_id);
      if(sel < o_cnt){
        cfg::device_id = opts[sel];
        cfg::locked_dev_id = opts[sel];
        cfg::save();
        sync_settings_if_core();
        if(s_scr_home) build_grid();
      }
    }, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t* lock_hint = lv_label_create(p1);
    lv_label_set_text(lock_hint, "Locked Mode: Audio output is pinned to the selected player above.\nThe player picker on the catalog bar is disabled while locked.");
    lv_obj_set_style_text_font(lock_hint, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lock_hint, lv_color_hex(0x7A7060), 0);
  } else {
    lv_obj_t* roam_hint = lv_label_create(p1);
    if(is_core_mode){
      const String& cur_p = (core::s_profile_count > 0 && core::s_active_profile_idx < core::s_profile_count)
                            ? core::s_profiles[core::s_active_profile_idx].display_name : String("Active Profile");
      lv_label_set_text_fmt(roam_hint,
        "Roaming Active (%s -> %s):\n"
        "You can switch profiles and pick between assigned players directly on the catalog top bar.",
        cur_p.c_str(), get_active_player_name().c_str());
    } else {
      lv_label_set_text_fmt(roam_hint,
        "Player Roaming Active (Current: %s):\n"
        "Tap the player pill on the catalog top bar to switch between any Yoto player on your account.",
        get_active_player_name().c_str());
    }
    lv_obj_set_style_text_font(roam_hint, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(roam_hint, lv_color_hex(0x137333), 0);
  }

  set_section(p1, "Operational Mode");
  lv_obj_t* r_mode = set_row(p1);
  lv_obj_t* b_mcore = pill_btn(r_mode, "Core Managed", 0, [](lv_event_t*){
    if(cfg::op_mode != "core"){
      cfg::op_mode = "core";
      cfg::save();
      s_lib_ok = false;
    }
    open_settings();
  }, cfg::op_mode == "core");
  lv_obj_set_size(b_mcore, 160, 50);

  lv_obj_t* b_mstand = pill_btn(r_mode, "Standalone Cloud", 1, [](lv_event_t*){
    if(cfg::op_mode != "standalone"){
      cfg::op_mode = "standalone";
      cfg::save();
      s_lib_ok = false;
    }
    open_settings();
  }, cfg::op_mode != "core");
  lv_obj_set_size(b_mstand, 175, 50);

  lv_obj_t* mode_hint = lv_label_create(p1);
  lv_label_set_text(mode_hint, cfg::op_mode == "core"
    ? "Core Managed: Hub-assisted multi-child profiles, ABS audiobooks & local proxy."
    : "Standalone: Direct connection to Yoto Cloud without requiring a local Core server.");
  lv_obj_set_style_text_font(mode_hint, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(mode_hint, lv_color_hex(0x7A7060), 0);

  // ==========================================
  // TAB 2: 💡 DISPLAY / DIM
  // ==========================================
  lv_obj_t* p2 = s_gu_tab_panels[2];

  set_section(p2, "Volume limit  (maximum ear-safe cap)");
  lv_obj_t* r_vol = set_row(p2);
  lv_obj_set_style_pad_all(r_vol, 6, 0);
  lv_obj_t* vs = lv_slider_create(r_vol);
  lv_obj_set_size(vs, 500, 18);
  lv_slider_set_range(vs, 10, 100);
  lv_slider_set_value(vs, cfg::vol_max, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(vs, lv_color_hex(0xE5352B), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(vs, lv_color_hex(0xE5352B), LV_PART_KNOB);
  lv_obj_set_style_pad_all(vs, 10, LV_PART_KNOB);
  lv_obj_add_event_cb(vs, set_vol_drag_cb, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(vs, set_vol_done_cb, LV_EVENT_RELEASED, NULL);
  s_set_vol_lbl = lv_label_create(r_vol);
  lv_label_set_text_fmt(s_set_vol_lbl, "%d", cfg::vol_max);
  lv_obj_set_style_text_font(s_set_vol_lbl, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(s_set_vol_lbl, lv_color_hex(0x23303A), 0);

  set_section(p2, "Night clock inactivity timeout");
  lv_obj_t* r_saver = set_row(p2);
  pill_btn(r_saver, "Off", 0,  set_saver_cb, cfg::saver_min == 0);
  pill_btn(r_saver, "2m",  2,  set_saver_cb, cfg::saver_min == 2);
  pill_btn(r_saver, "5m",  5,  set_saver_cb, cfg::saver_min == 5);
  pill_btn(r_saver, "10m", 10, set_saver_cb, cfg::saver_min == 10);
  pill_btn(r_saver, "30m", 30, set_saver_cb, cfg::saver_min == 30);
  s_set_saver_lbl = lv_label_create(r_saver);
  if(cfg::saver_min) lv_label_set_text_fmt(s_set_saver_lbl, "after %d min", cfg::saver_min);
  else               lv_label_set_text(s_set_saver_lbl, "Off");
  lv_obj_set_style_text_font(s_set_saver_lbl, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(s_set_saver_lbl, lv_color_hex(0x9A9384), 0);

  set_section(p2, "Bedside clock digit size");
  lv_obj_t* r_size = set_row(p2);
  int cur_sz = cfg::saver_size < 0 ? 0 : cfg::saver_size > 2 ? 2 : cfg::saver_size;
  lv_obj_set_size(pill_btn(r_size, "Small",  0, set_size_cb, cur_sz == 0), 124, 50);
  lv_obj_set_size(pill_btn(r_size, "Medium", 1, set_size_cb, cur_sz == 1), 124, 50);
  lv_obj_set_size(pill_btn(r_size, "Large",  2, set_size_cb, cur_sz == 2), 124, 50);
  s_set_size_lbl = lv_label_create(r_size);
  lv_label_set_text(s_set_size_lbl, SV_SIZE_NAME[cur_sz]);
  lv_obj_set_style_text_font(s_set_size_lbl, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(s_set_size_lbl, lv_color_hex(0x9A9384), 0);

  // ==========================================
  // TAB 3: 📶 NETWORK & SYSTEM
  // ==========================================
  lv_obj_t* p3 = s_gu_tab_panels[3];

  set_section(p3, "Network Diagnostics");
  String ip = WiFi.localIP().toString();
  String sd = s_sd_ok ? String((uint32_t)(SD.usedBytes()/(1024ULL*1024ULL))) + " MB used" : String("none");
  lv_obj_t* diag = lv_label_create(p3);
  lv_label_set_text_fmt(diag,
    "IP Address: %s      Wi-Fi RSSI: %d dBm\n"
    "SSID: %s            Core Host: %s\n"
    "Player Target: %s (%s)\n"
    "Cards in catalog: %d     SD cache: %s\n"
    "Free Heap: %u KB         Version: %s (%s)",
    ip.c_str(), (int)WiFi.RSSI(),
    cfg::wifi_ssid.c_str(), cfg::core_url.length() ? cfg::core_url.c_str() : "None (Standalone)",
    get_active_player_name().c_str(), cfg::device_id.length() ? cfg::device_id.c_str() : "None",
    s_card_count, sd.c_str(),
    (unsigned)(ESP.getFreeHeap()/1024), STORYDECK_VERSION, __DATE__);
  lv_obj_set_style_text_font(diag, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(diag, lv_color_hex(0x23303A), 0);

  set_section(p3, "Web Portals & OTA");
  lv_obj_t* urls = lv_label_create(p3);
  lv_label_set_text_fmt(urls,
    "Web Settings Portal:  http://%s/settings\n"
    "OTA Firmware Update:  http://%s/update",
    ip.c_str(), ip.c_str());
  lv_obj_set_style_text_font(urls, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(urls, lv_color_hex(0x3A6B50), 0);

  set_section(p3, "Hardware Maintenance");
  lv_obj_t* r_maint = set_row(p3);
  lv_obj_t* rb = pill_btn(r_maint, "Restart Device", 0, [](lv_event_t*){ ESP.restart(); });
  lv_obj_set_size(rb, 160, 50);
  lv_obj_set_style_text_color(lv_obj_get_child(rb, 0), lv_color_hex(0xE5352B), 0);

  lv_obj_t* hs = pill_btn(r_maint, "Start Hotspot", 0, start_hotspot_cb);
  lv_obj_set_size(hs, 160, 50);

  if(cfg::op_mode == "core" && cfg::core_url.length() > 0){
    lv_obj_t* rp = pill_btn(r_maint, "Re-pair Core", 0, [](lv_event_t*){
      core::s_is_approved = false;
      s_lib_ok = false;
      s_set_sleep_lbl = nullptr;
      if(s_scr_settings){ lv_obj_del_async(s_scr_settings); s_scr_settings = nullptr; }
      s_scr_prev = nullptr;
      net_init_and_fetch();
    });
    lv_obj_set_size(rp, 160, 50);
  }

  if(s_sd_ok){
    lv_obj_t* clr = pill_btn(r_maint, "Purge SD Cache", 0, [](lv_event_t*){
      while(sd_prune_stale_covers(true) > 0) {}
      for(int i = 0; i < core::s_profile_count; i++){
        if(core::s_profiles[i].avatar_buf){
          free(core::s_profiles[i].avatar_buf);
          core::s_profiles[i].avatar_buf = nullptr;
        }
      }
      open_settings();
    });
    lv_obj_set_size(clr, 165, 50);
  }

  // Set initial active tab visibility
  gu_tab_switch(s_gu_active_tab);

  lv_obj_t* old_s = s_scr_settings;
  s_scr_settings = scr;
  lv_scr_load(scr);
  if(old_s && old_s != scr) lv_obj_del_async(old_s);
}
