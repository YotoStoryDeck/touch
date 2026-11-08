#pragma once
// Book Detail & Chapter List Screen (s_scr_detail) — Full Web Kiosk DetailScreen Parity
#include <Arduino.h>
#include <lvgl.h>
#include "app_state.h"
#include "storage_sd.h"
#include "cover_decoder.h"
#include "catalog_loader.h"
#include "ui_catalog.h"

static void back_cb(lv_event_t* e){
  cancel_detail_timer();
  s_detail_play_lbl = nullptr;
  s_detail_bar = nullptr;
  if(s_scr_home){
    lv_scr_load(s_scr_home);
  } else if(s_scr_error){
    lv_scr_load(s_scr_error);
  }
  if(s_scr_detail){
    lv_obj_del_async(s_scr_detail);
    s_scr_detail = nullptr;
  }
}

// (Re)build the tappable chapter rows from the current s_ch globals into `list`, clearing any
// existing rows first — so the same list can be refilled after a live fetch replaces the cached
// (or empty "loading") placeholder rendered when the screen first opened.
static void detail_fill_chapters(lv_obj_t* list){
  lv_obj_clean(list);
  for(int k=0;k<s_ch_count;k++){
    lv_obj_t* row = lv_obj_create(list);
    lv_obj_set_size(row, lv_pct(100), 58);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 2, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0xE7E0D2), 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 16, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    // press feedback: a soft red wash while the finger is down (rows are transparent at rest)
    lv_obj_set_style_bg_color(row, lv_color_hex(0xE5352B), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_10, LV_STATE_PRESSED);
    lv_obj_set_user_data(row, (void*)(intptr_t)k);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(row, [](lv_event_t* e){
      play_from((int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e)));
    }, LV_EVENT_CLICKED, NULL);

    bool is_ch_active = (s_playing && s_play_card == s_cur_card && s_play_ch == k);
    if(is_ch_active){
      lv_obj_set_style_bg_color(row, lv_color_hex(0xE0EDE7), 0);
      lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    }
    lv_obj_t* badge = lv_obj_create(row);
    lv_obj_set_size(badge, 36, 36);
    lv_obj_set_style_radius(badge, 18, 0);
    lv_obj_set_style_bg_color(badge, is_ch_active ? lv_color_hex(0x3AB06A) : lv_color_hex(0xFFB02E), 0);
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* bl = lv_label_create(badge);
    if(is_ch_active) lv_label_set_text(bl, LV_SYMBOL_PLAY);
    else             lv_label_set_text_fmt(bl, "%d", k+1);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(bl, is_ch_active ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x6A4400), 0);
    lv_obj_clear_flag(bl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(bl);

    lv_obj_t* ct = lv_label_create(row);
    lv_label_set_long_mode(ct, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(ct, 1);
    lv_label_set_text(ct, s_ch[k].title.c_str());
    lv_obj_set_style_text_font(ct, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ct, lv_color_hex(0x23303A), 0);
    lv_obj_clear_flag(ct, LV_OBJ_FLAG_CLICKABLE);

    char db[16]; fmt_mmss(db, s_ch[k].dur);
    lv_obj_t* cd = lv_label_create(row);
    lv_label_set_text(cd, db);
    lv_obj_set_style_text_font(cd, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(cd, lv_color_hex(0x9A9384), 0);
    lv_obj_clear_flag(cd, LV_OBJ_FLAG_CLICKABLE);

    // Explicit Play button on the right of each chapter row so the user can skip directly to that chapter
    lv_obj_t* ch_play = lv_btn_create(row);
    lv_obj_set_size(ch_play, 44, 44);
    lv_obj_set_style_radius(ch_play, 22, 0);
    lv_obj_set_style_bg_color(ch_play, is_ch_active ? lv_color_hex(0x3AB06A) : lv_color_hex(0xE5352B), 0);
    lv_obj_set_style_shadow_width(ch_play, 0, 0);
    lv_obj_set_style_pad_all(ch_play, 0, 0);
    lv_obj_clear_flag(ch_play, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_user_data(ch_play, (void*)(intptr_t)k);
    lv_obj_add_event_cb(ch_play, [](lv_event_t* e){
      lv_event_stop_bubbling(e);
      play_from((int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e)));
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_t* ch_pi = lv_label_create(ch_play);
    lv_label_set_text(ch_pi, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(ch_pi, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(ch_pi, lv_color_hex(0xFFFFFF), 0);
    lv_obj_clear_flag(ch_pi, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(ch_pi);
  }
}

static void detail_set_summary(lv_obj_t* sum){
  if(s_ch_count <= 0){ lv_label_set_text(sum, "Loading chapters..."); return; }
  int total=0; for(int k=0;k<s_ch_count;k++) total += s_ch[k].dur;
  char tb[32]; fmt_total(tb, total);
  lv_label_set_text_fmt(sum, "%d chapters   \xE2\x80\xA2   %s", s_ch_count, tb);
}

static void open_detail(int i){
  cancel_detail_timer();
  log_msg("[USER] Inspecting card %d: '%s' (%s) - prioritizing detail & tracks", i, s_cards[i].title.c_str(), s_cards[i].id.c_str());
  s_detail_play_lbl = nullptr;
  s_detail_bar = nullptr;
  s_cur_card = i;
  Card& c = s_cards[i];

  // 1) Chapters: SD cache first — instant and offline. On a miss, open with an empty
  //    "loading" list and fetch live AFTER the screen is painted (slow path below).
  bool have_ch = sd_chap_read(c.id.c_str());
  if(!have_ch || s_ch_count <= 0){ s_ch_count = 0; s_detail_title = c.title.c_str(); have_ch = false; }

  // 2) Detail cover: SD cache first. On a miss, fall back to the grid thumb (already decoded
  //    in PSRAM) upscaled in place as an instant placeholder; the sharp cover loads below.
  bool cover_sd = false;
  if(!s_dcover) s_dcover = (uint8_t*)heap_caps_malloc(DCOVER_SZ, MALLOC_CAP_SPIRAM);
  if(s_dcover) cover_sd = sd_cache_read(c.id.c_str(), DCOVER_W, s_dcover, DCOVER_SZ, c.cover.c_str());

  lv_obj_t* scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0xFBF7F0), 0);

  // ---- left white panel: cover + big Play (height 410px, leaving 70px for bottom Nowbar) ----
  lv_obj_t* left = lv_obj_create(scr);
  lv_obj_set_size(left, 286, 410);
  lv_obj_set_pos(left, 0, 0);
  lv_obj_set_style_bg_color(left, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_border_width(left, 0, 0);
  lv_obj_set_style_radius(left, 0, 0);
  lv_obj_set_style_pad_all(left, 8, 0);
  lv_obj_set_style_pad_row(left, 8, 0);
  lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(left, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* cover = lv_img_create(left);
  lv_obj_set_size(cover, DCOVER_W, DCOVER_H);
  lv_obj_set_style_radius(cover, 14, 0);
  lv_obj_set_style_clip_corner(cover, true, 0);
  lv_obj_set_style_bg_color(cover, lv_color_hex(0xE5352B), 0);
  lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
  if(cover_sd){
    set_img(s_dcover_dsc, cover, s_dcover, DCOVER_W, DCOVER_H, DCOVER_SZ);
  } else if(c.pool_slot >= 0 && s_thumb_pool[c.pool_slot].card_idx == i && s_thumb_pool[c.pool_slot].buf){
    lv_img_set_src(cover, &s_thumb_pool[c.pool_slot].dsc);
    lv_img_set_pivot(cover, THUMB_W/2, THUMB_H/2);
    lv_img_set_zoom(cover, (uint16_t)(256 * DCOVER_W / THUMB_W));
  }

  bool is_curr_playing = (s_playing && s_play_card == i);
  lv_obj_t* play = lv_btn_create(left);
  lv_obj_set_size(play, DCOVER_W, 52);
  lv_obj_set_style_bg_color(play, is_curr_playing ? lv_color_hex(0x23303A) : lv_color_hex(0xE5352B), 0);
  lv_obj_set_style_radius(play, 16, 0);
  lv_obj_t* pl = lv_label_create(play);
  s_detail_play_lbl = pl;
  lv_label_set_text(pl, is_curr_playing ? (LV_SYMBOL_PAUSE "  Now Playing") : (LV_SYMBOL_PLAY "  Play Book"));
  lv_obj_set_style_text_font(pl, &lv_font_montserrat_20, 0);
  lv_obj_center(pl);
  lv_obj_add_event_cb(play, [](lv_event_t*){
    // If this book is already actively playing, open the Now Playing screen instead of restarting
    if(s_playing && s_play_card == s_cur_card){
      if(s_play_ch >= 0) s_cur_ch = s_play_ch;
      open_now();
      return;
    }
    // Pass -1 to resume where the profile/player left off (omits chapter in /api/play)
    play_from(-1);
  }, LV_EVENT_CLICKED, NULL);

  // ---- right panel: title + chapter list (height 410px above bottom Nowbar) ----
  lv_obj_t* right = lv_obj_create(scr);
  lv_obj_set_size(right, 514, 410);
  lv_obj_set_pos(right, 286, 0);
  lv_obj_set_style_bg_color(right, lv_color_hex(0xFBF7F0), 0);
  lv_obj_set_style_border_width(right, 0, 0);
  lv_obj_set_style_radius(right, 0, 0);
  lv_obj_set_style_pad_left(right, 18, 0);
  lv_obj_set_style_pad_right(right, 18, 0);
  lv_obj_set_style_pad_top(right, 14, 0);
  lv_obj_set_style_pad_bottom(right, 6, 0);
  lv_obj_set_style_pad_row(right, 4, 0);
  lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
  lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(right);
  lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(title, 478);
  lv_label_set_text(title, s_detail_title.c_str());
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xE5352B), 0);

  lv_obj_t* sub = lv_label_create(right);
  String sub_txt = c.author.length() ? c.author.c_str() : (c.is_abs() ? "Audiobookshelf" : "Yoto");
  lv_label_set_text(sub, sub_txt.c_str());
  lv_obj_set_style_text_font(sub, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(sub, lv_color_hex(0x7A7060), 0);

  lv_obj_t* sum_row = lv_obj_create(right);
  lv_obj_set_size(sum_row, lv_pct(100), 32);
  lv_obj_set_style_bg_opa(sum_row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(sum_row, 0, 0);
  lv_obj_set_style_pad_all(sum_row, 0, 0);
  lv_obj_set_style_pad_column(sum_row, 10, 0);
  lv_obj_set_flex_flow(sum_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(sum_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(sum_row, LV_OBJ_FLAG_SCROLLABLE);

  bool is_abs_book = c.is_abs();
  lv_obj_t* src_badge = lv_label_create(sum_row);
  lv_label_set_text(src_badge, is_abs_book ? "Audiobookshelf" : "Yoto");

  if(c.series.length() > 0){
    lv_obj_t* sbtn = lv_btn_create(sum_row);
    lv_obj_set_style_bg_color(sbtn, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_bg_color(sbtn, lv_color_hex(0xE5352B), LV_STATE_PRESSED);
    lv_obj_set_style_radius(sbtn, 12, 0);
    lv_obj_set_style_pad_left(sbtn, 10, 0);
    lv_obj_set_style_pad_right(sbtn, 10, 0);
    lv_obj_set_style_pad_top(sbtn, 4, 0);
    lv_obj_set_style_pad_bottom(sbtn, 4, 0);
    lv_obj_set_style_shadow_width(sbtn, 0, 0);
    lv_obj_t* slbl = lv_label_create(sbtn);
    String stxt = "Series: " + String(c.series.c_str()) + " >";
    lv_label_set_text(slbl, stxt.c_str());
    lv_obj_set_style_text_font(slbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(slbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(slbl);
    lv_obj_add_event_cb(sbtn, [](lv_event_t*){
      Card& cur_c = s_cards[s_cur_card];
      if(cur_c.series.length() > 0){
        open_series_view(cur_c.series);
      }
    }, LV_EVENT_CLICKED, NULL);
  }
  lv_obj_set_style_text_font(src_badge, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(src_badge, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_bg_color(src_badge, is_abs_book ? lv_color_hex(0x4F46E5) : lv_color_hex(0xE5352B), 0);
  lv_obj_set_style_bg_opa(src_badge, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(src_badge, 6, 0);
  lv_obj_set_style_pad_left(src_badge, 8, 0);
  lv_obj_set_style_pad_right(src_badge, 8, 0);
  lv_obj_set_style_pad_top(src_badge, 3, 0);
  lv_obj_set_style_pad_bottom(src_badge, 3, 0);
  lv_obj_clear_flag(src_badge, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t* sum = lv_label_create(sum_row);
  detail_set_summary(sum);
  lv_obj_set_style_text_font(sum, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(sum, lv_color_hex(0x3A6B50), 0);

  lv_obj_t* list = lv_obj_create(right);
  lv_obj_set_width(list, lv_pct(100));
  lv_obj_set_flex_grow(list, 1);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(list, 0, 0);
  lv_obj_set_style_pad_all(list, 0, 0);
  lv_obj_set_style_pad_right(list, 6, 0);
  lv_obj_set_style_pad_row(list, 0, 0);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);

  detail_fill_chapters(list);   // cached rows now, or empty until the fetch below returns

  // ---- bottom Now Playing Mini-Bar (mirroring Kiosk DetailScreen.render() line 60) ----
  lv_obj_t* dbar = lv_obj_create(scr);
  s_detail_bar = dbar;
  lv_obj_set_size(dbar, LCD_H_RES, 70);
  lv_obj_align(dbar, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(dbar, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_border_side(dbar, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_border_width(dbar, 1, 0);
  lv_obj_set_style_border_color(dbar, lv_color_hex(0xE7E0D2), 0);
  lv_obj_set_style_radius(dbar, 0, 0);
  lv_obj_set_style_pad_all(dbar, 0, 0);
  lv_obj_clear_flag(dbar, LV_OBJ_FLAG_SCROLLABLE);
  populate_nowbar_obj(dbar, false);

  // ---- back button (floats over the left panel) ----
  lv_obj_t* back = lv_btn_create(scr);
  lv_obj_set_size(back, 52, 52);
  lv_obj_set_pos(back, 14, 14);
  lv_obj_set_style_radius(back, 26, 0);
  lv_obj_set_style_bg_color(back, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_shadow_width(back, 8, 0);
  lv_obj_set_style_shadow_opa(back, LV_OPA_30, 0);
  lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* bk = lv_label_create(back);
  lv_label_set_text(bk, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_font(bk, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(bk, lv_color_hex(0x23303A), 0);
  lv_obj_center(bk);

  lv_obj_t* oldd = s_scr_detail; s_scr_detail = scr;
  lv_obj_t* oldn = s_scr_now;    s_scr_now = nullptr;
  s_np_barfill = s_np_tcur = s_np_trem = s_np_playicon = s_np_chapter = s_np_sleep = s_np_vol_slider = s_np_vol_limit = s_np_vol_label = nullptr;
  lv_scr_load(scr);
  if(oldd) lv_obj_del_async(oldd);
  if(oldn) lv_obj_del_async(oldn);
  lv_refr_now(lv_disp_get_default());   // paint the screen NOW so the tap feels instant

  // ---- async slow path: defer heavy network / JPEG operations to a shallow timer ----
  if(!have_ch || (!cover_sd && s_dcover)){
    DetailAsyncCtx* ctx = new DetailAsyncCtx{
      i, !have_ch, (!cover_sd && s_dcover != nullptr),
      scr, title, sum, list, cover
    };
    s_detail_async_tmr = lv_timer_create([](lv_timer_t* t){
      s_detail_async_tmr = nullptr;
      DetailAsyncCtx* c = (DetailAsyncCtx*)t->user_data;
      lv_timer_del(t);
      if(!c) return;
      if(s_scr_detail != c->scr_target){
        delete c;
        return;
      }
      Card& card = s_cards[c->card_idx];
      if(c->need_ch){
        if(fetch_card_detail(c->card_idx) && s_ch_count > 0){
          if(s_scr_detail == c->scr_target){
            lv_label_set_text(c->title_lbl, s_detail_title.c_str());
            detail_set_summary(c->sum_lbl);
            detail_fill_chapters(c->list_box);
          }
        } else if(s_scr_detail == c->scr_target){
          s_ch_count = 1;
          s_ch[0].key = "01";
          s_ch[0].track_key = "01";
          s_ch[0].title = card.title.length() ? card.title.c_str() : "Full Audiobook";
          s_ch[0].dur = 1800;
          detail_set_summary(c->sum_lbl);
          detail_fill_chapters(c->list_box);
          sd_chap_write(card.id.c_str());
        }
      }
      if(c->need_cover && s_scr_detail == c->scr_target && s_dcover){
        if(load_cover_cached(card.id.c_str(), card.cover.c_str(), DCOVER_W, DCOVER_H, DCOVER_SZ, s_dcover)){
          lv_img_set_zoom(c->cover_img, 256);
          lv_img_set_pivot(c->cover_img, 0, 0);
          set_img(s_dcover_dsc, c->cover_img, s_dcover, DCOVER_W, DCOVER_H, DCOVER_SZ);
        }
      }
      delete c;
    }, 40, ctx);
    lv_timer_set_repeat_count(s_detail_async_tmr, 1);
  }
}
