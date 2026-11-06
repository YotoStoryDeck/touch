#pragma once
// Global Application State, Data Models (Card, Chapter), Screen Pointers, and Shared Helpers
#include <Arduino.h>
#include <ArduinoJson.h>
#include <lvgl.h>
#include <esp_system.h>
#include "esp_heap_caps.h"
#include "app_config.h"
#include "core_api.h"
#include "hal_display.h"
#include "app_log.h"

// Keep big JSON work OUT of scarce internal RAM (which the TLS handshake + LVGL draw buffer
// fight over) by routing it to the idle 8MB PSRAM. Used for the raw response body AND the
// parsed DOM.
struct PsramAllocator : ArduinoJson::Allocator {
  void* allocate(size_t n) override          { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
  void  deallocate(void* p) override          { free(p); }
  void* reallocate(void* p, size_t n) override{ return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};

// ---------------- app state ----------------
#define THUMB_W   160
#define THUMB_H   254                       // 160 * 1011/638 — 4-up grid, ~19mm tiles (bigger tap target)
#define THUMB_SZ  (THUMB_W*THUMB_H*2)      // RGB565 bytes per cover (81280); ~4.0MB PSRAM for 49 covers
enum TabFilter { TAB_ALL=0, TAB_SERIES=1, TAB_ABS=2, TAB_YOTO=3 };
static TabFilter s_active_tab = TAB_ALL;

#define MAX_CARDS 64

struct Card {
  String id; String title;
  String author="";                        // book author / creator
  String lp;                               // lastPlayedAt (direct-API path sorts on it)
  String cover;                            // cover imageL URL (direct-API: fetch+decode from CDN)
  String source="yoto";                    // "yoto" or "abs"
  String series="";                        // primary series / collection name
  float  sequence=-1.0f;                   // book sequence in series (-1.0f = none; >= 0.0f = valid e.g. 0.0 for #0-3)
  String seq_display="";                   // e.g. "#0-3", "#1", or "1-3"
  String extra_series[2];                  // up to 2 additional series/groups from series_list
  float  extra_seq[2] = {-1.0f, -1.0f};
  String extra_seq_disp[2];
  int    extra_series_cnt=0;
  bool   is_highlight=false;               // highlighted / pinned card
  bool   daily=false;                      // synthetic "Yoto Daily" tile — one-tap, plays today
  lv_obj_t* tile=nullptr;                  // the lv_img tile in the grid
  uint8_t*  thumb=nullptr;                 // PSRAM RGB565 buffer (persists for image lifetime)
  lv_img_dsc_t dsc{};                      // descriptor LVGL renders from
};

// Yoto Daily isn't a library card — it's the player's "Button Play" card (3nC80), whose
// "daily" chapter holds one track per day (trackKey = YYYYMMDD). We inject a tile for it and
// play TODAY's track directly, mirroring the player's physical button.
static const char* YOTO_DAILY_ID    = "3nC80";
static const char* YOTO_DAILY_COVER =
  "https://card-content.yotoplay.com/yoto/pub/jC1GVU5Iwhl0yo5Lpcsn_OtXvp-5q2H2KwGAqeZR9ds";
static Card s_cards[MAX_CARDS];
int s_card_count=0;
static lv_obj_t* s_status=nullptr;
static lv_obj_t* s_status_box=nullptr;
static lv_obj_t* s_barlabel=nullptr;        // idle-state hint / cover-load progress (left side)
static lv_obj_t* s_bar=nullptr;             // home bottom bar — holds the mini now-playing

// screens (home persists so the 49 covers stay loaded; detail is rebuilt per card)
lv_obj_t* s_scr_home=nullptr;
static lv_obj_t* s_scr_detail=nullptr;
static int s_cur_card=-1;                   // card being BROWSED (detail/now screens read this)

// card detail (chapters) — fetched on demand from /board/card/{id}
#define DCOVER_W  210
#define DCOVER_H  333                       // 210 * 1011/638, keeps cover aspect
#define DCOVER_SZ (DCOVER_W*DCOVER_H*2)
#define MAX_CH    40
struct Chapter { String key; String track_key; String title; int dur; };
static Chapter   s_ch[MAX_CH];
static int       s_ch_count=0;
static String    s_detail_title;
static uint8_t*  s_dcover=nullptr;          // reusable PSRAM buffer for the detail cover
static lv_img_dsc_t s_dcover_dsc{};
static lv_obj_t* s_detail_play_lbl = nullptr;
static lv_obj_t* s_detail_bar = nullptr;    // bottom Now Playing mini-bar on Detail screen

// now-playing screen + optimistic playback state
#define NCOVER_W  170
#define NCOVER_H  270                       // 170 * 1011/638
#define NCOVER_SZ (NCOVER_W*NCOVER_H*2)
static lv_obj_t* s_scr_now=nullptr;
static int  s_cur_ch=-1;
static bool s_playing=false;
static int  s_pos=0;                        // optimistic seconds into the current chapter
int s_vol=30;

// playback IDENTITY — the card/chapter actually playing on the device, tracked separately
// from s_cur_card (which follows browsing).
static int    s_play_card=-1;
static int    s_play_ch=-1;
static String s_play_title;
static String s_play_author;
static String s_play_chapter;
static uint8_t* s_ncover=nullptr;
static lv_img_dsc_t s_ncover_dsc{};
static lv_obj_t* s_np_barfill=nullptr;      // live now-playing widgets (null when off-screen)
static lv_obj_t* s_np_tcur=nullptr;
static lv_obj_t* s_np_trem=nullptr;
static lv_obj_t* s_np_playicon=nullptr;
static lv_obj_t* s_np_chapter=nullptr;
static lv_obj_t* s_np_sleep=nullptr;        // "Zz 23m" sleep-timer chip on the now screen
static int  s_play_dur=0;                   // duration (s) of the chapter actually playing

// bedtime sleep timer: wall-clock seconds until we auto-stop the player (0 = off)
int s_sleep_sec=0;

// grown-ups settings screen + night-clock screensaver + pairing / error screens
lv_obj_t* s_scr_settings=nullptr;
lv_obj_t* s_scr_error=nullptr;
lv_obj_t* s_scr_pin=nullptr;
lv_obj_t* s_scr_pairing=nullptr;
static lv_timer_t* s_pairing_timer=nullptr;
lv_obj_t* s_scr_prev=nullptr;               // tracks origin screen when opening PIN modal / settings
lv_obj_t* s_saver_prev_scr=nullptr;         // tracks screen to restore when waking from screensaver
static bool s_local_session_active=false;   // true if current audio was started/controlled on this unit
static lv_obj_t* s_err_hs_client_lbl=nullptr;
static bool      s_hotspot_wanted=false;
lv_obj_t* s_set_sleep_lbl=nullptr;          // live countdown inside settings (null when closed)
static bool s_lib_ok=false;                 // library fetched + grid built
static bool s_net_ready=false;              // NTP/token init done after WiFi came up

// ---- crash-loop guard ----
#define BOOT_CRASH_LIMIT 3
static RTC_NOINIT_ATTR uint32_t s_crash_magic;
static RTC_NOINIT_ATTR uint32_t s_crash_count;
static bool s_safe_mode = false;
static bool s_crash_cleared = false;
static void crash_guard_init(){
  esp_reset_reason_t r = esp_reset_reason();
  if(s_crash_magic != 0xC0FFEE42){ s_crash_magic = 0xC0FFEE42; s_crash_count = 0; }
  bool crashed = (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
                  r == ESP_RST_WDT || r == ESP_RST_BROWNOUT);
  if(crashed) s_crash_count++; else s_crash_count = 0;
#ifdef WOKWI_SIMULATION
  s_safe_mode = false;
#else
  s_safe_mode = (s_crash_count >= BOOT_CRASH_LIMIT);
#endif
  Serial.printf("[BOOT] reset reason=%d crashes=%u safe_mode=%d\n", (int)r, (unsigned)s_crash_count, (int)s_safe_mode);
}

static void status_hide(){
  if(s_status_box){
    lv_obj_del(s_status_box);
    s_status_box = nullptr;
    s_status = nullptr;
    lv_refr_now(lv_disp_get_default());
  }
}

static void status(const char* msg){
  lv_obj_t* cur_act = lv_scr_act();
  if(!s_status_box || lv_obj_get_parent(s_status_box) != cur_act){
    if(s_status_box){ lv_obj_del(s_status_box); s_status_box = nullptr; s_status = nullptr; }
    // White card modal overlay with subtle border & shadow so message is crisp and prominent
    s_status_box = lv_obj_create(cur_act);
    lv_obj_set_size(s_status_box, 480, 120);
    lv_obj_center(s_status_box);
    lv_obj_set_style_bg_color(s_status_box, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(s_status_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_status_box, 16, 0);
    lv_obj_set_style_border_width(s_status_box, 2, 0);
    lv_obj_set_style_border_color(s_status_box, lv_color_hex(0xE5352B), 0);
    lv_obj_set_style_shadow_width(s_status_box, 32, 0);
    lv_obj_set_style_shadow_color(s_status_box, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(s_status_box, LV_OPA_40, 0);
    lv_obj_set_style_pad_all(s_status_box, 18, 0);
    lv_obj_clear_flag(s_status_box, LV_OBJ_FLAG_SCROLLABLE);

    s_status = lv_label_create(s_status_box);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0x23303A), 0);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_status, 440);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_status);
  }
  lv_label_set_text(s_status, msg);
  lv_obj_move_foreground(s_status_box);
  lv_refr_now(lv_disp_get_default());
}

