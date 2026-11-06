#pragma once
// Full-Screen Now Playing UI, Progress/Chapter Label Updaters, and Confetti Animation
#include <Arduino.h>
#include <lvgl.h>
#include "app_state.h"
#include "cover_decoder.h"

void np_next();
void np_prev();

static void np_update_progress(){
  if(s_np_vol_slider){
    int vmax = get_active_player_vol_max();
    lv_slider_set_range(s_np_vol_slider, 0, vmax);
    if(!lv_obj_has_state(s_np_vol_slider, LV_STATE_EDITED) && !lv_obj_has_state(s_np_vol_slider, LV_STATE_FOCUSED)){
      lv_slider_set_value(s_np_vol_slider, s_vol > vmax ? vmax : s_vol, LV_ANIM_OFF);
    }
  }
  if(!s_np_barfill) return;
  int dur = s_play_dur;
  int pct = dur>0 ? (int)(100L*s_pos/dur) : 0;
  if(pct>100) pct=100;
  lv_obj_set_width(s_np_barfill, lv_pct(pct));
  if(s_np_tcur){ char b[16]; fmt_mmss(b, s_pos); lv_label_set_text(s_np_tcur, b); }
  if(s_np_trem){ char b[16]; fmt_mmss(b, dur-s_pos); lv_label_set_text_fmt(s_np_trem, "-%s", b); }
}

static void np_set_chapter_label(){
  if(!s_np_chapter) return;
  String ch_t = (s_cur_ch >= 0 && s_cur_ch < s_ch_count) ? s_ch[s_cur_ch].title : s_play_chapter;
  if(s_play_author.length() > 0 && ch_t.length() > 0){
    lv_label_set_text_fmt(s_np_chapter, "%s  \xE2\x80\xA2  %s", s_play_author.c_str(), ch_t.c_str());
  } else if(ch_t.length() > 0){
    lv_label_set_text(s_np_chapter, ch_t.c_str());
  } else if(s_play_author.length() > 0){
    lv_label_set_text(s_np_chapter, s_play_author.c_str());
  } else {
    lv_label_set_text(s_np_chapter, "Playing");
  }
}

