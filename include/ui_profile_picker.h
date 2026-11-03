#pragma once
// Child Profile Carousel and Avatar Loader
// StoryDeck Core multi-profile selection
#include <Arduino.h>
#include <lvgl.h>
#include "time.h"
#include "app_config.h"
#include "core_api.h"
#include "hal_display.h"
#include "cover_decoder.h"

extern lv_obj_t* s_scr_profile;
extern lv_obj_t* s_scr_home;
extern lv_obj_t* s_scr_prev;
extern int       s_card_count;
extern bool      fetch_library_core();
extern void      build_grid();
extern void      load_thumbs();


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

static void profile_card_cb(lv_event_t* e){
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if(idx >= 0 && idx < core::s_profile_count){
    core::s_active_profile_idx = idx;
    if(!cfg::lock_player && core::s_profiles[idx].default_player_id.length() > 0){
      cfg::device_id = core::s_profiles[idx].default_player_id;
    }
    // In Core mode, reload catalog for this child profile
    if(cfg::op_mode == "core" && cfg::core_url.length() > 0){
      fetch_library_core();
      build_grid();
      if(s_card_count > 0) load_thumbs();
    }
    // Transition from profile picker to card grid
    if(s_scr_profile){
      lv_obj_t* p = s_scr_profile;
      s_scr_profile = nullptr;
      if(s_scr_home) lv_scr_load(s_scr_home);
      lv_obj_del_async(p);
    }
  }
}

void open_profile_picker(){
  if(core::s_profile_count <= 0) return;

  lv_obj_t* scr = lv_obj_create(NULL);
  s_scr_profile = scr;
  lv_obj_set_style_bg_color(scr, lv_color_hex(0xFBF7F0), 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // Settings Gear button (top right)
  lv_obj_t* gear = lv_btn_create(scr);
  lv_obj_set_size(gear, 52, 52);
  lv_obj_set_pos(gear, LCD_H_RES - 68, 14);
  lv_obj_set_style_radius(gear, 26, 0);
  lv_obj_set_style_bg_color(gear, lv_color_hex(0xFFFFFF), 0);
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
  lv_obj_t* row = lv_obj_create(scr);
  lv_obj_set_size(row, LCD_H_RES - 60, 310);
  lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -25);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 10, 0);
  lv_obj_set_style_pad_column(row, 28, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scroll_dir(row, LV_DIR_HOR);

  for(int i = 0; i < core::s_profile_count; i++){
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

    // Profile Avatar (Circular container with initial or placeholder)
    lv_obj_t* av_box = lv_obj_create(card);
    lv_obj_set_size(av_box, 130, 130);
    lv_obj_set_style_radius(av_box, 65, 0);
    lv_obj_set_style_clip_corner(av_box, true, 0);
    lv_obj_set_style_bg_color(av_box, lv_color_hex(0xE5352B), 0);
    lv_obj_set_style_border_width(av_box, 0, 0);
    lv_obj_set_style_pad_all(av_box, 0, 0);
    lv_obj_align(av_box, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_clear_flag(av_box, LV_OBJ_FLAG_SCROLLABLE);

    // Check if avatar image is available and fetch/decode if not cached
    bool av_loaded = false;
    if(core::s_profiles[i].avatar_url.length() > 0){
      const size_t AV_SZ = 130 * 130 * 2;
      if(!core::s_profiles[i].avatar_buf){
        core::s_profiles[i].avatar_buf = (uint8_t*)heap_caps_malloc(AV_SZ, MALLOC_CAP_SPIRAM);
        if(core::s_profiles[i].avatar_buf){
          av_loaded = fetch_cover(core::s_profiles[i].avatar_url, 130, 130, core::s_profiles[i].avatar_buf);
          if(!av_loaded){
            free(core::s_profiles[i].avatar_buf);
            core::s_profiles[i].avatar_buf = nullptr;
          }
        }
      } else {
        av_loaded = true;
      }
    }

    if(av_loaded && core::s_profiles[i].avatar_buf){
      lv_obj_t* av_img = lv_img_create(av_box);
      lv_obj_set_size(av_img, 130, 130);
      set_img(core::s_profiles[i].avatar_dsc, av_img, core::s_profiles[i].avatar_buf, 130, 130, 130*130*2);
      lv_obj_center(av_img);
    } else {
      // Fallback: Avatar Initial
      lv_obj_t* av_txt = lv_label_create(av_box);
      char initial[2] = {core::s_profiles[i].display_name.length() > 0 ? core::s_profiles[i].display_name[0] : 'C', '\0'};
      lv_label_set_text(av_txt, initial);
      lv_obj_set_style_text_font(av_txt, &lv_font_montserrat_36, 0);
      lv_obj_set_style_text_color(av_txt, lv_color_hex(0xFFFFFF), 0);
      lv_obj_center(av_txt);
    }

    // Profile Name Label
    lv_obj_t* name = lv_label_create(card);
    lv_label_set_text(name, core::s_profiles[i].display_name.c_str());
    lv_obj_set_style_text_font(name, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(0x23303A), 0);
    lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -42);

    // Player assignment sub-label
    lv_obj_t* target = lv_label_create(card);
    lv_label_set_text(target, core::s_profiles[i].default_player_name.c_str());
    lv_obj_set_style_text_font(target, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(target, lv_color_hex(0x8A8276), 0);
    lv_obj_align(target, LV_ALIGN_BOTTOM_MID, 0, -18);
  }

  lv_scr_load(scr);
  lv_refr_now(lv_disp_get_default());
}