// Kiosk cleanTitle parity: strip leading 'The ', 'A ', 'An ' (case-insensitive) and lowercase
static String clean_sort_title(const String& raw){
  String s = raw;
  s.trim();
  String low = s;
  low.toLowerCase();
  if(low.startsWith("the "))      low = low.substring(4);
  else if(low.startsWith("an "))  low = low.substring(3);
  else if(low.startsWith("a "))   low = low.substring(2);
  low.trim();
  return low;
}

static void fmt_mmss(char* b, int s){ if(s<0)s=0; sprintf(b, "%d:%02d", s/60, s%60); }
static void fmt_total(char* b, int s){ int h=s/3600, m=(s%3600)/60; if(h) sprintf(b,"%dh %dm",h,m); else sprintf(b,"%dm",m); }


// Kiosk App.isBedtimeNow() parity: returns true if current local wall-clock time is within Bedtime -> Wake window
inline bool is_bedtime_now(){
  struct tm tmv;
  if(!getLocalTime(&tmv, 0)) return false;
  int cur_m   = tmv.tm_hour * 60 + tmv.tm_min;
  int night_m = cfg::night_h * 60 + cfg::night_m;
  int wake_m  = cfg::wake_h  * 60 + cfg::wake_m;
  if(night_m > wake_m){
    return (cur_m >= night_m || cur_m < wake_m);
  }
  return (cur_m >= night_m && cur_m < wake_m);
}