static void np_set_playicon(){
  if(s_np_playicon) lv_label_set_text(s_np_playicon, s_playing? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

// A one-second burst of soft confetti dots floating up the now-playing screen
static void confetti_y(void* o, int32_t v){ lv_obj_set_y((lv_obj_t*)o, v); }
static void confetti_o(void* o, int32_t v){ lv_obj_set_style_opa((lv_obj_t*)o, v, 0); }
static void confetti_done(lv_anim_t* a){ lv_obj_del((lv_obj_t*)a->var); }
static void confetti(lv_obj_t* scr){
  static const uint32_t cols[4] = {0xFFB02E, 0xFFFFFF, 0xFBF7F0, 0xFF8A7E};
  for(int i=0;i<12;i++){
    int sz = 8 + (rand() % 8);
    lv_obj_t* d = lv_obj_create(scr);
    lv_obj_set_size(d, sz, sz);
    lv_obj_set_style_radius(d, sz/2, 0);
    lv_obj_set_style_bg_color(d, lv_color_hex(cols[i & 3]), 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(d, 80 + (rand() % 640), LCD_V_RES);
    uint32_t dur = 700 + (rand() % 500);
    lv_anim_t a; lv_anim_init(&a);
    lv_anim_set_var(&a, d);
    lv_anim_set_time(&a, dur);
    lv_anim_set_delay(&a, rand() % 250);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, confetti_y);
    lv_anim_set_values(&a, LCD_V_RES, 60 + (rand() % 180));
    lv_anim_set_ready_cb(&a, confetti_done);
    lv_anim_start(&a);
    lv_anim_t b = a;
    lv_anim_set_exec_cb(&b, confetti_o);
    lv_anim_set_values(&b, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_ready_cb(&b, NULL);
    lv_anim_start(&b);
  }
}

static void open_now(){
  bool okv=false;
  if(!s_ncover) s_ncover=(uint8_t*)heap_caps_malloc(NCOVER_SZ, MALLOC_CAP_SPIRAM);
  if(s_ncover){
    if(s_cur_card >= 0 && s_cur_card < s_card_count){
      okv=load_cover_cached(s_cards[s_cur_card].id.c_str(), s_cards[s_cur_card].cover.c_str(), NCOVER_W, NCOVER_H, NCOVER_SZ, s_ncover);
    } else {
      String cid = s_play_item_id.length() > 0 ? s_play_item_id : "ext_card";
      okv=load_cover_cached(cid, s_play_cover, NCOVER_W, NCOVER_H, NCOVER_SZ, s_ncover);
    }
  }

  lv_obj_t* scr=lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0xE5352B), 0);
  lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0xB3211A), 0);
  lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // top: chevron-down (home) + label
  lv_obj_t* chev=lv_btn_create(scr);
  lv_obj_set_size(chev, 56, 56); lv_obj_set_pos(chev, 12, 10);
  lv_obj_set_style_bg_opa(chev, LV_OPA_TRANSP, 0);
  lv_obj_set_style_shadow_width(chev, 0, 0);
  lv_obj_add_event_cb(chev, [](lv_event_t*){ go_home(); }, LV_EVENT_CLICKED, NULL);
  lv_obj_t* cl=lv_label_create(chev); lv_label_set_text(cl, LV_SYMBOL_DOWN);
  lv_obj_set_style_text_font(cl, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(cl, lv_color_hex(0xFFFFFF), 0); lv_obj_center(cl);

  lv_obj_t* top=lv_label_create(scr);
  lv_label_set_text_fmt(top, "Playing on %s", get_active_player_name().c_str());
  lv_obj_set_style_text_font(top, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(top, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 24);

  // sleep-timer chip (top right): "Zz 23 min" while the bedtime timer runs, blank otherwise
  s_np_sleep=lv_label_create(scr);
  if(s_sleep_sec > 0) lv_label_set_text_fmt(s_np_sleep, "Zz %d min", (s_sleep_sec + 59) / 60);
  else                lv_label_set_text(s_np_sleep, "");
  lv_obj_set_style_text_font(s_np_sleep, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(s_np_sleep, lv_color_hex(0xFFD9A0), 0);
  lv_obj_align(s_np_sleep, LV_ALIGN_TOP_RIGHT, -18, 24);

  // cover (clickable: returns to book detail / tracklist view)
  lv_obj_t* cover=lv_img_create(scr);
  lv_obj_set_size(cover, NCOVER_W, NCOVER_H); lv_obj_set_pos(cover, 44, 120);
  lv_obj_set_style_radius(cover, 14, 0); lv_obj_set_style_clip_corner(cover, true, 0);
  lv_obj_set_style_bg_color(cover, lv_color_hex(0xB3211A), 0);
  lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
  lv_obj_add_flag(cover, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(cover, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(cover, [](lv_event_t*){
    if(s_cur_card >= 0 && s_cur_card < s_card_count) open_detail(s_cur_card);
    else go_home();
  }, LV_EVENT_CLICKED, NULL);
  if(okv) set_img(s_ncover_dsc, cover, s_ncover, NCOVER_W, NCOVER_H, NCOVER_SZ);

  const int ix=224, iw=536;
  lv_obj_t* tt=lv_label_create(scr);
  lv_label_set_long_mode(tt, LV_LABEL_LONG_DOT);
  lv_obj_set_width(tt, iw); lv_obj_set_pos(tt, ix, 124);
  lv_label_set_text(tt, s_detail_title.length() > 0 ? s_detail_title.c_str() : s_play_title.c_str());
  lv_obj_set_style_text_font(tt, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(tt, lv_color_hex(0xFFFFFF), 0);
  lv_obj_add_flag(tt, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(tt, [](lv_event_t*){
    if(s_cur_card >= 0 && s_cur_card < s_card_count) open_detail(s_cur_card);
    else go_home();
  }, LV_EVENT_CLICKED, NULL);

  s_np_chapter=lv_label_create(scr);
  lv_label_set_long_mode(s_np_chapter, LV_LABEL_LONG_DOT);
  lv_obj_set_width(s_np_chapter, iw); lv_obj_set_pos(s_np_chapter, ix, 166);
  lv_obj_set_style_text_font(s_np_chapter, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(s_np_chapter, lv_color_hex(0xFFFFFF), 0);
  np_set_chapter_label();

  // progress
  lv_obj_t* track=lv_obj_create(scr);
  lv_obj_set_size(track, iw, 12); lv_obj_set_pos(track, ix, 224);
  lv_obj_set_style_radius(track, 6, 0);
  lv_obj_set_style_bg_color(track, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_bg_opa(track, LV_OPA_30, 0);
  lv_obj_set_style_border_width(track, 0, 0);
  lv_obj_set_style_pad_all(track, 0, 0);
  lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);
  s_np_barfill=lv_obj_create(track);
  lv_obj_set_size(s_np_barfill, lv_pct(0), 12);
  lv_obj_align(s_np_barfill, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_set_style_radius(s_np_barfill, 6, 0);
  lv_obj_set_style_bg_color(s_np_barfill, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_border_width(s_np_barfill, 0, 0);
  lv_obj_clear_flag(s_np_barfill, LV_OBJ_FLAG_SCROLLABLE);

  s_np_tcur=lv_label_create(scr);
  lv_obj_set_pos(s_np_tcur, ix, 242);
  lv_obj_set_style_text_font(s_np_tcur, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(s_np_tcur, lv_color_hex(0xFFFFFF), 0);
  s_np_trem=lv_label_create(scr);
  lv_obj_set_pos(s_np_trem, ix+iw-58, 242);
  lv_obj_set_style_text_font(s_np_trem, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(s_np_trem, lv_color_hex(0xFFFFFF), 0);

  // controls — big transport buttons (72/84px) with 36px glyphs for easy little-finger taps
  const int cy=292;
  lv_obj_t* prev=lv_btn_create(scr);
  lv_obj_set_size(prev, 72, 72); lv_obj_set_pos(prev, ix, cy);
  lv_obj_set_style_bg_opa(prev, LV_OPA_TRANSP, 0); lv_obj_set_style_shadow_width(prev,0,0);
  lv_obj_add_event_cb(prev, [](lv_event_t*){ np_prev(); }, LV_EVENT_CLICKED, NULL);
  lv_obj_t* pv=lv_label_create(prev); lv_label_set_text(pv, LV_SYMBOL_PREV);
  lv_obj_set_style_text_font(pv,&lv_font_montserrat_36,0);
  lv_obj_set_style_text_color(pv,lv_color_hex(0xFFFFFF),0); lv_obj_center(pv);

  lv_obj_t* big=lv_btn_create(scr);
  lv_obj_set_size(big, 84, 84); lv_obj_set_pos(big, ix+84, cy-6);
  lv_obj_set_style_radius(big, 42, 0);
  lv_obj_set_style_bg_color(big, lv_color_hex(0xFFFFFF), 0);
  lv_obj_add_event_cb(big, [](lv_event_t*){ np_toggle(); }, LV_EVENT_CLICKED, NULL);
  s_np_playicon=lv_label_create(big);
  lv_obj_set_style_text_font(s_np_playicon,&lv_font_montserrat_36,0);
  lv_obj_set_style_text_color(s_np_playicon,lv_color_hex(0xE5352B),0); lv_obj_center(s_np_playicon);
  np_set_playicon();

  lv_obj_t* next=lv_btn_create(scr);
  lv_obj_set_size(next, 72, 72); lv_obj_set_pos(next, ix+186, cy);
  lv_obj_set_style_bg_opa(next, LV_OPA_TRANSP, 0); lv_obj_set_style_shadow_width(next,0,0);
  lv_obj_add_event_cb(next, [](lv_event_t*){ np_next(); }, LV_EVENT_CLICKED, NULL);
  lv_obj_t* nx=lv_label_create(next); lv_label_set_text(nx, LV_SYMBOL_NEXT);
  lv_obj_set_style_text_font(nx,&lv_font_montserrat_36,0);
  lv_obj_set_style_text_color(nx,lv_color_hex(0xFFFFFF),0); lv_obj_center(nx);

  // volume — its own row below the transport, with a thicker, longer slider that's easy to drag
  lv_obj_t* vi=lv_label_create(scr);
  lv_label_set_text(vi, LV_SYMBOL_VOLUME_MID);
  lv_obj_set_style_text_font(vi,&lv_font_montserrat_24,0);
  lv_obj_set_style_text_color(vi,lv_color_hex(0xFFFFFF),0);
  lv_obj_set_pos(vi, ix, cy+108);
  lv_obj_t* slider=lv_slider_create(scr);
  lv_obj_set_size(slider, 260, 16); lv_obj_set_pos(slider, ix+44, cy+112);
  // the parent-set cap IS the top of the slider — full-right now means "as loud as allowed",
  // instead of showing 100 while the command silently clamps to the cap
  int vmax = get_active_player_vol_max();
  lv_slider_set_range(slider, 0, vmax);
  lv_slider_set_value(slider, s_vol > vmax ? vmax : s_vol, LV_ANIM_OFF);
  s_np_vol_slider = slider;
  lv_obj_set_style_bg_color(slider, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(slider, LV_OPA_40, LV_PART_MAIN);
  lv_obj_set_style_bg_color(slider, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
  lv_obj_set_style_pad_all(slider, 10, LV_PART_KNOB);   // grow the knob (~36px) so it's easy to grab
  lv_obj_add_event_cb(slider, [](lv_event_t* e){
    np_volume(lv_slider_get_value(lv_event_get_target(e)));
  }, LV_EVENT_RELEASED, NULL);

  np_update_progress();

  lv_obj_t* oldd=s_scr_detail; s_scr_detail=nullptr;
  lv_obj_t* oldn=s_scr_now;    s_scr_now=scr;
  lv_scr_load(scr);
  if(oldd) lv_obj_del_async(oldd);   // detail screen owns the row firing this event
  if(oldn) lv_obj_del_async(oldn);
}
