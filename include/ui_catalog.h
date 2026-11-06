#pragma once
// Home Catalog Screen: Top Navigation Bar (Profile, Target Player, All/Series Tabs, Gear),
// Target Player Picker Dropdown, Card Tiles & Badges, Series Grouping, and Now Playing Mini-Bar
#include <Arduino.h>
#include <lvgl.h>
#include "app_state.h"
#include "storage_sd.h"

static void tile_cb(lv_event_t* e){
  int i=(int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
  if(i<0 || i>=s_card_count) return;
  if(s_cards[i].daily) play_yoto_daily(i);   // one tap → today's episode (no chapter list)
  else                 open_detail(i);
}


static bool is_played_past_7_days(const PsramString& lp){
  if(lp.empty()) return false;
  time_t now = time(nullptr);
  if(now > 1700000000){
    time_t cutoff = now - (7 * 86400);
    if(lp.length() >= 10 && lp[4] == '-' && lp[7] == '-'){
      struct tm tm_lp = {};
      tm_lp.tm_year = atoi(lp.substr(0, 4).c_str()) - 1900;
      tm_lp.tm_mon  = atoi(lp.substr(5, 2).c_str()) - 1;
      tm_lp.tm_mday = atoi(lp.substr(8, 2).c_str());
      if(lp.length() >= 19 && (lp[10] == 'T' || lp[10] == ' ')){
        tm_lp.tm_hour = atoi(lp.substr(11, 2).c_str());
        tm_lp.tm_min  = atoi(lp.substr(14, 2).c_str());
        tm_lp.tm_sec  = atoi(lp.substr(17, 2).c_str());
      }
      time_t lp_epoch = mktime(&tm_lp);
      return (lp_epoch >= cutoff);
    }
    struct tm tm_cut;
    gmtime_r(&cutoff, &tm_cut);
    char cutoff_date[12];
    strftime(cutoff_date, sizeof(cutoff_date), "%Y-%m-%d", &tm_cut);
    return (lp.substr(0, 10) >= cutoff_date);
  }
  return true;
}

static bool card_matches_active_tab(const Card& c){
  if(s_active_tab == TAB_ALL) return true;
  if(s_active_tab == TAB_SERIES) return (c.series.length() > 0);
  if(s_active_tab == TAB_RECENT){
    if(c.daily) return true;
    return is_played_past_7_days(c.lp);
  }
  if(s_active_tab == TAB_YOTO) return (!c.is_abs() || c.daily);
  if(s_active_tab == TAB_ABS)  return c.is_abs();
  return true;
}


static lv_obj_t* s_grid = nullptr;
static lv_obj_t* s_tab_btns[4] = {nullptr, nullptr, nullptr, nullptr};
static void populate_grid(); // fwd

static void tab_btn_cb(lv_event_t* e){
  int tab = (int)(intptr_t)lv_event_get_user_data(e);
  if(tab < 0 || tab > 2) return;
  s_selected_series_filter = "";
  const char* tname = (tab == 1) ? "Series" : (tab == 2 ? "Recently Played" : "Catalog");
  Serial.printf("[TAB] Switched catalog tab to %d (%s)\n", tab, tname);
  s_active_tab = (TabFilter)tab;
  for(int i = 0; i < 3; i++){
    lv_obj_t* b = s_tab_btns[i];
    if(!b) continue;
    bool sel = (i == tab);
    lv_obj_set_style_bg_color(b, sel ? lv_color_hex(0xE5352B) : lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_color(b, sel ? lv_color_hex(0xE5352B) : lv_color_hex(0xE7E0D2), 0);
    lv_obj_t* lbl = lv_obj_get_child(b, 0);
    if(lbl) lv_obj_set_style_text_color(lbl, sel ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x23303A), 0);
  }
  populate_grid();
  lv_refr_now(lv_disp_get_default());
}

static lv_obj_t* s_top_dev_lbl = nullptr;
static lv_obj_t* s_top_dev_dot = nullptr;

// Add Kiosk badges (ABS / YOTO source badge, ★ highlight badge, #sequence badge, and optional title fallback)
// Optimized to reuse existing child objects and avoid lv_obj_clean churn / memory fragmentation
static void decorate_tile(lv_obj_t* tile, int card_idx, bool show_seq){
  if(!tile || card_idx < 0 || card_idx >= s_card_count) return;
  const Card& c = s_cards[card_idx];
  bool has_cover = (c.pool_slot >= 0 && s_thumb_pool[c.pool_slot].buf != nullptr);

  lv_obj_t* title_lbl = nullptr;
  lv_obj_t* star_lbl  = nullptr;
  lv_obj_t* seq_lbl   = nullptr;
  uint32_t cnt = lv_obj_get_child_cnt(tile);
  for(uint32_t idx = 0; idx < cnt; idx++){
    lv_obj_t* ch = lv_obj_get_child(tile, idx);
    void* ud = lv_obj_get_user_data(ch);
    if(ud == (void*)1001) title_lbl = ch;
    else if(ud == (void*)1002) star_lbl = ch;
    else if(ud == (void*)1003) seq_lbl = ch;
  }

  // Fallback title label
  if(!has_cover){
    if(!title_lbl){
      title_lbl = lv_label_create(tile);
      lv_obj_set_user_data(title_lbl, (void*)1001);
      lv_obj_set_width(title_lbl, THUMB_W - 20);
      lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_WRAP);
      lv_obj_set_style_text_align(title_lbl, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_16, 0);
      lv_obj_set_style_text_color(title_lbl, lv_color_hex(0xFFFFFF), 0);
      lv_obj_clear_flag(title_lbl, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_center(title_lbl);
    }
    lv_label_set_text(title_lbl, c.title.c_str());
    lv_obj_clear_flag(title_lbl, LV_OBJ_FLAG_HIDDEN);
  } else if(title_lbl){
    lv_obj_add_flag(title_lbl, LV_OBJ_FLAG_HIDDEN);
  }

  // 1. Highlight Star Badge (top-left) - ONLY show in main Catalog tab, NOT in Series or Recently Played
  bool show_star = c.is_highlight && (s_active_tab == TAB_ALL);
  if(show_star){
    if(!star_lbl){
      star_lbl = lv_label_create(tile);
      lv_obj_set_user_data(star_lbl, (void*)1002);
      lv_label_set_text(star_lbl, " * ");
      lv_obj_set_style_text_font(star_lbl, &lv_font_montserrat_16, 0);
      lv_obj_set_style_text_color(star_lbl, lv_color_hex(0xFFFFFF), 0);
      lv_obj_set_style_bg_color(star_lbl, lv_color_hex(0xF59E0B), 0);
      lv_obj_set_style_bg_opa(star_lbl, LV_OPA_90, 0);
      lv_obj_set_style_radius(star_lbl, 6, 0);
      lv_obj_set_style_pad_left(star_lbl, 4, 0);
      lv_obj_set_style_pad_right(star_lbl, 4, 0);
      lv_obj_set_style_pad_top(star_lbl, 2, 0);
      lv_obj_set_style_pad_bottom(star_lbl, 0, 0);
      lv_obj_clear_flag(star_lbl, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_align(star_lbl, LV_ALIGN_TOP_LEFT, 6, 6);
    }
    lv_obj_clear_flag(star_lbl, LV_OBJ_FLAG_HIDDEN);
  } else if(star_lbl){
    lv_obj_add_flag(star_lbl, LV_OBJ_FLAG_HIDDEN);
  }

  // 2. Sequence Badge in Series view (top-right)
  bool has_seq = (c.seq_display.length() > 0 || c.sequence >= 0.0f);
  if(show_seq && has_seq){
    String s_txt;
    if(c.seq_display.length() > 0){
      s_txt = c.seq_display.startsWith("#") ? String(c.seq_display.c_str()) : (String("#") + c.seq_display.c_str());
    } else {
      int iv = (int)c.sequence;
      if(fabsf(c.sequence - (float)iv) < 0.01f) s_txt = String("#") + String(iv);
      else s_txt = String("#") + String(c.sequence, 1);
    }
    if(!seq_lbl){
      seq_lbl = lv_label_create(tile);
      lv_obj_set_user_data(seq_lbl, (void*)1003);
      lv_obj_set_style_text_font(seq_lbl, &lv_font_montserrat_14, 0);
      lv_obj_set_style_text_color(seq_lbl, lv_color_hex(0xFFFFFF), 0);
      lv_obj_set_style_bg_color(seq_lbl, lv_color_hex(0x1E293B), 0);
      lv_obj_set_style_bg_opa(seq_lbl, LV_OPA_90, 0);
      lv_obj_set_style_radius(seq_lbl, 6, 0);
      lv_obj_set_style_pad_left(seq_lbl, 6, 0);
      lv_obj_set_style_pad_right(seq_lbl, 6, 0);
      lv_obj_set_style_pad_top(seq_lbl, 2, 0);
      lv_obj_set_style_pad_bottom(seq_lbl, 2, 0);
      lv_obj_clear_flag(seq_lbl, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_align(seq_lbl, LV_ALIGN_TOP_RIGHT, -6, 6);
    }
    lv_label_set_text(seq_lbl, s_txt.c_str());
    lv_obj_clear_flag(seq_lbl, LV_OBJ_FLAG_HIDDEN);
  } else if(seq_lbl){
    lv_obj_add_flag(seq_lbl, LV_OBJ_FLAG_HIDDEN);
  }
}

static bool is_tile_visible(lv_obj_t* tile){
  if(!tile || !lv_obj_is_valid(tile)) return false;
  lv_area_t a;
  lv_obj_get_coords(tile, &a);
  // Ignore tiles that have not yet undergone initial layout
  if(lv_area_get_width(&a) <= 0 || lv_area_get_height(&a) <= 0) return false;
  return (a.y2 >= -40 && a.y1 <= (LCD_V_RES + 40));
}

static int acquire_thumb_slot(int card_idx){
  for(int p = 0; p < THUMB_POOL_SIZE; p++){
    if(s_thumb_pool[p].card_idx < 0){
      return p;
    }
  }

  int best_slot = -1;
  uint32_t oldest_time = 0xFFFFFFFF;
  for(int p = 0; p < THUMB_POOL_SIZE; p++){
    int c_idx = s_thumb_pool[p].card_idx;
    bool visible = false;
    if(c_idx >= 0 && c_idx < s_card_count && s_cards[c_idx].tile){
      visible = is_tile_visible(s_cards[c_idx].tile);
    }
    if(!visible){
      if(s_thumb_pool[p].last_use < oldest_time){
        oldest_time = s_thumb_pool[p].last_use;
        best_slot = p;
      }
    }
  }

  if(best_slot < 0){
    for(int p = 0; p < THUMB_POOL_SIZE; p++){
      if(s_thumb_pool[p].last_use < oldest_time){
        oldest_time = s_thumb_pool[p].last_use;
        best_slot = p;
      }
    }
  }

  if(best_slot >= 0){
    int old_idx = s_thumb_pool[best_slot].card_idx;
    if(old_idx >= 0 && old_idx < s_card_count){
      s_cards[old_idx].pool_slot = -1;
      if(s_cards[old_idx].tile && lv_obj_is_valid(s_cards[old_idx].tile)){
        lv_obj_set_style_bg_img_src(s_cards[old_idx].tile, NULL, 0);
        decorate_tile(s_cards[old_idx].tile, old_idx, s_active_tab == TAB_SERIES);
        lv_obj_invalidate(s_cards[old_idx].tile);
      }
    }
    s_thumb_pool[best_slot].card_idx = -1;
  }
  return best_slot;
}

bool load_thumb_for_card(int card_idx, bool force_refr){
  if(card_idx < 0 || card_idx >= s_card_count) return false;
  Card& c = s_cards[card_idx];

  if(c.pool_slot >= 0 && c.pool_slot < THUMB_POOL_SIZE){
    s_thumb_pool[c.pool_slot].last_use = millis();
    return true;
  }

  if(!s_sd_ok || !sd_cache_exists(c.id.c_str(), THUMB_W, c.cover.c_str())){
    return false;
  }

  int slot = acquire_thumb_slot(card_idx);
  if(slot < 0) return false;

  if(!sd_cache_read(c.id.c_str(), THUMB_W, s_thumb_pool[slot].buf, THUMB_SZ, c.cover.c_str())){
    s_thumb_pool[slot].card_idx = -1;
    return false;
  }

  s_thumb_pool[slot].card_idx = card_idx;
  s_thumb_pool[slot].last_use = millis();
  c.pool_slot = slot;

  if(c.tile && lv_obj_is_valid(c.tile)){
    lv_obj_set_style_bg_img_src(c.tile, &s_thumb_pool[slot].dsc, 0);
    if(s_active_tab != TAB_SERIES || s_selected_series_filter.length() > 0) decorate_tile(c.tile, card_idx, s_active_tab == TAB_SERIES);
    lv_obj_invalidate(c.tile);
  }

  return true;
}

void update_visible_thumbs(bool force_refr){
  if(!s_grid) return;
  for(int i = 0; i < s_card_count; i++){
    if(s_cards[i].tile && is_tile_visible(s_cards[i].tile)){
      if(s_cards[i].pool_slot < 0){
        if(load_thumb_for_card(i, force_refr)) yield();
      } else {
        s_thumb_pool[s_cards[i].pool_slot].last_use = millis();
      }
    }
  }
}

// User interaction detector: Returns true if user touched recently or grid is in motion
inline bool is_ui_interacting(){
  if(s_grid && lv_obj_is_scrolling(s_grid)) return true;
  if(lv_disp_get_inactive_time(NULL) < 1500) return true;
  return false;
}

// On-demand visible cover streaming:
// When scrolling lands on tiles that are not yet cached on MicroSD, stream their 160x254
// thumbnail covers over WiFi peacefully when idle so placeholders turn into covers without UI lag.
inline bool stream_visible_uncached_thumb(){
  if(is_ui_interacting()) return false;
  if(!s_grid || s_card_count <= 0) return false;
  lv_obj_t* act = lv_scr_act();
  if(act != s_scr_home) return false;

  for(int i = 0; i < s_card_count; i++){
    if(s_cards[i].tile && is_tile_visible(s_cards[i].tile)){
      if(s_cards[i].pool_slot >= 0) continue;

      // If it exists on SD, load it immediately into an LRU PSRAM slot
      if(sd_cache_exists(s_cards[i].id.c_str(), THUMB_W, s_cards[i].cover.c_str())){
        if(load_thumb_for_card(i, true)) return true;
      }

      // Check failure backoff (skip cards that failed within 20 seconds)
      if(s_cards[i].stream_fail_t > 0 && (millis() - s_cards[i].stream_fail_t < 20000)){
        continue;
      }

      // Visible tile is missing its cover on SD: fetch it over WiFi, cache to SD, and load!
      if(s_cards[i].cover.length() > 0){
        if(!s_dcover) s_dcover = (uint8_t*)heap_caps_malloc(DCOVER_SZ, MALLOC_CAP_SPIRAM);
        if(s_dcover && load_cover_cached(s_cards[i].id.c_str(), s_cards[i].cover.c_str(), THUMB_W, THUMB_H, THUMB_SZ, s_dcover)){
          load_thumb_for_card(i, true);
          log_msg("[STREAM] Cached & loaded cover for card %d: '%s'", i, s_cards[i].title.c_str());
          return true;
        } else {
          s_cards[i].stream_fail_t = millis();
        }
      }
    }
  }
  return false;
}

static lv_obj_t* create_card_tile(lv_obj_t* parent, int i, bool show_seq){
  Card& c = s_cards[i];
  lv_obj_t* tile = lv_obj_create(parent);
  lv_obj_set_size(tile, THUMB_W, THUMB_H);
  lv_obj_set_style_bg_color(tile, c.is_abs() ? lv_color_hex(0x312E81) : lv_color_hex(0xE5352B), 0);
  lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(tile, 14, 0);
  lv_obj_set_style_pad_all(tile, 0, 0);
  lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_translate_y(tile, 3, LV_STATE_PRESSED);
  lv_obj_set_style_opa(tile, LV_OPA_80, LV_STATE_PRESSED);
  lv_obj_set_user_data(tile, (void*)(intptr_t)i);
  lv_obj_add_event_cb(tile, tile_cb, LV_EVENT_CLICKED, NULL);
  c.tile = tile;
  if(c.pool_slot >= 0 && s_thumb_pool[c.pool_slot].buf){
    lv_obj_set_style_bg_img_src(tile, &s_thumb_pool[c.pool_slot].dsc, 0);
  } else {
    lv_obj_set_style_bg_img_src(tile, NULL, 0);
  }
  decorate_tile(tile, i, show_seq);
  return tile;
}

static std::vector<PsramString, PsramVectorAlloc<PsramString>> s_series_group_names;
static void series_tile_cb(lv_event_t* e){
  int g_idx = (int)(intptr_t)lv_event_get_user_data(e);
  if(g_idx < 0 || g_idx >= (int)s_series_group_names.size()){
    lv_obj_t* target = lv_event_get_current_target(e);
    if(target) g_idx = (int)(intptr_t)lv_obj_get_user_data(target);
  }
  if(g_idx >= 0 && g_idx < (int)s_series_group_names.size()){
    s_selected_series_filter = s_series_group_names[g_idx];
    Serial.printf("[SERIES] Selected series: '%s' (idx %d)\n", s_selected_series_filter.c_str(), g_idx);
    populate_grid();
    lv_refr_now(lv_disp_get_default());
  }
}
static void series_back_cb(lv_event_t*){
  s_selected_series_filter = "";
  populate_grid();
  lv_refr_now(lv_disp_get_default());
}

static lv_obj_t* create_series_tile(lv_obj_t* parent, int elig_g_idx, const PsramString& gname, int rep_card_idx, int count){
  lv_obj_t* tile = lv_obj_create(parent);
  lv_obj_set_size(tile, THUMB_W, THUMB_H);
  lv_obj_set_style_bg_color(tile, lv_color_hex(0x312E81), 0);
  lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(tile, 14, 0);
  lv_obj_set_style_pad_all(tile, 0, 0);
  lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_translate_y(tile, 3, LV_STATE_PRESSED);
  lv_obj_set_style_opa(tile, LV_OPA_80, LV_STATE_PRESSED);
  lv_obj_set_user_data(tile, (void*)(intptr_t)elig_g_idx);
  lv_obj_add_event_cb(tile, series_tile_cb, LV_EVENT_CLICKED, (void*)(intptr_t)elig_g_idx);

  if(rep_card_idx >= 0 && rep_card_idx < s_card_count){
    Card& rc = s_cards[rep_card_idx];
    rc.tile = tile;
    if(rc.pool_slot >= 0 && s_thumb_pool[rc.pool_slot].buf){
      lv_obj_set_style_bg_img_src(tile, &s_thumb_pool[rc.pool_slot].dsc, 0);
    }
  }

  // Top-right count pill: number-only "(7)"
  lv_obj_t* ql = lv_label_create(tile);
  lv_label_set_text_fmt(ql, "(%d)", count);
  lv_obj_set_style_text_font(ql, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(ql, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_bg_color(ql, lv_color_hex(0x1E293B), 0);
  lv_obj_set_style_bg_opa(ql, LV_OPA_90, 0);
  lv_obj_set_style_radius(ql, 6, 0);
  lv_obj_set_style_pad_left(ql, 6, 0);
  lv_obj_set_style_pad_right(ql, 6, 0);
  lv_obj_set_style_pad_top(ql, 2, 0);
  lv_obj_set_style_pad_bottom(ql, 2, 0);
  lv_obj_clear_flag(ql, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(ql, LV_ALIGN_TOP_RIGHT, -6, 6);

  // Bottom translucent banner: Series title
  lv_obj_t* ban = lv_obj_create(tile);
  lv_obj_set_size(ban, THUMB_W, 52);
  lv_obj_align(ban, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(ban, lv_color_hex(0x1E293B), 0);
  lv_obj_set_style_bg_opa(ban, LV_OPA_80, 0);
  lv_obj_set_style_radius(ban, 10, 0);
  lv_obj_set_style_pad_left(ban, 6, 0);
  lv_obj_set_style_pad_right(ban, 6, 0);
  lv_obj_set_style_pad_top(ban, 4, 0);
  lv_obj_set_style_pad_bottom(ban, 4, 0);
  lv_obj_set_style_border_width(ban, 0, 0);
  lv_obj_clear_flag(ban, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t* bl = lv_label_create(ban);
  lv_label_set_text(bl, gname.c_str());
  lv_obj_set_width(bl, THUMB_W - 16);
  lv_label_set_long_mode(bl, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(bl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(bl, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(bl, lv_color_hex(0xFFFFFF), 0);
  lv_obj_clear_flag(bl, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_center(bl);

  return tile;
}

static void populate_grid(){

  if(!s_grid) return;
  lv_obj_clean(s_grid);
  clear_card_pool_slots();

  if(s_active_tab == TAB_SERIES){
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    static std::vector<int, PsramVectorAlloc<int>> group_counts;
    static std::vector<int, PsramVectorAlloc<int>> elig_idx;
    static std::vector<int, PsramVectorAlloc<int>> rep_idx;
    static std::vector<int, PsramVectorAlloc<int>> m_idx;
    s_series_group_names.clear();
    group_counts.clear();
    elig_idx.clear();
    rep_idx.clear();
    m_idx.clear();

    auto parse_disp_seq = [](const PsramString& sd) -> float {
      for(size_t i = 0; i < sd.length(); i++){
        if(sd[i] >= '0' && sd[i] <= '9') return sd.substring(i).toFloat();
      }
      return -1.0f;
    };
    auto card_seq_in_group = [&](const Card& c, const PsramString& grp) -> float {
      PsramString cs = c.series; cs.trim();
      if(cs.length() == 0) cs = "Standalone & Singles";
      if(cs.equalsIgnoreCase(grp)){
        if(c.sequence >= 0.0f) return c.sequence;
        float p = parse_disp_seq(c.seq_display);
        return p >= 0.0f ? p : 999999.0f;
      }
      for(int ex = 0; ex < c.extra_series_cnt; ex++){
        if(c.extra_series[ex].equalsIgnoreCase(grp)){
          if(c.extra_seq[ex] >= 0.0f) return c.extra_seq[ex];
          float p = parse_disp_seq(c.extra_seq_disp[ex]);
          return p >= 0.0f ? p : 999999.0f;
        }
      }
      return 999999.0f;
    };

    auto add_to_group = [&](const PsramString& raw_name, int card_index){
      PsramString gname = raw_name;
      gname.trim();
      if(gname.length() == 0) gname = "Standalone & Singles";
      int found = -1;
      for(size_t g = 0; g < s_series_group_names.size(); g++){
        if(s_series_group_names[g].equalsIgnoreCase(gname)){ found = (int)g; break; }
      }
      if(found >= 0){
        group_counts[found]++;
        int cur_rep = rep_idx[found];
        float cur_seq = (cur_rep >= 0) ? card_seq_in_group(s_cards[cur_rep], gname) : 999999.0f;
        float new_seq = card_seq_in_group(s_cards[card_index], gname);
        if(new_seq < cur_seq) rep_idx[found] = card_index;
      } else {
        s_series_group_names.push_back(gname);
        group_counts.push_back(1);
        rep_idx.push_back(card_index);
      }
    };

    for(int i = 0; i < s_card_count; i++){
      add_to_group(s_cards[i].series, i);
      for(int ex = 0; ex < s_cards[i].extra_series_cnt; ex++){
        if(s_cards[i].extra_series[ex].length() > 0){
          add_to_group(s_cards[i].extra_series[ex], i);
        }
      }
    }

    // LEVEL 2: Selected Series Tracklist View
    if(s_selected_series_filter.length() > 0){
      const PsramString& sel_grp = s_selected_series_filter;
      for(int i = 0; i < s_card_count; i++){
        String cs = s_cards[i].series.c_str();
        cs.trim();
        if(cs.length() == 0) cs = "Standalone & Singles";
        bool match = cs.equalsIgnoreCase(sel_grp.c_str());
        if(!match){
          for(int ex = 0; ex < s_cards[i].extra_series_cnt; ex++){
            String es = s_cards[i].extra_series[ex].c_str();
            es.trim();
            if(es.equalsIgnoreCase(sel_grp.c_str())){ match = true; break; }
          }
        }
        if(match) m_idx.push_back(i);
      }
      int m_cnt = (int)m_idx.size();
      for(int a = 0; a < m_cnt; a++){
        for(int b = a + 1; b < m_cnt; b++){
          const Card& ca = s_cards[m_idx[a]];
          const Card& cb = s_cards[m_idx[b]];
          float sa = card_seq_in_group(ca, sel_grp);
          float sb = card_seq_in_group(cb, sel_grp);
          bool swap_it = false;
          if(sb != sa) swap_it = (sb < sa);
          else swap_it = (cb.sort_title < ca.sort_title);
          if(swap_it){
            int tmp = m_idx[a]; m_idx[a] = m_idx[b]; m_idx[b] = tmp;
          }
        }
      }

      // Level 2 Header Row with Back Button
      lv_obj_t* hdr = lv_obj_create(s_grid);
      lv_obj_set_size(hdr, LCD_H_RES - 36, 44);
      lv_obj_set_style_bg_color(hdr, lv_color_hex(0xF3ECE0), 0);
      lv_obj_set_style_radius(hdr, 12, 0);
      lv_obj_set_style_border_width(hdr, 1, 0);
      lv_obj_set_style_border_color(hdr, lv_color_hex(0xE7E0D2), 0);
      lv_obj_set_style_pad_left(hdr, 10, 0);
      lv_obj_set_style_pad_right(hdr, 14, 0);
      lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(hdr, 12, 0);
      lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

      lv_obj_t* bk_btn = lv_btn_create(hdr);
      lv_obj_set_size(bk_btn, 170, 34);
      lv_obj_set_style_radius(bk_btn, 17, 0);
      lv_obj_set_style_bg_color(bk_btn, lv_color_hex(0xFFFFFF), 0);
      lv_obj_set_style_border_width(bk_btn, 1, 0);
      lv_obj_set_style_border_color(bk_btn, lv_color_hex(0xE7E0D2), 0);
      lv_obj_set_style_shadow_width(bk_btn, 0, 0);
      lv_obj_add_event_cb(bk_btn, series_back_cb, LV_EVENT_CLICKED, NULL);

      lv_obj_t* bkl = lv_label_create(bk_btn);
      lv_label_set_text(bkl, LV_SYMBOL_LEFT " Back to Series");
      lv_obj_set_style_text_font(bkl, &lv_font_montserrat_14, 0);
      lv_obj_set_style_text_color(bkl, lv_color_hex(0x23303A), 0);
      lv_obj_center(bkl);

      lv_obj_t* htitle = lv_label_create(hdr);
      lv_label_set_text(htitle, sel_grp.c_str());
      lv_obj_set_style_text_font(htitle, &lv_font_montserrat_20, 0);
      lv_obj_set_style_text_color(htitle, lv_color_hex(0x23303A), 0);
      lv_label_set_long_mode(htitle, LV_LABEL_LONG_DOT);
      lv_obj_set_flex_grow(htitle, 1);

      lv_obj_t* hcnt = lv_label_create(hdr);
      lv_label_set_text_fmt(hcnt, "(%d)", m_cnt);
      lv_obj_set_style_text_font(hcnt, &lv_font_montserrat_16, 0);
      lv_obj_set_style_text_color(hcnt, lv_color_hex(0x8A8276), 0);

      for(int k = 0; k < m_cnt; k++){
        create_card_tile(s_grid, m_idx[k], true);
      }
      lv_obj_update_layout(s_grid);
      return;
    }

    // LEVEL 1: Series Collection Browser
    for(size_t g = 0; g < s_series_group_names.size(); g++){
      if(core::s_exclude_single_series && group_counts[g] <= 1) continue;
      elig_idx.push_back((int)g);
    }
    int elig_cnt = (int)elig_idx.size();

    for(int a = 0; a < elig_cnt; a++){
      for(int b = a + 1; b < elig_cnt; b++){
        const PsramString& na = s_series_group_names[elig_idx[a]];
        const PsramString& nb = s_series_group_names[elig_idx[b]];
        bool swap_it = false;
        if(na == "Standalone & Singles" && nb != "Standalone & Singles") swap_it = true;
        else if(nb != "Standalone & Singles" && clean_sort_title(nb.c_str()) < clean_sort_title(na.c_str())) swap_it = true;
        if(swap_it){
          int tmp = elig_idx[a]; elig_idx[a] = elig_idx[b]; elig_idx[b] = tmp;
        }
      }
    }

    if(elig_cnt == 0){
      lv_obj_t* empty = lv_label_create(s_grid);
      lv_label_set_text(empty, "No multi-book series found in this library.");
      lv_obj_set_width(empty, LCD_H_RES - 40);
      lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_style_text_font(empty, &lv_font_montserrat_20, 0);
      lv_obj_set_style_text_color(empty, lv_color_hex(0x8A8276), 0);
      lv_obj_set_style_pad_top(empty, 60, 0);
      return;
    }

    for(int e = 0; e < elig_cnt; e++){
      int g_idx = elig_idx[e];
      create_series_tile(s_grid, g_idx, s_series_group_names[g_idx], rep_idx[g_idx], group_counts[g_idx]);
    }
    lv_obj_update_layout(s_grid);
    return;
  }

  // Flat Grid View (TAB_ALL / Catalog, TAB_RECENT, TAB_ABS, TAB_YOTO)
  lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(s_grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

  static std::vector<int, PsramVectorAlloc<int>> grid_indices;
  grid_indices.clear();
  for(int i = 0; i < s_card_count; i++){
    if(card_matches_active_tab(s_cards[i])) grid_indices.push_back(i);
  }

  if(s_active_tab == TAB_RECENT){
    std::sort(grid_indices.begin(), grid_indices.end(), [](int ia, int ib){
      const Card& a = s_cards[ia];
      const Card& b = s_cards[ib];
      if(a.daily != b.daily) return a.daily;
      return a.lp > b.lp; // newest played first
    });
  }

  int shown = 0;
  for(size_t gi = 0; gi < grid_indices.size(); gi++){
    int i = grid_indices[gi];
    create_card_tile(s_grid, i, false);
    shown++;
    if((shown & 31) == 0) yield();
  }
  if(shown == 0){
    lv_obj_t* empty = lv_label_create(s_grid);
    if(s_active_tab == TAB_RECENT){
      lv_label_set_text(empty, "No audiobooks played in the past 7 days.");
    } else {
      lv_label_set_text(empty, "No audiobooks found in this category.");
    }
    lv_obj_set_width(empty, LCD_H_RES - 40);
    lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(empty, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(empty, lv_color_hex(0x8A8276), 0);
    lv_obj_set_style_pad_top(empty, 60, 0);
  }
}

void build_grid(){
  status_hide();
  s_top_dev_lbl = nullptr;
  s_top_dev_dot = nullptr;
  for(int t = 0; t < 4; t++) s_tab_btns[t] = nullptr;
  if(!s_scr_home){
    s_scr_home = lv_obj_create(NULL);
  } else {
    lv_obj_clean(s_scr_home);
  }
  lv_obj_t* scr=s_scr_home;
  lv_obj_set_style_bg_color(scr, lv_color_hex(0xFBF7F0), 0);

  // Top Tab Navigation Bar (Direct flat children of nav for 100% reliable LVGL touch hit-testing)
  // Left: [Profile Pill + Target Player Pill], Right: [All, Series, Gear]
  if((int)s_active_tab > 2) s_active_tab = TAB_ALL;

  lv_obj_t* nav = lv_obj_create(scr);
  lv_obj_set_size(nav, LCD_H_RES, 48);
  lv_obj_align(nav, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(nav, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_border_side(nav, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(nav, 1, 0);
  lv_obj_set_style_border_color(nav, lv_color_hex(0xE7E0D2), 0);
  lv_obj_set_style_radius(nav, 0, 0);
  lv_obj_set_style_pad_all(nav, 0, 0);
  lv_obj_clear_flag(nav, LV_OBJ_FLAG_SCROLLABLE);

  // Top Left: Dedicated Player Status Pill (showing active player & connection status dot)
  lv_obj_t* dev_btn = lv_obj_create(nav);
  lv_obj_set_size(dev_btn, 240, 38);
  lv_obj_set_pos(dev_btn, 12, 5);
  lv_obj_set_style_radius(dev_btn, 19, 0);
  lv_obj_set_style_bg_color(dev_btn, lv_color_hex(0xF4EDE0), 0);
  lv_obj_set_style_border_width(dev_btn, 1, 0);
  lv_obj_set_style_border_color(dev_btn, lv_color_hex(0xE7E0D2), 0);
  lv_obj_set_style_pad_all(dev_btn, 0, 0);
  lv_obj_set_style_shadow_width(dev_btn, 0, 0);
  lv_obj_clear_flag(dev_btn, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* dot = lv_obj_create(dev_btn);
  s_top_dev_dot = dot;
  lv_obj_set_size(dot, 12, 12);
  lv_obj_set_style_radius(dot, 6, 0);
  lv_obj_set_style_bg_color(dot, get_player_dot_color(cfg::device_id), 0);
  lv_obj_set_style_border_width(dot, 0, 0);
  lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(dot, LV_ALIGN_LEFT_MID, 12, 0);

  lv_obj_t* dname = lv_label_create(dev_btn);
  s_top_dev_lbl = dname;
  lv_label_set_text(dname, get_active_player_name().c_str());
  lv_obj_set_style_text_font(dname, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(dname, lv_color_hex(0x23303A), 0);
  lv_label_set_long_mode(dname, LV_LABEL_LONG_DOT);
  lv_obj_set_width(dname, 195);
  lv_obj_clear_flag(dname, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(dname, LV_ALIGN_LEFT_MID, 32, 0);

  // 3. Right: Category Pills (Catalog + Series + Recently Played) + Grown-Ups Settings Gear Button
  static const char* tab_names[3]  = {"Catalog", "Series", "Recently Played"};
  static const int   tab_widths[3] = {96, 82, 164};
  static const int   tab_xs[3]     = {386, 490, 580};
  for(int t = 0; t < 3; t++){
    lv_obj_t* tb = lv_btn_create(nav);
    s_tab_btns[t] = tb;
    lv_obj_set_size(tb, tab_widths[t], 38);
    lv_obj_set_pos(tb, tab_xs[t], 5);
    lv_obj_set_style_radius(tb, 19, 0);
    bool sel = (t == (int)s_active_tab);
    lv_obj_set_style_bg_color(tb, sel ? lv_color_hex(0xE5352B) : lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(tb, 1, 0);
    lv_obj_set_style_border_color(tb, sel ? lv_color_hex(0xE5352B) : lv_color_hex(0xE7E0D2), 0);
    lv_obj_set_style_pad_all(tb, 0, 0);
    lv_obj_set_style_shadow_width(tb, 0, 0);
    lv_obj_add_event_cb(tb, tab_btn_cb, LV_EVENT_CLICKED, (void*)(intptr_t)t);

    lv_obj_t* tl = lv_label_create(tb);
    lv_label_set_text(tl, tab_names[t]);
    lv_obj_set_style_text_font(tl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(tl, sel ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x23303A), 0);
    lv_obj_clear_flag(tl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(tl);
  }

  // 4. Far Right: Grown-Ups Settings Gear Button
  lv_obj_t* top_gear = lv_btn_create(nav);
  lv_obj_set_size(top_gear, 40, 38);
  lv_obj_set_pos(top_gear, 752, 5);
  lv_obj_set_style_radius(top_gear, 19, 0);
  lv_obj_set_style_bg_color(top_gear, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_bg_color(top_gear, lv_color_hex(0xE5352B), LV_STATE_PRESSED);
  lv_obj_set_style_border_width(top_gear, 1, 0);
  lv_obj_set_style_border_color(top_gear, lv_color_hex(0xE7E0D2), 0);
  lv_obj_set_style_shadow_width(top_gear, 0, 0);
  lv_obj_add_event_cb(top_gear, [](lv_event_t*){
      s_scr_prev = s_scr_home;
    open_pin_modal(false);
  }, LV_EVENT_CLICKED, NULL);

  lv_obj_t* tgi = lv_label_create(top_gear);
  lv_label_set_text(tgi, LV_SYMBOL_SETTINGS);
  lv_obj_set_style_text_font(tgi, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(tgi, lv_color_hex(0x7A7060), 0);
  lv_obj_clear_flag(tgi, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_center(tgi);

  // Scrollable grid container (offset 48px for the top nav tab bar)
  lv_obj_t* cont=lv_obj_create(scr);
  s_grid=cont;
  lv_obj_set_size(cont, LCD_H_RES, 362);
  lv_obj_align(cont, LV_ALIGN_TOP_MID, 0, 48);
  lv_obj_set_style_bg_color(cont, lv_color_hex(0xFBF7F0), 0);
  lv_obj_set_style_border_width(cont, 0, 0);
  lv_obj_set_style_pad_all(cont, 18, 0);
  lv_obj_set_style_pad_row(cont, 20, 0);
  lv_obj_set_style_pad_column(cont, 20, 0);
  lv_obj_set_scroll_dir(cont, LV_DIR_VER);
  lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLL_ELASTIC);
  lv_obj_add_event_cb(cont, [](lv_event_t* e){
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_SCROLL_END){
      update_visible_thumbs(true);
    }
  }, LV_EVENT_ALL, NULL);

  populate_grid();

  // bottom status bar (Now Playing bar mirroring kiosk PlayerScreen.renderNowbar)
  s_bar=lv_obj_create(scr);
  lv_obj_set_size(s_bar, LCD_H_RES, 70);
  lv_obj_align(s_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(s_bar, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_border_side(s_bar, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_border_width(s_bar, 1, 0);
  lv_obj_set_style_border_color(s_bar, lv_color_hex(0xE7E0D2), 0);
  lv_obj_set_style_radius(s_bar, 0, 0);
  lv_obj_set_style_pad_all(s_bar, 0, 0);
  lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_SCROLLABLE);
  update_home_bar();
}

static void minibar_open_cb(lv_event_t*){
  if(!s_playing && s_play_title.length() == 0 && s_play_card < 0) return;
  s_cur_card = s_play_card;
  if(s_play_card >= 0 && s_play_card < s_card_count && s_cards[s_play_card].daily){
    s_cur_card = s_play_card; s_detail_title = "Yoto Daily";
    s_ch_count = 1; s_cur_ch = 0;
    s_ch[0].key = "daily"; s_ch[0].title = s_play_chapter; s_ch[0].dur = s_play_dur;
    open_now();
    return;
  }
  if(s_play_card >= 0 && s_play_card < s_card_count){
    if(s_cur_card != s_play_card){
      s_cur_card = s_play_card;
      if(!sd_chap_read(s_cards[s_play_card].id.c_str())) fetch_card_detail(s_play_card);
    }
    s_detail_title = s_cards[s_play_card].title.c_str();
  } else {
    s_cur_card = -1;
    s_detail_title = s_play_title;
    s_ch_count = 0;
  }
  s_cur_ch = s_play_ch;
  open_now();
}
static void minibar_toggle_cb(lv_event_t*){ np_toggle(); }


// Reusable Nowbar builder (100% kiosk PlayerScreen.renderNowbar parity; shared by Catalog & Detail screens)
static void populate_nowbar_obj(lv_obj_t* bar, bool is_home_bar){
  if(!bar) return;
  lv_obj_clean(bar);

  bool has_active = (s_playing || s_play_title.length() > 0 || (s_play_card >= 0 && s_play_card < s_card_count));
  if(!has_active){
    // Idle state: musical note icon badge + "Nothing playing — tap a book to listen"
    lv_obj_t* mb = lv_obj_create(bar);
    lv_obj_set_size(mb, 38, 38);
    lv_obj_set_style_radius(mb, 10, 0);
    lv_obj_set_style_bg_color(mb, lv_color_hex(0xF3ECE0), 0);
    lv_obj_set_style_border_width(mb, 0, 0);
    lv_obj_clear_flag(mb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(mb, LV_ALIGN_LEFT_MID, 18, 0);
    lv_obj_t* mbi = lv_label_create(mb);
    lv_label_set_text(mbi, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(mbi, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(mbi, lv_color_hex(0x8A8276), 0);
    lv_obj_center(mbi);

    lv_obj_t* lbl = lv_label_create(bar);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x9A9384), 0);
    lv_label_set_text(lbl, is_home_bar ? "Nothing playing  -  tap a book to listen" : "Nothing playing  -  tap a chapter to listen");
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 68, 0);
    if(is_home_bar) s_barlabel = lbl;
    return;
  }
  if(is_home_bar) s_barlabel = nullptr;

  // Far right: circular red Play/Pause button
  lv_obj_t* pp=lv_btn_create(bar);
  lv_obj_set_size(pp, 54, 54);
  lv_obj_align(pp, LV_ALIGN_RIGHT_MID, -16, 0);
  lv_obj_set_style_radius(pp, 27, 0);
  lv_obj_set_style_bg_color(pp, lv_color_hex(0xE5352B), 0);
  lv_obj_set_style_shadow_width(pp, 0, 0);
  lv_obj_add_event_cb(pp, minibar_toggle_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* ppi=lv_label_create(pp);
  lv_label_set_text(ppi, s_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
  lv_obj_set_style_text_font(ppi, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(ppi, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(ppi);

  // Tappable title/chapter block -> opens full Now Playing screen (expanded to full width)
  lv_obj_t* tap=lv_obj_create(bar);
  lv_obj_set_size(tap, 680, 64);
  lv_obj_align(tap, LV_ALIGN_LEFT_MID, 18, 0);
  lv_obj_set_style_bg_opa(tap, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(tap, 0, 0);
  lv_obj_set_style_pad_all(tap, 0, 0);
  lv_obj_clear_flag(tap, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(tap, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(tap, minibar_open_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* tt=lv_label_create(tap);
  lv_label_set_long_mode(tt, LV_LABEL_LONG_DOT);
  lv_obj_set_width(tt, 670);
  lv_label_set_text_fmt(tt, LV_SYMBOL_AUDIO "  %s", s_play_title.c_str());
  lv_obj_set_style_text_font(tt, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(tt, lv_color_hex(0x23303A), 0);
  lv_obj_align(tt, LV_ALIGN_LEFT_MID, 0, -11);

  lv_obj_t* tc=lv_label_create(tap);
  lv_label_set_long_mode(tc, LV_LABEL_LONG_DOT);
  lv_obj_set_width(tc, 670);
  if(s_play_author.length() > 0 && s_play_chapter.length() > 0){
    lv_label_set_text_fmt(tc, "%s  \xE2\x80\xA2  %s", s_play_author.c_str(), s_play_chapter.c_str());
  } else {
    lv_label_set_text(tc, s_play_chapter.c_str());
  }
  lv_obj_set_style_text_font(tc, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(tc, lv_color_hex(0x9A9384), 0);
  lv_obj_align(tc, LV_ALIGN_LEFT_MID, 28, 14);
}

// Rebuild the home bottom bar (and Detail screen bottom bar if visible)
static void update_home_bar(){
  if(s_top_dev_dot) lv_obj_set_style_bg_color(s_top_dev_dot, get_player_dot_color(cfg::device_id), 0);
  if(s_bar) populate_nowbar_obj(s_bar, true);
  if(s_detail_bar && s_scr_detail) populate_nowbar_obj(s_detail_bar, false);
}