inline void fmt_sleep(char* b, size_t n){
  if(s_sleep_sec <= 0)       snprintf(b, n, "Off");
  else if(s_sleep_sec >= 60) snprintf(b, n, "%d min left", (s_sleep_sec + 59) / 60);
  else                       snprintf(b, n, "%d sec left", s_sleep_sec);
}


static lv_color_t get_player_dot_color(const String& dev_id){
  const core::PlayerInfo* pi = core::find_player_info(dev_id);
  if(pi){
    if(pi->online && !pi->is_home) return lv_color_hex(0xEA580C); // orange = away / traveling
    if(s_playing || pi->is_playing) return lv_color_hex(0xE5352B); // red = playing
    if(pi->online) return lv_color_hex(0x16A34A);                 // green = online
    return lv_color_hex(0x9CA3AF);                                // grey = offline
  }
  return s_playing ? lv_color_hex(0xE5352B) : lv_color_hex(0x16A34A);
}

// Resolve friendly display name of the target player (from active device ID, profile, or standalone list)
inline String get_active_player_name(){
  const core::PlayerInfo* pi = core::find_player_info(cfg::device_id);
  if(pi && pi->name.length() > 0) return pi->name;
  if(cfg::op_mode == "core" && core::s_profile_count > 0 && core::s_active_profile_idx < core::s_profile_count){
    const String& pn = core::s_profiles[core::s_active_profile_idx].default_player_name;
    if(pn.length() > 0) return pn;
  }
  if(cfg::device_id.length() > 0) return String("Player ") + cfg::device_id;
  return "Yoto Player";
}

inline void go_home(){
  s_np_barfill=s_np_tcur=s_np_trem=s_np_playicon=s_np_chapter=s_np_sleep=nullptr;
  s_set_sleep_lbl=nullptr;
  s_detail_play_lbl=nullptr;
  s_detail_bar=nullptr;
  if(s_scr_home) lv_scr_load(s_scr_home);
  else if(s_scr_error) lv_scr_load(s_scr_error);
  if(s_scr_detail){   lv_obj_del_async(s_scr_detail);   s_scr_detail=nullptr; }
  if(s_scr_now){      lv_obj_del_async(s_scr_now);      s_scr_now=nullptr; }
  if(s_scr_settings){ lv_obj_del_async(s_scr_settings); s_scr_settings=nullptr; }
  if(s_scr_pin){      lv_obj_del_async(s_scr_pin);      s_scr_pin=nullptr; }
  if(s_pairing_timer){ lv_timer_del(s_pairing_timer); s_pairing_timer=nullptr; }
  if(s_scr_pairing){  lv_obj_del_async(s_scr_pairing);  s_scr_pairing=nullptr; }
}

