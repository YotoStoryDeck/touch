#pragma once
// Child Profile Carousel and Avatar Loader
// StoryDeck Core multi-profile selection (100% parity with web-touch/js/screens/profile.js)
#include <Arduino.h>
#include <lvgl.h>
#include "time.h"
#include "app_config.h"
#include "core_api.h"
#include "hal_display.h"
#include "app_state.h"
#include "cover_decoder.h"

extern lv_obj_t* s_scr_profile;
extern lv_obj_t* s_scr_home;
extern lv_obj_t* s_scr_prev;
extern int       s_card_count;
extern bool      fetch_library_core();
extern void      build_grid();
extern void      load_thumbs();
extern void      update_home_bar();


// ---------------- Child Profile Selection Screen (profile.js parity) ----------------
static const char* get_time_greeting(){
  time_t now = time(nullptr);
  struct tm tmv;
  if(localtime_r(&now, &tmv)){
    int h = tmv.tm_hour;
    if(h >= 5 && h < 12) return "WHO IS LISTENING THIS MORNING?";
    if(h >= 12 && h < 17) return "WHO IS LISTENING THIS AFTERNOON?";
    return "WHO IS LISTENING TONIGHT?";
  }
  return "WHO IS LISTENING?";
}

// Kiosk App.syncActiveDevicePlayback() parity: sync target player's live volume/playing state on profile switch
static void sync_active_device_playback(){
  const core::PlayerInfo* pi = core::find_player_info(cfg::device_id);
  if(!pi) return;
  if(pi->volume > 0){
    int vmax = pi->volume_max > 0 ? pi->volume_max : cfg::vol_max;
    s_vol = pi->volume > vmax ? vmax : pi->volume;
  }
  if(pi->is_playing){
    s_playing = true;
  }
}

static void profile_card_cb(lv_event_t* e){
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if(idx >= 0 && idx < core::s_profile_count){
    core::s_active_profile_idx = idx;
    cfg::device_id = core::resolve_active_device_id(idx);
    cfg::save();
    sync_active_device_playback();

    // Load s_scr_home and schedule s_scr_profile deletion before rebuilding the catalog grid
    if(s_scr_profile){
      lv_obj_t* p = s_scr_profile;
      s_scr_profile = nullptr;
      if(s_scr_home) lv_scr_load(s_scr_home);
      lv_obj_del_async(p);
    }
    // Fetch catalog JSON and build grid tiles
    if(cfg::op_mode == "core" && cfg::core_url.length() > 0){
      fetch_library_core();
      build_grid();
    }
    if(s_scr_home){
      lv_scr_load(s_scr_home);
      lv_refr_now(lv_disp_get_default());
    }
    // Now stream/decode covers with visible tile-by-tile progress
    if(cfg::op_mode == "core" && s_card_count > 0){
      load_thumbs();
    }
  }
}

