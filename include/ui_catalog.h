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


static bool card_matches_active_tab(const Card& c){
  if(s_active_tab == TAB_ALL) return true;
  if(s_active_tab == TAB_YOTO) return (c.source == "yoto" || c.daily);
  if(s_active_tab == TAB_ABS)  return (c.source == "abs");
  if(s_active_tab == TAB_SERIES) return (c.series.length() > 0);
  return true;
}


static lv_obj_t* s_grid = nullptr;
static lv_obj_t* s_tab_btns[4] = {nullptr, nullptr, nullptr, nullptr};
static void populate_grid(); // fwd
static void close_device_picker_popup(); // fwd

static void tab_btn_cb(lv_event_t* e){
  close_device_picker_popup();
  int tab = (int)(intptr_t)lv_event_get_user_data(e);
  if(tab < 0 || tab > 1) return;
  Serial.printf("[TAB] Switched catalog tab to %d (%s)\n", tab, tab == 1 ? "Series" : "All");
  s_active_tab = (TabFilter)tab;
  for(int i = 0; i < 2; i++){
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

static lv_obj_t* s_dev_popup = nullptr;
static lv_obj_t* s_top_dev_lbl = nullptr;
static lv_obj_t* s_top_dev_dot = nullptr;

static void close_device_picker_popup(){
  if(s_dev_popup){
    lv_obj_del(s_dev_popup);
    s_dev_popup = nullptr;
  }
}

static void toggle_device_picker_popup(){
  if(s_dev_popup){
    close_device_picker_popup();
    return;
  }
  if(cfg::lock_player || !s_scr_home) return;
  const core::PlayerInfo* plist = nullptr;
  int pcnt = 0;
  get_selectable_players(plist, pcnt);
  if(!plist || pcnt <= 1) return;

  // Create a full-screen transparent backdrop on s_scr_home so tapping outside dismisses the dropdown
  s_dev_popup = lv_obj_create(s_scr_home);
  lv_obj_set_size(s_dev_popup, LCD_H_RES, LCD_V_RES);
  lv_obj_set_pos(s_dev_popup, 0, 0);
  lv_obj_set_style_bg_opa(s_dev_popup, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(s_dev_popup, 0, 0);
  lv_obj_set_style_pad_all(s_dev_popup, 0, 0);
  lv_obj_clear_flag(s_dev_popup, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_dev_popup, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_dev_popup, [](lv_event_t*){ close_device_picker_popup(); }, LV_EVENT_CLICKED, NULL);

  // Dropdown card anchored right under the player pill
  int card_x = (cfg::op_mode == "core" && core::s_profile_count > 0) ? 156 : 8;
  int card_h = 44 + pcnt * 50;
  if(card_h > 320) card_h = 320;
  lv_obj_t* card = lv_obj_create(s_dev_popup);
  lv_obj_set_pos(card, card_x, 50);
  lv_obj_set_size(card, 280, card_h);
  lv_obj_set_style_bg_color(card, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_radius(card, 14, 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(0xE7E0D2), 0);
  lv_obj_set_style_shadow_width(card, 16, 0);
  lv_obj_set_style_shadow_opa(card, LV_OPA_30, 0);
  lv_obj_set_style_pad_all(card, 8, 0);
  lv_obj_set_style_pad_row(card, 4, 0);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(card, LV_DIR_VER);

  lv_obj_t* hdr = lv_label_create(card);
  lv_label_set_text(hdr, "SELECT TARGET PLAYER");
  lv_obj_set_style_text_font(hdr, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hdr, lv_color_hex(0x8A8276), 0);
  lv_obj_set_style_pad_left(hdr, 6, 0);
  lv_obj_set_style_pad_bottom(hdr, 4, 0);

  String def_id = (cfg::op_mode == "core" && core::s_profile_count > 0)
                  ? core::s_profiles[core::s_active_profile_idx].default_player_id : String("");

  for(int i = 0; i < pcnt; i++){
    const core::PlayerInfo& pi = plist[i];
    bool is_sel = (pi.id == cfg::device_id);
    lv_obj_t* row = lv_btn_create(card);
    lv_obj_set_size(row, lv_pct(100), 44);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_bg_color(row, is_sel ? lv_color_hex(0xF4EDE0) : lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0xE8DEC8), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(row, is_sel ? 1 : 0, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0xE5352B), 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_pad_left(row, 10, 0);
    lv_obj_set_style_pad_right(row, 10, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_add_event_cb(row, [](lv_event_t* e){
      int idx = (int)(intptr_t)lv_event_get_user_data(e);
      const core::PlayerInfo* pl = nullptr; int cnt = 0;
      get_selectable_players(pl, cnt);
      if(pl && idx >= 0 && idx < cnt){
        cfg::device_id = pl[idx].id;
        cfg::save();
        if(s_top_dev_lbl) lv_label_set_text(s_top_dev_lbl, get_active_player_name().c_str());
        if(s_top_dev_dot) lv_obj_set_style_bg_color(s_top_dev_dot, get_player_dot_color(cfg::device_id), 0);
        update_home_bar();
      }
      close_device_picker_popup();
    }, LV_EVENT_CLICKED, (void*)(intptr_t)i);

    lv_obj_t* d = lv_obj_create(row);
    lv_obj_set_size(d, 10, 10);
    lv_obj_set_style_radius(d, 5, 0);
    lv_obj_set_style_bg_color(d, get_player_dot_color(pi.id), 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* nm = lv_label_create(row);
    String label_txt = pi.name;
    if(pi.online && !pi.is_home) label_txt += " (AWAY)";
    else if(pi.id == def_id)     label_txt += " (Default)";
    lv_label_set_text(nm, label_txt.c_str());
    lv_obj_set_style_text_font(nm, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(nm, is_sel ? lv_color_hex(0xE5352B) : lv_color_hex(0x23303A), 0);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(nm, 1);
  }
}

// Add Kiosk badges (ABS / YOTO source badge, ★ highlight badge, #sequence badge, and optional title fallback)
static void decorate_tile(lv_obj_t* tile, int card_idx, bool show_seq){
  if(!tile || card_idx < 0 || card_idx >= s_card_count) return;
  lv_obj_clean(tile);
  const Card& c = s_cards[card_idx];

  // If cover is not yet decoded or failed, show centered book title on the colored tile
  if(!c.dsc.data){
    lv_obj_t* lbl = lv_label_create(tile);
    lv_label_set_text(lbl, c.title.c_str());
    lv_obj_set_width(lbl, THUMB_W - 20);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(lbl);
  }

  // 1. Highlight Star Badge (top-left, single styled lv_label pill)
  if(c.is_highlight){
    lv_obj_t* sl = lv_label_create(tile);
    lv_label_set_text(sl, " * ");
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(sl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0xF59E0B), 0);
    lv_obj_set_style_bg_opa(sl, LV_OPA_90, 0);
    lv_obj_set_style_radius(sl, 6, 0);
    lv_obj_set_style_pad_left(sl, 4, 0);
    lv_obj_set_style_pad_right(sl, 4, 0);
    lv_obj_set_style_pad_top(sl, 2, 0);
    lv_obj_set_style_pad_bottom(sl, 0, 0);
    lv_obj_clear_flag(sl, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(sl, LV_ALIGN_TOP_LEFT, 6, 6);
  }

  // 2. Sequence Badge in Series view (top-right, single styled lv_label pill e.g. #1, #2)
  if(show_seq && (c.seq_display.length() > 0 || c.sequence > 0)){
    String s_txt = c.seq_display.length() > 0
                   ? (c.seq_display.startsWith("#") ? c.seq_display : (String("#") + c.seq_display))
                   : (String("#") + String(c.sequence));
    lv_obj_t* ql = lv_label_create(tile);
    lv_label_set_text(ql, s_txt.c_str());
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
  }

  // 3. Source Badge (bottom-right: single styled lv_label pill, ABS indigo #4F46E5 vs YOTO red #E5352B)
  bool is_abs = (c.source == "abs");
  lv_obj_t* bl = lv_label_create(tile);
  lv_label_set_text(bl, is_abs ? "ABS" : "YOTO");
  lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(bl, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_bg_color(bl, is_abs ? lv_color_hex(0x4F46E5) : lv_color_hex(0xE5352B), 0);
  lv_obj_set_style_bg_opa(bl, LV_OPA_90, 0);
  lv_obj_set_style_radius(bl, 6, 0);
  lv_obj_set_style_pad_left(bl, 6, 0);
  lv_obj_set_style_pad_right(bl, 6, 0);
  lv_obj_set_style_pad_top(bl, 2, 0);
  lv_obj_set_style_pad_bottom(bl, 2, 0);
  lv_obj_clear_flag(bl, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(bl, LV_ALIGN_BOTTOM_RIGHT, -6, -6);
}

static lv_obj_t* create_card_tile(lv_obj_t* parent, int i, bool show_seq){
  Card& c = s_cards[i];
  lv_obj_t* tile = lv_img_create(parent);
  lv_obj_set_size(tile, THUMB_W, THUMB_H);
  lv_obj_set_style_bg_color(tile, (c.source == "abs") ? lv_color_hex(0x312E81) : lv_color_hex(0xE5352B), 0);
  lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(tile, 14, 0);
  lv_obj_set_style_clip_corner(tile, true, 0);
  lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_translate_y(tile, 3, LV_STATE_PRESSED);
  lv_obj_set_style_opa(tile, LV_OPA_80, LV_STATE_PRESSED);
  lv_obj_set_user_data(tile, (void*)(intptr_t)i);
  lv_obj_add_event_cb(tile, tile_cb, LV_EVENT_CLICKED, NULL);
  if(c.dsc.data){
    lv_img_set_src(tile, &c.dsc);
  }
  decorate_tile(tile, i, show_seq);
  c.tile = tile;
  return tile;
}

static void populate_grid(){
  if(!s_grid) return;
  lv_obj_clean(s_grid);
  for(int i = 0; i < s_card_count; i++) s_cards[i].tile = nullptr;

  if(s_active_tab == TAB_SERIES){
    // Kiosk CatalogScreen.renderSeriesView() parity:
    // Group books by series name, optionally exclude 1-card series, sort series headers alphabetically
    // ignoring 'The', 'A', 'An' (with 'Standalone & Singles' last), and sort books within each series by sequence ascending!
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    String group_names[64];
    int group_counts[64];
    int gcnt = 0;

    for(int i = 0; i < s_card_count; i++){
      String gname = s_cards[i].series;
      gname.trim();
      if(gname.length() == 0) gname = "Standalone & Singles";
      int found = -1;
      for(int g = 0; g < gcnt; g++){
        if(group_names[g].equalsIgnoreCase(gname)){ found = g; break; }
      }
      if(found >= 0){
        group_counts[found]++;
      } else if(gcnt < 64){
        group_names[gcnt] = gname;
        group_counts[gcnt] = 1;
        gcnt++;
      }
    }

    // Filter out single-card series when exclude_single_card_series is enabled (and filter out Standalone & Singles if excluding singles)
    int elig_idx[64];
    int elig_cnt = 0;
    for(int g = 0; g < gcnt; g++){
      if(core::s_exclude_single_series){
        if(group_names[g] == "Standalone & Singles") continue;
        if(group_counts[g] <= 1) continue;
      }
      elig_idx[elig_cnt++] = g;
    }

    // Sort eligible series groups alphabetically ignoring 'The', 'A', 'An' ('Standalone & Singles' last)
    for(int a = 0; a < elig_cnt; a++){
      for(int b = a + 1; b < elig_cnt; b++){
        const String& na = group_names[elig_idx[a]];
        const String& nb = group_names[elig_idx[b]];
        bool swap_it = false;
        if(na == "Standalone & Singles" && nb != "Standalone & Singles") swap_it = true;
        else if(nb != "Standalone & Singles" && clean_sort_title(nb) < clean_sort_title(na)) swap_it = true;
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
      const String& gname = group_names[elig_idx[e]];
      int m_idx[64];
      int m_cnt = 0;
      for(int i = 0; i < s_card_count; i++){
        String cs = s_cards[i].series;
        cs.trim();
        if(cs.length() == 0) cs = "Standalone & Singles";
        if(cs.equalsIgnoreCase(gname)){
          m_idx[m_cnt++] = i;
        }
      }
      // Sort books within this series by sequence ascending, then clean title
      for(int a = 0; a < m_cnt; a++){
        for(int b = a + 1; b < m_cnt; b++){
          const Card& ca = s_cards[m_idx[a]];
          const Card& cb = s_cards[m_idx[b]];
          int sa = ca.sequence > 0 ? ca.sequence : 999999;
          int sb = cb.sequence > 0 ? cb.sequence : 999999;
          bool swap_it = false;
          if(sb != sa) swap_it = (sb < sa);
          else swap_it = (clean_sort_title(cb.title) < clean_sort_title(ca.title));
          if(swap_it){
            int tmp = m_idx[a]; m_idx[a] = m_idx[b]; m_idx[b] = tmp;
          }
        }
      }

      // Series Section Header Row (full width)
      lv_obj_t* hdr = lv_obj_create(s_grid);
      lv_obj_set_size(hdr, LCD_H_RES - 40, 38);
      lv_obj_set_style_bg_color(hdr, lv_color_hex(0xF3ECE0), 0);
      lv_obj_set_style_radius(hdr, 10, 0);
      lv_obj_set_style_border_width(hdr, 1, 0);
      lv_obj_set_style_border_color(hdr, lv_color_hex(0xE7E0D2), 0);
      lv_obj_set_style_pad_left(hdr, 14, 0);
      lv_obj_set_style_pad_right(hdr, 14, 0);
      lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(hdr, 10, 0);
      lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

      lv_obj_t* htitle = lv_label_create(hdr);
      lv_label_set_text(htitle, gname.c_str());
      lv_obj_set_style_text_font(htitle, &lv_font_montserrat_20, 0);
      lv_obj_set_style_text_color(htitle, lv_color_hex(0x23303A), 0);

      lv_obj_t* hcnt = lv_label_create(hdr);
      lv_label_set_text_fmt(hcnt, "(%d %s)", m_cnt, m_cnt == 1 ? "card" : "cards");
      lv_obj_set_style_text_font(hcnt, &lv_font_montserrat_16, 0);
      lv_obj_set_style_text_color(hcnt, lv_color_hex(0x8A8276), 0);

      for(int k = 0; k < m_cnt; k++){
        create_card_tile(s_grid, m_idx[k], true);
      }
    }
    return;
  }

  // Flat Grid View (TAB_ALL, TAB_ABS, TAB_YOTO)
  lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(s_grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

  int shown = 0;
  for(int i = 0; i < s_card_count; i++){
    if(!card_matches_active_tab(s_cards[i])) continue;
    create_card_tile(s_grid, i, false);
    shown++;
  }
  if(shown == 0){
    lv_obj_t* empty = lv_label_create(s_grid);
    lv_label_set_text(empty, "No audiobooks found in this category.");
    lv_obj_set_width(empty, LCD_H_RES - 40);
    lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(empty, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(empty, lv_color_hex(0x8A8276), 0);
    lv_obj_set_style_pad_top(empty, 60, 0);
  }
}

void build_grid(){
  s_dev_popup = nullptr;
  s_top_dev_lbl = nullptr;
  s_top_dev_dot = nullptr;
  for(int t = 0; t < 4; t++) s_tab_btns[t] = nullptr;
  if(!s_scr_home){
    s_scr_home = lv_obj_create(NULL);
  } else {
    lv_obj_clean(s_scr_home);
  }
  s_status=nullptr;
  lv_obj_t* scr=s_scr_home;
  lv_obj_set_style_bg_color(scr, lv_color_hex(0xFBF7F0), 0);

  // Top Tab Navigation Bar (Direct flat children of nav for 100% reliable LVGL touch hit-testing)
  // Left: [Profile Pill + Target Player Pill], Right: [All, Series, Gear]
  if((int)s_active_tab > 1) s_active_tab = TAB_ALL;

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

  bool is_core_with_profs = (cfg::op_mode == "core" && core::s_profile_count > 0);
  bool can_switch_prof    = is_core_with_profs && core::s_allow_profile_switch && (core::s_profile_count > 1);

  // 1. Left: Profile Pill (ONLY in Core Mode; Standalone Mode has no profiles, just players)
  if(is_core_with_profs){
    lv_obj_t* prof_btn = lv_btn_create(nav);
    lv_obj_set_size(prof_btn, 140, 38);
    lv_obj_set_pos(prof_btn, 8, 5);
    lv_obj_set_style_radius(prof_btn, 19, 0);
    lv_obj_set_style_bg_color(prof_btn, lv_color_hex(0xF4EDE0), 0);
    lv_obj_set_style_bg_color(prof_btn, lv_color_hex(0xE8DEC8), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(prof_btn, 1, 0);
    lv_obj_set_style_border_color(prof_btn, lv_color_hex(0xE7E0D2), 0);
    lv_obj_set_style_pad_all(prof_btn, 0, 0);
    lv_obj_set_style_shadow_width(prof_btn, 0, 0);
    if(can_switch_prof){
      lv_obj_add_event_cb(prof_btn, [](lv_event_t*){
        close_device_picker_popup();
        s_scr_prev = s_scr_home;
        open_profile_picker();
      }, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t* idot = lv_obj_create(prof_btn);
    lv_obj_set_size(idot, 24, 24);
    lv_obj_set_style_radius(idot, 12, 0);
    lv_obj_set_style_bg_color(idot, lv_color_hex(0xE5352B), 0);
    lv_obj_set_style_border_width(idot, 0, 0);
    lv_obj_set_style_pad_all(idot, 0, 0);
    lv_obj_clear_flag(idot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(idot, LV_ALIGN_LEFT_MID, 7, 0);

    lv_obj_t* ilbl = lv_label_create(idot);
    const String& cur_pname = core::s_profiles[core::s_active_profile_idx].display_name;
    char ini[2] = {cur_pname.length() > 0 ? cur_pname[0] : 'P', '\0'};
    lv_label_set_text(ilbl, ini);
    lv_obj_set_style_text_font(ilbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ilbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(ilbl);

    lv_obj_t* plbl = lv_label_create(prof_btn);
    lv_label_set_text(plbl, cur_pname.c_str());
    lv_obj_set_style_text_font(plbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(plbl, lv_color_hex(0x23303A), 0);
    lv_label_set_long_mode(plbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(plbl, can_switch_prof ? 78 : 96);
    lv_obj_clear_flag(plbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(plbl, LV_ALIGN_LEFT_MID, 36, 0);

    if(can_switch_prof){
      lv_obj_t* darr = lv_label_create(prof_btn);
      lv_label_set_text(darr, LV_SYMBOL_DOWN);
      lv_obj_set_style_text_font(darr, &lv_font_montserrat_14, 0);
      lv_obj_set_style_text_color(darr, lv_color_hex(0x8A8276), 0);
      lv_obj_clear_flag(darr, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_align(darr, LV_ALIGN_RIGHT_MID, -10, 0);
    }
  }

  // 2. Left: Target Player Selector Pill
  const core::PlayerInfo* sel_players = nullptr;
  int sel_player_cnt = 0;
  get_selectable_players(sel_players, sel_player_cnt);
  bool can_pick_player = (!cfg::lock_player && sel_player_cnt > 1);
  int dev_pill_x = is_core_with_profs ? 156 : 8;
  int dev_pill_w = is_core_with_profs ? 185 : 260;

  lv_obj_t* dev_btn = lv_btn_create(nav);
  lv_obj_set_size(dev_btn, dev_pill_w, 38);
  lv_obj_set_pos(dev_btn, dev_pill_x, 5);
  lv_obj_set_style_radius(dev_btn, 19, 0);
  lv_obj_set_style_bg_color(dev_btn, lv_color_hex(0xF4EDE0), 0);
  lv_obj_set_style_bg_color(dev_btn, lv_color_hex(0xE8DEC8), LV_STATE_PRESSED);
  lv_obj_set_style_border_width(dev_btn, 1, 0);
  lv_obj_set_style_border_color(dev_btn, lv_color_hex(0xE7E0D2), 0);
  lv_obj_set_style_pad_all(dev_btn, 0, 0);
  lv_obj_set_style_shadow_width(dev_btn, 0, 0);
  if(can_pick_player){
    lv_obj_add_event_cb(dev_btn, [](lv_event_t*){
      toggle_device_picker_popup();
    }, LV_EVENT_CLICKED, NULL);
  }

  lv_obj_t* dot = lv_obj_create(dev_btn);
  s_top_dev_dot = dot;
  lv_obj_set_size(dot, 10, 10);
  lv_obj_set_style_radius(dot, 5, 0);
  lv_obj_set_style_bg_color(dot, get_player_dot_color(cfg::device_id), 0);
  lv_obj_set_style_border_width(dot, 0, 0);
  lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(dot, LV_ALIGN_LEFT_MID, 12, 0);

  lv_obj_t* dname = lv_label_create(dev_btn);
  s_top_dev_lbl = dname;
  lv_label_set_text(dname, get_active_player_name().c_str());
  lv_obj_set_style_text_font(dname, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(dname, lv_color_hex(0x23303A), 0);
  lv_label_set_long_mode(dname, LV_LABEL_LONG_DOT);
  lv_obj_set_width(dname, dev_pill_w - (can_pick_player ? 52 : 34));
  lv_obj_clear_flag(dname, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(dname, LV_ALIGN_LEFT_MID, 28, 0);

  if(can_pick_player){
    lv_obj_t* dchev = lv_label_create(dev_btn);
    lv_label_set_text(dchev, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_font(dchev, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(dchev, lv_color_hex(0x8A8276), 0);
    lv_obj_clear_flag(dchev, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(dchev, LV_ALIGN_RIGHT_MID, -10, 0);
  }

  // 3. Right: Category Pills (All + Series) + Grown-Ups Settings Gear Button
  static const char* tab_names[2]  = {"All", "Series"};
  static const int   tab_widths[2] = {96, 106};
  static const int   tab_xs[2]     = {534, 638};
  for(int t = 0; t < 2; t++){
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
    close_device_picker_popup();
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
  if(s_play_card < 0 || s_play_card >= s_card_count) return;
  if(s_cards[s_play_card].daily){
    s_cur_card = s_play_card; s_detail_title = "Yoto Daily";
    s_ch_count = 1; s_cur_ch = 0;
    s_ch[0].key = "daily"; s_ch[0].title = s_play_chapter; s_ch[0].dur = s_play_dur;
    open_now();
    return;
  }
  if(s_cur_card != s_play_card){
    s_cur_card = s_play_card;
    if(!sd_chap_read(s_cards[s_play_card].id)) fetch_card_detail(s_play_card);
  }
  s_cur_ch = s_play_ch;
  open_now();
}
static void minibar_toggle_cb(lv_event_t*){ np_toggle(); }


// Rebuild the home bottom bar (100% kiosk PlayerScreen.renderNowbar parity; NO duplicate gear button)
static void update_home_bar(){
  if(!s_bar) return;
  lv_obj_clean(s_bar);
  if(s_top_dev_dot) lv_obj_set_style_bg_color(s_top_dev_dot, get_player_dot_color(cfg::device_id), 0);

  // Left: live status dot + active player name
  lv_obj_t* dot=lv_obj_create(s_bar);
  lv_obj_set_size(dot, 12, 12);
  lv_obj_set_style_radius(dot, 6, 0);
  lv_obj_set_style_bg_color(dot, get_player_dot_color(cfg::device_id), 0);
  lv_obj_set_style_border_width(dot, 0, 0);
  lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(dot, LV_ALIGN_LEFT_MID, 18, 0);

  lv_obj_t* name=lv_label_create(s_bar);
  lv_label_set_text(name, get_active_player_name().c_str());
  lv_obj_set_style_text_font(name, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(name, lv_color_hex(0x23303A), 0);
  lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
  lv_obj_set_width(name, 165);
  lv_obj_align(name, LV_ALIGN_LEFT_MID, 38, 0);

  if(s_play_card < 0){
    // Idle state mirroring kiosk: musical note icon badge + "Nothing playing — tap a book to listen"
    lv_obj_t* mb = lv_obj_create(s_bar);
    lv_obj_set_size(mb, 38, 38);
    lv_obj_set_style_radius(mb, 10, 0);
    lv_obj_set_style_bg_color(mb, lv_color_hex(0xF3ECE0), 0);
    lv_obj_set_style_border_width(mb, 0, 0);
    lv_obj_clear_flag(mb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(mb, LV_ALIGN_LEFT_MID, 212, 0);
    lv_obj_t* mbi = lv_label_create(mb);
    lv_label_set_text(mbi, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(mbi, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(mbi, lv_color_hex(0x8A8276), 0);
    lv_obj_center(mbi);

    s_barlabel=lv_label_create(s_bar);
    lv_obj_set_style_text_font(s_barlabel, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_barlabel, lv_color_hex(0x9A9384), 0);
    lv_label_set_text(s_barlabel, "Nothing playing  -  tap a book to listen");
    lv_obj_align(s_barlabel, LV_ALIGN_LEFT_MID, 262, 0);
    return;
  }
  s_barlabel=nullptr;   // mini-player owns the center/right while something is playing

  // Far right: circular red Play/Pause button
  lv_obj_t* pp=lv_btn_create(s_bar);
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

  // Tappable title/chapter block -> opens full Now Playing screen
  lv_obj_t* tap=lv_obj_create(s_bar);
  lv_obj_set_size(tap, 500, 64);
  lv_obj_align(tap, LV_ALIGN_LEFT_MID, 212, 0);
  lv_obj_set_style_bg_opa(tap, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(tap, 0, 0);
  lv_obj_set_style_pad_all(tap, 0, 0);
  lv_obj_clear_flag(tap, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(tap, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(tap, minibar_open_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* tt=lv_label_create(tap);
  lv_label_set_long_mode(tt, LV_LABEL_LONG_DOT);
  lv_obj_set_width(tt, 490);
  lv_label_set_text_fmt(tt, LV_SYMBOL_AUDIO "  %s", s_play_title.c_str());
  lv_obj_set_style_text_font(tt, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(tt, lv_color_hex(0x23303A), 0);
  lv_obj_align(tt, LV_ALIGN_LEFT_MID, 0, -11);

  lv_obj_t* tc=lv_label_create(tap);
  lv_label_set_long_mode(tc, LV_LABEL_LONG_DOT);
  lv_obj_set_width(tc, 490);
  if(s_play_author.length() > 0 && s_play_chapter.length() > 0){
    lv_label_set_text_fmt(tc, "%s  \xE2\x80\xA2  %s", s_play_author.c_str(), s_play_chapter.c_str());
  } else {
    lv_label_set_text(tc, s_play_chapter.c_str());
  }
  lv_obj_set_style_text_font(tc, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(tc, lv_color_hex(0x9A9384), 0);
  lv_obj_align(tc, LV_ALIGN_LEFT_MID, 28, 14);
}