// Non-blocking cover loader state (runs 1 HTTP cover per loop() pass so UI touch is never blocked)
static bool s_thumbs_loading = false;
static int  s_thumb_cursor   = 0;
static int  s_thumb_ok       = 0;
static String s_thumb_first_err = "";

// Kiosk App.syncActiveDevicePlayback() parity:
// Binds the active target player's live playback state from Core to the Now Playing Mini-Bar and state vars
inline void sync_active_player_playback(){
  if(cfg::op_mode != "core") return;
  const core::PlayerInfo* pi = core::find_player_info(cfg::device_id);
  // Auto-Follow Active Household Player:
  // If configured player is offline or not playing, automatically reflect an active playing player in the child's profile
  if(!pi || (!pi->is_playing && !pi->has_card) || !pi->online){
    int prof_idx = core::s_active_profile_idx;
    if(prof_idx >= 0 && prof_idx < core::s_profile_count){
      const auto& pr = core::s_profiles[prof_idx];
      for(int k = 0; k < pr.player_count; k++){
        if(pr.players[k].is_playing){
          pi = &pr.players[k];
          break;
        }
      }
    }
  }
  if(!pi) return;
  if(pi->volume > 0){
    int vmax = pi->volume_max > 0 ? pi->volume_max : cfg::vol_max;
    s_vol = pi->volume < vmax ? pi->volume : vmax;
  }
  bool has_media = (pi->is_playing || pi->has_card || pi->item_id.length() > 0 || pi->now_playing.length() > 0);
  if(has_media){
    s_playing = pi->is_playing;
    int matched = -1;
    for(int i = 0; i < s_card_count; i++){
      if(pi->item_id.length() > 0 && s_cards[i].id == pi->item_id){ matched = i; break; }
    }
    if(matched < 0 && pi->now_playing.length() > 0){
      String np_low = pi->now_playing; np_low.toLowerCase();
      for(int i = 0; i < s_card_count; i++){
        String tl = s_cards[i].title; tl.toLowerCase();
        if(tl.length() > 0 && (tl.indexOf(np_low) >= 0 || np_low.indexOf(tl) >= 0)){ matched = i; break; }
      }
    }
    if(matched >= 0){
      s_play_card   = matched;
      s_play_title  = pi->now_playing.length() > 0 ? pi->now_playing : s_cards[matched].title;
      s_play_author = pi->author.length() > 0 ? pi->author : s_cards[matched].author;
    } else if(s_card_count > 0){
      // External card not in filtered list: still show on mini-bar using index 0 as anchor
      if(s_play_card < 0) s_play_card = 0;
      s_play_title  = pi->now_playing.length() > 0 ? pi->now_playing : String("Playing Track");
      s_play_author = pi->author;
    }
    if(pi->chapter_title.length() > 0) s_play_chapter = pi->chapter_title;
    else if(s_play_chapter.length() == 0) s_play_chapter = "Playing";
    if(pi->duration > 0) s_play_dur = pi->duration;
    if(pi->position >= 0) s_pos = pi->position;
  } else {
    s_playing      = false;
    s_play_card    = -1;
    s_play_title   = "";
    s_play_author  = "";
    s_play_chapter = "";
  }
}

// Shared cross-module forward declarations
static void open_detail(int i);
static void play_from(int chapterIdx);
static void open_now();
static void np_toggle();
void np_volume(int v);
static void update_home_bar();
static bool fetch_card_detail(int i);
static void play_yoto_daily(int idx);
void open_settings();
void open_pin_modal(bool for_settings_change=false);
static void show_error(const char* title, const char* message, const char* action_hint=nullptr);
void net_init_and_fetch();
void build_grid();
void load_thumbs();
bool fetch_library_core();
static void decorate_tile(lv_obj_t* tile, int card_idx, bool show_seq);
static void ensure_hotspot_active();
void start_hotspot_cb(lv_event_t* e);