void open_profile_picker(){
  if(core::s_profile_count <= 0) return;

  lv_obj_t* scr = lv_obj_create(NULL);
  s_scr_profile = scr;
  lv_obj_set_style_bg_color(scr, lv_color_hex(0xFBF7F0), 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // Settings Gear button (top right) -> opens Parent PIN modal -> Grown-Ups Settings
  lv_obj_t* gear = lv_btn_create(scr);
  lv_obj_set_size(gear, 52, 52);
  lv_obj_set_pos(gear, LCD_H_RES - 68, 14);
  lv_obj_set_style_radius(gear, 26, 0);
  lv_obj_set_style_bg_color(gear, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_bg_color(gear, lv_color_hex(0xE5352B), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(gear, 8, 0);
  lv_obj_set_style_shadow_opa(gear, LV_OPA_30, 0);
  lv_obj_add_event_cb(gear, [](lv_event_t*){ s_scr_prev = s_scr_profile; open_pin_modal(false); }, LV_EVENT_CLICKED, NULL);
  lv_obj_t* gl = lv_label_create(gear);
  lv_label_set_text(gl, LV_SYMBOL_SETTINGS);
  lv_obj_set_style_text_font(gl, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(gl, lv_color_hex(0x23303A), 0);
  lv_obj_center(gl);

  // Greeting Banner
  lv_obj_t* title = lv_label_create(scr);
  lv_label_set_text(title, get_time_greeting());
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xE5352B), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 36);

  lv_obj_t* sub = lv_label_create(scr);
  lv_label_set_text(sub, "Tap your profile avatar to explore your audiobooks");
  lv_obj_set_style_text_font(sub, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(sub, lv_color_hex(0x7A7060), 0);
  lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 78);

  // Horizontal Scrolling Row for Profile Cards
  // IMPORTANT: Use LV_FLEX_ALIGN_START when >3 profiles so LVGL doesn't place overflowing cards at
  // negative X offsets and snap back to center on release! For <=3 cards, center them cleanly.
  lv_obj_t* row = lv_obj_create(scr);
  lv_obj_set_size(row, LCD_H_RES - 40, 310);
  lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -25);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_top(row, 10, 0);
  lv_obj_set_style_pad_bottom(row, 10, 0);
  lv_obj_set_style_pad_left(row, 24, 0);
  lv_obj_set_style_pad_right(row, 24, 0);
  lv_obj_set_style_pad_column(row, 28, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_flex_align_t row_align = (core::s_profile_count <= 3) ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_START;
  lv_obj_set_flex_align(row, row_align, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scroll_dir(row, LV_DIR_HOR);
  lv_obj_set_scroll_snap_x(row, LV_SCROLL_SNAP_NONE);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLL_ELASTIC);

  for(int i = 0; i < core::s_profile_count; i++){
    core::Profile& pr = core::s_profiles[i];
    lv_obj_t* card = lv_btn_create(row);
    lv_obj_set_size(card, 180, 260);
    lv_obj_set_style_radius(card, 22, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0xF3E9D8), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0xE7E0D2), 0);
    lv_obj_set_style_shadow_width(card, 12, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_20, 0);
    lv_obj_add_event_cb(card, profile_card_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);

    // Profile Avatar (Circular container with profile's custom color and monogram/photo)
    lv_obj_t* av_box = lv_obj_create(card);
    lv_obj_set_size(av_box, 130, 130);
    lv_obj_set_style_radius(av_box, 65, 0);
    lv_obj_set_style_clip_corner(av_box, true, 0);
    lv_obj_set_style_bg_color(av_box, lv_color_hex(pr.avatar_color), 0);
    lv_obj_set_style_border_width(av_box, 0, 0);
    lv_obj_set_style_pad_all(av_box, 0, 0);
    lv_obj_align(av_box, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_clear_flag(av_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Only fetch/decode raster avatar if a custom uploaded photo URL is present (e.g. ?t=... or external http image).
    // For default generated initial avatars (/api/avatar/<id> without ?t=), crisp native LVGL vector font looks
    // sharper than a scaled 160->130 JPEG!
    bool has_custom_photo = (pr.avatar_url.indexOf("?t=") >= 0) ||
                            (pr.avatar_url.length() > 0 && pr.avatar_url.indexOf("/api/avatar/") < 0);
    bool av_loaded = false;
    if(has_custom_photo){
      const size_t AV_SZ = 130 * 130 * 2;
      if(!pr.avatar_buf){
        pr.avatar_buf = (uint8_t*)heap_caps_malloc(AV_SZ, MALLOC_CAP_SPIRAM);
        if(pr.avatar_buf){
          av_loaded = load_cover_cached(String("av_") + pr.id, pr.avatar_url, 130, 130, AV_SZ, pr.avatar_buf);
          if(!av_loaded){
            free(pr.avatar_buf);
            pr.avatar_buf = nullptr;
          }
        }
      } else {
        av_loaded = true;
      }
    }

    if(av_loaded && pr.avatar_buf){
      lv_obj_t* av_img = lv_img_create(av_box);
      lv_obj_set_size(av_img, 130, 130);
      set_img(pr.avatar_dsc, av_img, pr.avatar_buf, 130, 130, 130*130*2);
      lv_obj_clear_flag(av_img, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_center(av_img);
    } else {
      // Crisp vector monogram using Core's household disambiguated initial (e.g. "J", "W", "Ja", "Jm")
      lv_obj_t* av_txt = lv_label_create(av_box);
      String mono = pr.avatar_initial.length() > 0 ? pr.avatar_initial : (pr.display_name.length() > 0 ? pr.display_name.substring(0, 1) : String("C"));
      lv_label_set_text(av_txt, mono.c_str());
      lv_obj_set_style_text_font(av_txt, &lv_font_montserrat_36, 0);
      lv_obj_set_style_text_color(av_txt, lv_color_hex(0xFFFFFF), 0);
      lv_obj_clear_flag(av_txt, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_center(av_txt);
    }

    // Profile Name Label
    lv_obj_t* name = lv_label_create(card);
    lv_label_set_text(name, pr.display_name.c_str());
    lv_obj_set_style_text_font(name, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(0x23303A), 0);
    lv_obj_clear_flag(name, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -42);

    // Player assignment sub-label (shows Locked player if Hardware Lock is active, else profile default player)
    lv_obj_t* target = lv_label_create(card);
    String sub_player = cfg::lock_player ? get_active_player_name() : pr.default_player_name;
    lv_label_set_text(target, sub_player.c_str());
    lv_obj_set_style_text_font(target, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(target, lv_color_hex(0x8A8276), 0);
    lv_obj_clear_flag(target, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(target, LV_ALIGN_BOTTOM_MID, 0, -18);
  }

  lv_scr_load(scr);
  lv_refr_now(lv_disp_get_default());
}
