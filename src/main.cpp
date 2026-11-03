// Standalone Yoto wall controller: the board talks TLS straight to api.yotoplay.com
// (token refresh in NVS — no Mac/proxy in the loop). Cover art comes from Yoto's image
// CDN pre-resized and is decoded (PNG/JPEG) to RGB565 on the MCU, cached to MicroSD.
// Runtime config (WiFi, Yoto credentials, parent knobs) lives in NVS: editable from the
// on-device "grown-ups" screen (hold the gear) and http://yoto-controller.local/settings;
// if WiFi can't connect the board raises a "Yoto-Setup" hotspot with the same portal.
// On-screen status labels make boot/network debuggable without serial.
// Waveshare ESP32-S3-Touch-LCD-4.3B · arduino-esp32 3.3.9 · LVGL 8.4 · ArduinoJson 7.

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoOTA.h>   // OTA path 1: push firmware from PlatformIO over WiFi
#include <WebServer.h>    // sync web server backing ElegantOTA (no async libs needed)
#include <ElegantOTA.h>   // OTA path 2: browser uploader at http://<board>/update
#include <ArduinoJson.h>
#include <lvgl.h>
#include <time.h>          // NTP clock for the screensaver / OK-to-wake
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "FS.h"            // SD thumbnail cache (FS/SD/SPI ship with arduino-esp32 core)
#include "SD.h"
#include "SPI.h"
#include "secrets.h"
#include "app_config.h"   // NVS-backed runtime settings (cfg::)
#include "yoto_api.h"     // direct-to-Yoto client
#include "core_api.h"     // StoryDeck Core client (broker parity)
#include "yoto_mqtt.h"    // on-board MQTT live-state feed (reflects what's REALLY playing)
#include "web_portal.h"   // /settings web page (WiFi + credentials without reflashing)
#include <pngle.h>        // PNG decoder for CDN cover art (book covers)
#include <JPEGDEC.h>      // JPEG decoder (podcast covers are square JPEGs, not PNG)
#include <mbedtls/platform.h>  // runtime allocator override (TLS buffers → PSRAM, see setup)

// mbedTLS allocator: route all but the tiniest blocks to PSRAM, where 8MB sits mostly idle.
// Internal RAM is critically scarce here (the concurrent MQTT TLS session + LVGL draw buffer +
// WiFi leave a low-water of only a few hundred bytes), so a 4KB threshold still piled the
// handshake's 1–4KB blocks into internal RAM and starved on-demand fetches into -5/-11 failures.
// Dropped to 512B: only the hottest sub-512B bignum temporaries stay internal for speed; the
// rest go to PSRAM. Each side falls back to the other, so an exhausted pool degrades into a
// slow handshake, never a failed one. Installed first thing in setup().
static void* tls_psram_calloc(size_t n, size_t size){
  size_t total = n * size;
  uint32_t pref = (total >= 512) ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL;
  void* p = heap_caps_calloc(n, size, pref);
  if(!p) p = heap_caps_calloc(n, size, pref == MALLOC_CAP_SPIRAM ? MALLOC_CAP_INTERNAL : MALLOC_CAP_SPIRAM);
  return p;
}

#include "hal_display.h"
#include "storage_sd.h"
#include "ui_screensaver.h"

// ---------------- app state ----------------
#define THUMB_W   160
#define THUMB_H   254                       // 160 * 1011/638 — 4-up grid, ~19mm tiles (bigger tap target)
#define THUMB_SZ  (THUMB_W*THUMB_H*2)      // RGB565 bytes per cover (81280); ~4.0MB PSRAM for 49 covers
enum TabFilter { TAB_ALL=0, TAB_SERIES=1, TAB_ABS=2, TAB_YOTO=3 };
static TabFilter s_active_tab = TAB_ALL;

struct Card {
  String id; String title;
  String author="";                        // book author / creator
  String lp;                               // lastPlayedAt (direct-API path sorts on it)
  String cover;                            // cover imageL URL (direct-API: fetch+decode from CDN)
  String source="yoto";                    // "yoto" or "abs"
  String series="";                        // optional series / collection name
  int    sequence=0;                       // book sequence in series
  bool   daily=false;                      // synthetic "Yoto Daily" tile — one-tap, plays today
  lv_obj_t* tile=nullptr;                  // the lv_img tile in the grid
  uint8_t*  thumb=nullptr;                 // PSRAM RGB565 buffer (persists for image lifetime)
  lv_img_dsc_t dsc{};                       // descriptor LVGL renders from
};

// Yoto Daily isn't a library card — it's the player's "Button Play" card (3nC80), whose
// "daily" chapter holds one track per day (trackKey = YYYYMMDD). We inject a tile for it and
// play TODAY's track directly, mirroring the player's physical button.
static const char* YOTO_DAILY_ID    = "3nC80";
static const char* YOTO_DAILY_COVER  =
  "https://card-content.yotoplay.com/yoto/pub/jC1GVU5Iwhl0yo5Lpcsn_OtXvp-5q2H2KwGAqeZR9ds";
static Card s_cards[64];
static int  s_card_count=0;
static lv_obj_t* s_status=nullptr;
static lv_obj_t* s_barlabel=nullptr;        // idle-state hint / cover-load progress (left side)
static lv_obj_t* s_bar=nullptr;             // home bottom bar — holds the mini now-playing

// screens (home persists so the 49 covers stay loaded; detail is rebuilt per card)
static lv_obj_t* s_scr_home=nullptr;
static lv_obj_t* s_scr_detail=nullptr;
static int s_cur_card=-1;                    // card being BROWSED (detail/now screens read this)

// card detail (chapters) — fetched on demand from /board/card/{id}
#define DCOVER_W  210
#define DCOVER_H  333                       // 210 * 1011/638, keeps cover aspect
#define DCOVER_SZ (DCOVER_W*DCOVER_H*2)
#define MAX_CH    40
struct Chapter { String key; String title; int dur; };
static Chapter   s_ch[MAX_CH];
static int       s_ch_count=0;
static String    s_detail_title;
static uint8_t*  s_dcover=nullptr;          // reusable PSRAM buffer for the detail cover
static lv_img_dsc_t s_dcover_dsc{};

// now-playing screen + optimistic playback state (no live device poll yet — needs the
// family:device-status:view scope; we track state locally like the app.html prototype)
#define NCOVER_W  170
#define NCOVER_H  270                       // 170 * 1011/638
#define NCOVER_SZ (NCOVER_W*NCOVER_H*2)
static lv_obj_t* s_scr_now=nullptr;
static int  s_cur_ch=-1;
static bool s_playing=false;
static int  s_pos=0;                        // optimistic seconds into the current chapter
int s_vol=30;

// playback IDENTITY — the card/chapter actually playing on the device, tracked separately
// from s_cur_card (which follows browsing). Drives the home mini-player + reopening now-playing
// after the user has browsed to other cards. -1 = nothing has been started this session.
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
static int  s_play_dur=0;                   // duration (s) of the chapter actually playing —
                                            // lets the optimistic clock tick on ANY screen

// bedtime sleep timer: wall-clock seconds until we auto-stop the player (0 = off)
int s_sleep_sec=0;

// grown-ups settings screen + night-clock screensaver
static lv_obj_t* s_scr_settings=nullptr;
static lv_obj_t* s_scr_error=nullptr;
static lv_obj_t* s_scr_pin=nullptr;
static lv_obj_t* s_scr_profile=nullptr;
static lv_obj_t* s_scr_prev=nullptr;      // tracks origin screen when opening PIN modal / settings
static lv_obj_t* s_err_hs_client_lbl=nullptr;
static bool      s_hotspot_wanted=false;
static void ensure_hotspot_active();
static lv_obj_t* s_set_sleep_lbl=nullptr;   // live countdown inside settings (null when closed)
static lv_timer_t* s_gear_timer=nullptr;    // hold-the-gear-to-open timer
static bool s_lib_ok=false;                 // library fetched + grid built
static bool s_net_ready=false;              // NTP/token init done after WiFi came up

static void status(const char* msg){
  if(!s_status){
    s_status=lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0x23303A), 0);
    lv_obj_align(s_status, LV_ALIGN_CENTER, 0, 0);
  }
  lv_label_set_text(s_status, msg);
  lv_refr_now(lv_disp_get_default());      // paint immediately so we see progress
}

static void open_detail(int i);          // fwd
static void play_from(int chapterIdx);   // fwd
static void open_now();                  // fwd
static void np_toggle();                 // fwd
static void np_volume(int v);            // fwd
static void update_home_bar();           // fwd
static bool fetch_card_detail(int i);    // fwd
static bool sd_chap_read(const String& id);  // fwd (SD chapter cache read)
static void play_yoto_daily(int idx);    // fwd
static void open_settings();             // fwd
static void open_pin_modal(bool for_settings_change=false); // fwd
static void open_profile_picker(); // fwd
static void show_error(const char* title, const char* message, const char* action_hint=nullptr); // fwd
static void net_init_and_fetch();         // fwd
static void saver_open();                // fwd
static void saver_close();               // fwd
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

static void apply_tab_filter(){
  for(int i = 0; i < s_card_count; i++){
    if(s_cards[i].tile){
      if(card_matches_active_tab(s_cards[i])){
        lv_obj_clear_flag(s_cards[i].tile, LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(s_cards[i].tile, LV_OBJ_FLAG_HIDDEN);
      }
    }
  }
}

static lv_obj_t* s_grid=nullptr;

static void tab_btn_cb(lv_event_t* e){
  int tab = (int)(intptr_t)lv_event_get_user_data(e);
  s_active_tab = (TabFilter)tab;
  lv_obj_t* bar = lv_obj_get_parent(lv_event_get_target(e));
  uint32_t cnt = lv_obj_get_child_cnt(bar);
  for(uint32_t i = 0; i < cnt; i++){
    lv_obj_t* b = lv_obj_get_child(bar, i);
    bool sel = (i == (uint32_t)tab);
    lv_obj_set_style_bg_color(b, sel ? lv_color_hex(0xE5352B) : lv_color_hex(0xFFFFFF), 0);
    lv_obj_t* lbl = lv_obj_get_child(b, 0);
    if(lbl) lv_obj_set_style_text_color(lbl, sel ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x23303A), 0);
  }
  apply_tab_filter();
}

static String get_active_player_name();

static void build_grid(){
  if(!s_scr_home){
    s_scr_home = lv_obj_create(NULL);
  } else {
    lv_obj_clean(s_scr_home);
  }
  s_status=nullptr;
  lv_obj_t* scr=s_scr_home;
  lv_obj_set_style_bg_color(scr, lv_color_hex(0xFBF7F0), 0);

  // Top Tab Navigation Bar (100% web-touch catalog.js parity: Profile Pill, Player Pill, 4 Tabs, Settings Gear)
  lv_obj_t* nav = lv_obj_create(scr);
  lv_obj_set_size(nav, LCD_H_RES, 48);
  lv_obj_align(nav, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(nav, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_border_width(nav, 0, 0);
  lv_obj_set_style_pad_all(nav, 4, 0);
  lv_obj_set_style_pad_column(nav, 6, 0);
  lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(nav, LV_OBJ_FLAG_SCROLLABLE);

  // Left Group: Profile Pill (Core mode) & Target Player Pill
  if(cfg::op_mode == "core" && core::s_profile_count > 0){
    lv_obj_t* prof_btn = lv_btn_create(nav);
    lv_obj_set_size(prof_btn, 130, 38);
    lv_obj_set_style_radius(prof_btn, 19, 0);
    lv_obj_set_style_bg_color(prof_btn, lv_color_hex(0xF4EDE0), 0);
    lv_obj_set_style_border_width(prof_btn, 1, 0);
    lv_obj_set_style_border_color(prof_btn, lv_color_hex(0xE7E0D2), 0);
    lv_obj_set_style_pad_all(prof_btn, 2, 0);
    lv_obj_set_style_shadow_width(prof_btn, 0, 0);
    lv_obj_add_event_cb(prof_btn, [](lv_event_t*){
      s_scr_prev = s_scr_home;
      open_profile_picker();
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t* pcont = lv_obj_create(prof_btn);
    lv_obj_set_size(pcont, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(pcont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pcont, 0, 0);
    lv_obj_set_style_pad_all(pcont, 0, 0);
    lv_obj_set_style_pad_column(pcont, 4, 0);
    lv_obj_set_flex_flow(pcont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pcont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(pcont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* idot = lv_obj_create(pcont);
    lv_obj_set_size(idot, 24, 24);
    lv_obj_set_style_radius(idot, 12, 0);
    lv_obj_set_style_bg_color(idot, lv_color_hex(0xE5352B), 0);
    lv_obj_set_style_border_width(idot, 0, 0);
    lv_obj_set_style_pad_all(idot, 0, 0);
    lv_obj_clear_flag(idot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* ilbl = lv_label_create(idot);
    const String& cur_pname = core::s_profiles[core::s_active_profile_idx].display_name;
    char ini[2] = {cur_pname.length() > 0 ? cur_pname[0] : 'P', '\0'};
    lv_label_set_text(ilbl, ini);
    lv_obj_set_style_text_font(ilbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ilbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(ilbl);

    lv_obj_t* plbl = lv_label_create(pcont);
    lv_label_set_text(plbl, cur_pname.c_str());
    lv_obj_set_style_text_font(plbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(plbl, lv_color_hex(0x23303A), 0);
    lv_label_set_long_mode(plbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(plbl, 60);

    lv_obj_t* darr = lv_label_create(pcont);
    lv_label_set_text(darr, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_font(darr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(darr, lv_color_hex(0x8A8276), 0);
  }

  // Target Player Pill
  lv_obj_t* dev_btn = lv_obj_create(nav);
  lv_obj_set_size(dev_btn, 134, 38);
  lv_obj_set_style_radius(dev_btn, 19, 0);
  lv_obj_set_style_bg_color(dev_btn, lv_color_hex(0xF4EDE0), 0);
  lv_obj_set_style_border_width(dev_btn, 1, 0);
  lv_obj_set_style_border_color(dev_btn, lv_color_hex(0xE7E0D2), 0);
  lv_obj_set_style_pad_all(dev_btn, 2, 0);
  lv_obj_set_style_pad_column(dev_btn, 4, 0);
  lv_obj_set_flex_flow(dev_btn, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(dev_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(dev_btn, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* dot = lv_obj_create(dev_btn);
  lv_obj_set_size(dot, 10, 10);
  lv_obj_set_style_radius(dot, 5, 0);
  lv_obj_set_style_bg_color(dot, lv_color_hex(0x3AB06A), 0);
  lv_obj_set_style_border_width(dot, 0, 0);
  lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* dname = lv_label_create(dev_btn);
  lv_label_set_text(dname, get_active_player_name().c_str());
  lv_obj_set_style_text_font(dname, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(dname, lv_color_hex(0x23303A), 0);
  lv_label_set_long_mode(dname, LV_LABEL_LONG_DOT);
  lv_obj_set_width(dname, 96);

  // 4 Category Tabs (All Books, Series, Audiobooks, Yoto Cards)
  static const char* tab_names[4] = {"All", "Series", "Audio", "Cards"};
  for(int t = 0; t < 4; t++){
    lv_obj_t* tb = lv_btn_create(nav);
    lv_obj_set_size(tb, 96, 38);
    lv_obj_set_style_radius(tb, 19, 0);
    bool sel = (t == (int)s_active_tab);
    lv_obj_set_style_bg_color(tb, sel ? lv_color_hex(0xE5352B) : lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(tb, 1, 0);
    lv_obj_set_style_border_color(tb, lv_color_hex(0xE7E0D2), 0);
    lv_obj_set_style_shadow_width(tb, 0, 0);
    lv_obj_add_event_cb(tb, tab_btn_cb, LV_EVENT_CLICKED, (void*)(intptr_t)t);

    lv_obj_t* tl = lv_label_create(tb);
    lv_label_set_text(tl, tab_names[t]);
    lv_obj_set_style_text_font(tl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(tl, sel ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x23303A), 0);
    lv_obj_center(tl);
  }

  // Right: Grown-Ups Settings Gear Button (Instant tap opens PIN modal)
  lv_obj_t* top_gear = lv_btn_create(nav);
  lv_obj_set_size(top_gear, 42, 38);
  lv_obj_set_style_radius(top_gear, 19, 0);
  lv_obj_set_style_bg_color(top_gear, lv_color_hex(0xFFFFFF), 0);
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
  lv_obj_center(tgi);

  // scrollable grid container (offset 52px for the top nav tab bar)
  lv_obj_t* cont=lv_obj_create(scr);
  s_grid=cont;
  lv_obj_set_size(cont, LCD_H_RES, 360);
  lv_obj_align(cont, LV_ALIGN_TOP_MID, 0, 50);
  lv_obj_set_style_bg_color(cont, lv_color_hex(0xFBF7F0), 0);
  lv_obj_set_style_border_width(cont, 0, 0);
  // 4 columns of 160px tiles: 4*160 + 3*20 gap = 700; centered in the 800-wide row with ~32px
  // side margins. Bigger tiles than the old 5-up 122px grid = an easier target for little fingers.
  lv_obj_set_style_pad_all(cont, 18, 0);
  lv_obj_set_style_pad_row(cont, 20, 0);
  lv_obj_set_style_pad_column(cont, 20, 0);
  lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW_WRAP);
  // main=CENTER (center tiles in each row), but track place = START so rows stack from the
  // TOP — centering the track block hides the top rows once content overflows (unscrollable).
  lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_scroll_dir(cont, LV_DIR_VER);

  for(int i=0;i<s_card_count;i++){
    // image tile (cover fills it once the thumb loads); red rounded placeholder until then
    lv_obj_t* tile=lv_img_create(cont);
    lv_obj_set_size(tile, THUMB_W, THUMB_H);
    lv_obj_set_style_bg_color(tile, lv_color_hex(0xE5352B), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tile, 14, 0);
    lv_obj_set_style_clip_corner(tile, true, 0);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    // tactile press feedback: the tile dips + dims while a finger is on it, so every
    // touch visibly reacts even before the (blocking) detail fetch starts
    lv_obj_set_style_translate_y(tile, 3, LV_STATE_PRESSED);
    lv_obj_set_style_opa(tile, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_user_data(tile, (void*)(intptr_t)i);
    lv_obj_add_event_cb(tile, tile_cb, LV_EVENT_CLICKED, NULL);
    s_cards[i].tile=tile;
  }

  // bottom status bar (mini now-playing) — populated by update_home_bar()
  s_bar=lv_obj_create(scr);
  lv_obj_set_size(s_bar, LCD_H_RES, 70);
  lv_obj_align(s_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(s_bar, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_border_width(s_bar, 0, 0);
  lv_obj_set_style_radius(s_bar, 0, 0);
  lv_obj_set_style_pad_all(s_bar, 0, 0);
  lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_SCROLLABLE);
  update_home_bar();
}

// Reopen the now-playing screen for the card that's actually PLAYING (which may differ from
// the card currently browsed). Re-fetch its chapters if we've since browsed elsewhere.
static void minibar_open_cb(lv_event_t*){
  if(s_play_card < 0 || s_play_card >= s_card_count) return;
  if(s_cards[s_play_card].daily){
    // Yoto Daily has no normal chapter list — rebuild the one-track state we play it with,
    // from the preserved playback identity (don't refetch, which would load the wrong chapters).
    s_cur_card = s_play_card; s_detail_title = "Yoto Daily";
    s_ch_count = 1; s_cur_ch = 0;
    s_ch[0].key = "daily"; s_ch[0].title = s_play_chapter; s_ch[0].dur = s_play_dur;
    open_now();
    return;
  }
  if(s_cur_card != s_play_card){
    s_cur_card = s_play_card;
    // repopulate s_ch + s_detail_title for the now screen — SD cache first, only hit the
    // network if this card's chapters somehow aren't cached yet (matches open_detail/warmer)
    if(!sd_chap_read(s_cards[s_play_card].id)) fetch_card_detail(s_play_card);
  }
  s_cur_ch = s_play_ch;
  open_now();
}
static void minibar_toggle_cb(lv_event_t*){ np_toggle(); }   // np_toggle() refreshes the bar

// settings gear: a deliberate 2.5s HOLD opens the grown-ups screen — a quick kid-tap does
// nothing. The pending timer is cancelled the moment the finger lifts or slides off.
static void gear_open_cb(lv_timer_t*){ s_gear_timer=nullptr; s_scr_prev=s_scr_home; open_pin_modal(false); }
static void gear_event_cb(lv_event_t* e){
  lv_event_code_t c = lv_event_get_code(e);
  if(c==LV_EVENT_PRESSED && !s_gear_timer){
    s_gear_timer = lv_timer_create(gear_open_cb, 2500, NULL);
    lv_timer_set_repeat_count(s_gear_timer, 1);     // one-shot; self-deletes after firing
  } else if((c==LV_EVENT_RELEASED || c==LV_EVENT_PRESS_LOST) && s_gear_timer){
    lv_timer_del(s_gear_timer); s_gear_timer=nullptr;
  }
}

// Rebuild the home bottom bar from current state: left = device status; right = either the
// idle hint, or a mini now-playing (title + chapter, play/pause, tap-to-open).

// Resolve friendly display name of the target player (from active profile or configured device ID)
static String get_active_player_name(){
  if(cfg::op_mode == "core" && core::s_profile_count > 0 && core::s_active_profile_idx < core::s_profile_count){
    const String& pn = core::s_profiles[core::s_active_profile_idx].default_player_name;
    if(pn.length() > 0) return pn;
  }
  if(cfg::device_id.length() > 0) return String("Player ") + cfg::device_id;
  return "Yoto Player";
}

static void update_home_bar(){
  if(!s_bar) return;
  // the gear is about to be deleted+rebuilt — if a finger was mid-hold on it, the deleted
  // button can't deliver RELEASED, so disarm the pending open here
  if(s_gear_timer){ lv_timer_del(s_gear_timer); s_gear_timer=nullptr; }
  lv_obj_clean(s_bar);

  // left: green "online" dot + device name
  lv_obj_t* dot=lv_obj_create(s_bar);
  lv_obj_set_size(dot, 12, 12);
  lv_obj_set_style_radius(dot, 6, 0);
  lv_obj_set_style_bg_color(dot, lv_color_hex(0x3AB06A), 0);
  lv_obj_set_style_border_width(dot, 0, 0);
  lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(dot, LV_ALIGN_LEFT_MID, 16, 0);

  lv_obj_t* name=lv_label_create(s_bar);
  lv_label_set_text(name, get_active_player_name().c_str());
  lv_obj_set_style_text_font(name, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(name, lv_color_hex(0x3A6B50), 0);
  lv_obj_align(name, LV_ALIGN_LEFT_MID, 36, 0);

  // far right: settings gear — subtle grey, HOLD 2.5s to open (gear_event_cb). Text color
  // set on the button (pressed state turns it red) and inherited by the icon label.
  lv_obj_t* gear=lv_btn_create(s_bar);
  lv_obj_set_size(gear, 52, 52);
  lv_obj_align(gear, LV_ALIGN_RIGHT_MID, -8, 0);
  lv_obj_set_style_bg_opa(gear, LV_OPA_TRANSP, 0);
  lv_obj_set_style_shadow_width(gear, 0, 0);
  lv_obj_set_style_text_color(gear, lv_color_hex(0xC9C2B4), 0);
  lv_obj_set_style_text_color(gear, lv_color_hex(0xE5352B), LV_STATE_PRESSED);
  lv_obj_add_event_cb(gear, gear_event_cb, LV_EVENT_ALL, NULL);
  lv_obj_t* gi=lv_label_create(gear);
  lv_label_set_text(gi, LV_SYMBOL_SETTINGS);
  lv_obj_set_style_text_font(gi, &lv_font_montserrat_28, 0);
  lv_obj_center(gi);

  if(s_play_card < 0){
    // idle — keep the hint label (load_thumbs also writes cover-load progress here)
    s_barlabel=lv_label_create(s_bar);
    lv_obj_set_style_text_font(s_barlabel, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_barlabel, lv_color_hex(0x9A9384), 0);
    lv_label_set_text(s_barlabel, "");   // idle: left dot + player name is enough
    lv_obj_align(s_barlabel, LV_ALIGN_LEFT_MID, 188, 0);
    return;
  }
  s_barlabel=nullptr;   // mini-player owns the right side while something is playing

  // play/pause button (right of the tap block, left of the gear) — quick toggle from the grid
  lv_obj_t* pp=lv_btn_create(s_bar);
  lv_obj_set_size(pp, 54, 54);
  lv_obj_align(pp, LV_ALIGN_RIGHT_MID, -68, 0);
  lv_obj_set_style_radius(pp, 27, 0);
  lv_obj_set_style_bg_color(pp, lv_color_hex(0xE5352B), 0);
  lv_obj_set_style_shadow_width(pp, 0, 0);
  lv_obj_add_event_cb(pp, minibar_toggle_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* ppi=lv_label_create(pp);
  lv_label_set_text(ppi, s_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
  lv_obj_set_style_text_font(ppi, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(ppi, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(ppi);

  // tappable title/chapter block → opens the full Now Playing screen
  lv_obj_t* tap=lv_obj_create(s_bar);
  lv_obj_set_size(tap, 440, 64);
  lv_obj_align(tap, LV_ALIGN_RIGHT_MID, -132, 0);
  lv_obj_set_style_bg_opa(tap, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(tap, 0, 0);
  lv_obj_set_style_pad_all(tap, 0, 0);
  lv_obj_clear_flag(tap, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(tap, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(tap, minibar_open_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* tt=lv_label_create(tap);
  lv_label_set_long_mode(tt, LV_LABEL_LONG_DOT);
  lv_obj_set_width(tt, 440);
  lv_label_set_text_fmt(tt, LV_SYMBOL_AUDIO "  %s", s_play_title.c_str());
  lv_obj_set_style_text_font(tt, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(tt, lv_color_hex(0x23303A), 0);
  lv_obj_align(tt, LV_ALIGN_LEFT_MID, 0, -11);

  lv_obj_t* tc=lv_label_create(tap);
  lv_label_set_long_mode(tc, LV_LABEL_LONG_DOT);
  lv_obj_set_width(tc, 440);
  if(s_play_author.length() > 0 && s_play_chapter.length() > 0){
    lv_label_set_text_fmt(tc, "%s  \xE2\x80\xA2  %s", s_play_author.c_str(), s_play_chapter.c_str());
  } else {
    lv_label_set_text(tc, s_play_chapter.c_str());
  }
  lv_obj_set_style_text_font(tc, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(tc, lv_color_hex(0x9A9384), 0);
  lv_obj_align(tc, LV_ALIGN_LEFT_MID, 28, 14);
}

// Keep big JSON work OUT of scarce internal RAM (which the TLS handshake + LVGL draw buffer
// fight over) by routing it to the idle 8MB PSRAM. Used for the raw response body AND the
// parsed DOM. This is what lets the draw buffer grow back toward smooth scroll.
struct PsramAllocator : ArduinoJson::Allocator {
  void* allocate(size_t n) override          { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
  void  deallocate(void* p) override          { free(p); }
  void* reallocate(void* p, size_t n) override{ return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};

// Read the full body of an OPEN HTTPClient response into a NUL-terminated PSRAM buffer
// (caller frees). Returns nullptr on alloc failure. Buffering the whole body is required
// because stream-parsing from the TLS socket trips ArduinoJson on partial arrival; doing it
// in PSRAM (vs getString()'s internal-RAM String) is the headroom win.
static char* read_body_psram(HTTPClient& http, size_t& outLen){
  outLen = 0;
  int len = http.getSize();
  WiFiClient* stream = http.getStreamPtr();
  size_t cap = (len > 0) ? (size_t)len + 1 : 16384;
  char* buf = (char*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
  if(!buf) return nullptr;
  size_t got = 0; uint32_t t0 = millis();
  while(len < 0 || got < (size_t)len){
    size_t avail = stream->available();
    if(avail){
      if(got + avail + 1 > cap){
        size_t ncap = cap * 2;
        char* n = (char*)heap_caps_realloc(buf, ncap, MALLOC_CAP_SPIRAM);
        if(!n) break;
        buf = n; cap = ncap;
      }
      int n = stream->readBytes(buf + got, min(avail, cap - 1 - got));
      if(n > 0){ got += n; t0 = millis(); }
    } else if(!http.connected()){ break; }
    else if(millis() - t0 > 15000){ break; }
    else delay(1);
  }
  buf[got] = '\0';
  outLen = got;
  return buf;
}


// Add single card from Core catalog
static void add_core_card(const String& id, const String& title, const String& author, const String& cover, const String& src, const String& series, int seq){
  if(s_card_count >= 64) return;
  s_cards[s_card_count].id       = id;
  s_cards[s_card_count].title    = title;
  s_cards[s_card_count].author   = author;
  s_cards[s_card_count].cover    = cover;
  s_cards[s_card_count].source   = src;
  s_cards[s_card_count].series   = series;
  s_cards[s_card_count].sequence = seq;
  s_cards[s_card_count].daily    = false;
  s_card_count++;
}

static bool fetch_library_core(){
  s_card_count = 0;
  String prof_id = "default";
  if(core::s_profile_count > 0 && core::s_active_profile_idx < core::s_profile_count){
    prof_id = core::s_profiles[core::s_active_profile_idx].id;
  }
  bool ok = core::fetch_catalog(cfg::core_url, prof_id, add_core_card);
  if(!ok || s_card_count == 0){
    // If child catalog endpoint returned empty or failed, retry with default
    core::fetch_catalog(cfg::core_url, "default", add_core_card);
  }
  return (s_card_count > 0);
}

static bool fetch_library(){
  // GET /card/family/library straight from Yoto. The payload is large, so we parse it
  // through a filter (only cardId/title/lastPlayedAt/cover survive) and sort
  // recently-played-first on the board.
  // Buffer the FULL body in PSRAM (not an internal-RAM String), THEN parse — streaming
  // straight from the secure socket trips ArduinoJson into IncompleteInput when the stream
  // briefly has no byte ready. The filter + PSRAM-backed DOM keep internal RAM untouched.
  // The arena guard closes BEFORE the parse: anything the parse allocates in internal RAM
  // (the Card Strings) must not land inside the arena hole or the reclaim degrades.
  size_t blen = 0;
  char* body = nullptr;
  {
    tlsarena::Guard tg;                     // connect → stream → end, nothing else
    WiFiClientSecure cli; HTTPClient http;
    int code = yoto::get(cli, http, "/card/family/library");
    if(code != 200){
      if(code == 403 || code == 401){
        show_error("Authentication Required (HTTP 403)",
                   "Yoto rejected the request. Your Client ID, Secret, or Refresh Token\nis invalid or not yet configured.",
                   "Open Settings below or visit the web portal to enter valid credentials.");
      } else {
        show_error("Library Request Failed",
                   (String("Yoto API returned HTTP ") + code + " (free heap=" + ESP.getFreeHeap() + ")").c_str(),
                   "Check internet connection or retry.");
      }
      if(code > 0) http.end();
      return false;
    }
    body = read_body_psram(http, blen);
    http.end();
  }
  if(!body){ status("Library buf alloc fail"); return false; }
  JsonDocument filter;
  filter["cards"][0]["cardId"]                       = true;
  filter["cards"][0]["lastPlayedAt"]                 = true;
  filter["cards"][0]["card"]["title"]                = true;
  filter["cards"][0]["card"]["content"]["cover"]["imageL"] = true;
  PsramAllocator psram;
  JsonDocument doc(&psram);
  // const char* => ArduinoJson DUPLICATES strings into the (PSRAM) DOM, so freeing body is safe.
  DeserializationError err = deserializeJson(doc, (const char*)body, blen, DeserializationOption::Filter(filter));
  free(body);
  if(err){ status((String("JSON error: ") + err.c_str() + " (" + blen + "B)").c_str()); return false; }
  s_card_count = 0;
  for(JsonObject c : doc["cards"].as<JsonArray>()){
    if(s_card_count >= 64) break;
    s_cards[s_card_count].id    = (const char*)(c["cardId"] | "");
    s_cards[s_card_count].title = (const char*)(c["card"]["title"] | "");
    s_cards[s_card_count].lp    = (const char*)(c["lastPlayedAt"] | "");
    s_cards[s_card_count].cover = (const char*)(c["card"]["content"]["cover"]["imageL"] | "");
    s_card_count++;
  }
  // recently-played first — ISO-8601 timestamps sort lexically; "" (never played) sinks last
  for(int a=0;a<s_card_count;a++)
    for(int b=a+1;b<s_card_count;b++)
      if(s_cards[b].lp > s_cards[a].lp){ Card t=s_cards[a]; s_cards[a]=s_cards[b]; s_cards[b]=t; }
  // inject the "Yoto Daily" tile at the front (it's not in the library — see YOTO_DAILY_ID)
  if(s_card_count < 64){
    for(int k=s_card_count; k>0; k--) s_cards[k] = s_cards[k-1];
    s_cards[0] = Card{};
    s_cards[0].id = YOTO_DAILY_ID; s_cards[0].title = "Yoto Daily";
    s_cards[0].cover = YOTO_DAILY_COVER; s_cards[0].daily = true;
    s_card_count++;
  }
  return s_card_count > 0;
}

#include "cover_decoder.h"

// Stream one cover thumbnail into a PSRAM buffer and blit it onto the grid tile.
static bool load_thumb(int i){
  Card& c = s_cards[i];
  if(!c.thumb){
    c.thumb = (uint8_t*)heap_caps_malloc(THUMB_SZ, MALLOC_CAP_SPIRAM);
    if(!c.thumb) return false;
  }
  if(!load_cover_cached(c.id, c.cover, THUMB_W, THUMB_H, THUMB_SZ, c.thumb)) return false;
  set_img(c.dsc, c.tile, c.thumb, THUMB_W, THUMB_H, THUMB_SZ);
  return true;
}

static void load_thumbs(){
  int ok=0; String firstErr;
  for(int i=0;i<s_card_count;i++){
    if(load_thumb(i)) ok++;
    else if(firstErr.length()==0){ firstErr = s_thumb_diag; }
    if(s_barlabel)
      lv_label_set_text_fmt(s_barlabel, "loading covers  %d/%d", i+1, s_card_count);
    lv_refr_now(lv_disp_get_default());     // show each cover as it arrives
    vTaskDelay(pdMS_TO_TICKS(10));           // yield so WiFi and IDLE/WDT tasks execute cleanly
  }
  // one retry pass for stragglers (transient CDN/WiFi hiccups) before reporting failure;
  // a failed tile is the one whose descriptor never got pixels
  for(int i=0;i<s_card_count && ok<s_card_count;i++){
    if(!s_cards[i].dsc.data && load_thumb(i)){ ok++; lv_refr_now(lv_disp_get_default()); }
  }
  if(s_barlabel){
    if(ok < s_card_count)
      lv_label_set_text_fmt(s_barlabel, "covers %d/%d  -  %s", ok, s_card_count, firstErr.c_str());
    else
      lv_label_set_text(s_barlabel, "");   // covers done: clear the loading progress, leave bar clean
  }
}

// ---------------- detail screen ----------------
static void fmt_mmss(char* b, int s){ if(s<0)s=0; sprintf(b, "%d:%02d", s/60, s%60); }
static void fmt_total(char* b, int s){ int h=s/3600, m=(s%3600)/60; if(h) sprintf(b,"%dh %dm",h,m); else sprintf(b,"%dm",m); }

// Persist the freshly-parsed chapter list (in the s_ch globals) for this card to SD.
static void sd_chap_write(const String& id){
  if(!s_sd_ok) return;
  File f = SD.open(sd_chap_path(id), FILE_WRITE);
  if(!f) return;
  f.write((const uint8_t*)&CHAP_MAGIC, 2);
  sd_wr_str(f, s_detail_title);
  uint16_t n = (uint16_t)s_ch_count;
  f.write((const uint8_t*)&n, 2);
  for(int k=0;k<s_ch_count;k++){
    int32_t d = s_ch[k].dur;
    f.write((const uint8_t*)&d, 4);
    sd_wr_str(f, s_ch[k].key);
    sd_wr_str(f, s_ch[k].title);
  }
  f.close();
}

// Load this card's chapter list from SD into the s_ch globals. Returns false on any miss/short
// read so the caller falls back to a live fetch exactly as before — SD is a cache, not a dep.
static bool sd_chap_read(const String& id){
  if(!s_sd_ok) return false;
  File f = SD.open(sd_chap_path(id), FILE_READ);
  if(!f) return false;
  bool ok = false;
  do {
    uint16_t magic;
    if(f.read((uint8_t*)&magic, 2) != 2 || magic != CHAP_MAGIC) break;
    if(!sd_rd_str(f, s_detail_title)) break;
    uint16_t n;
    if(f.read((uint8_t*)&n, 2) != 2) break;
    if(n > MAX_CH) n = MAX_CH;
    int k = 0;
    for(; k < n; k++){
      int32_t d;
      if(f.read((uint8_t*)&d, 4) != 4) break;
      s_ch[k].dur = d;
      if(!sd_rd_str(f, s_ch[k].key))   break;
      if(!sd_rd_str(f, s_ch[k].title)) break;
    }
    if(k == n){ s_ch_count = n; ok = true; }
  } while(0);
  f.close();
  return ok;
}

static bool fetch_card_detail(int i){
  Card& c = s_cards[i];
  if(cfg::op_mode == "core" && cfg::core_url.length() > 0){
    s_ch_count = 0;
    String title_out = c.title;
    bool ok = core::fetch_media_tracks(cfg::core_url, c.id, title_out, [](const String& key, const String& tr_title, int dur){
      if(s_ch_count >= MAX_CH) return;
      s_ch[s_ch_count].key   = key;
      s_ch[s_ch_count].title = tr_title;
      s_ch[s_ch_count].dur   = dur;
      s_ch_count++;
    });
    if(ok && s_ch_count > 0){
      s_detail_title = title_out;
      sd_chap_write(c.id);
      return true;
    }
  }
  // GET /card/{id}; filter to title + chapters[].{key,title,duration}.
  size_t blen = 0;
  char* body = nullptr;
  {
    tlsarena::Guard tg;                     // closed before the parse — see fetch_library
    WiFiClientSecure cli; HTTPClient http;
    int code = yoto::get(cli, http, "/card/" + c.id);
    if(code != 200){ if(code > 0) http.end(); return false; }
    body = read_body_psram(http, blen);
    http.end();
  }
  if(!body) return false;
  JsonDocument filter;
  filter["card"]["title"] = true;
  filter["card"]["content"]["chapters"][0]["key"]      = true;
  filter["card"]["content"]["chapters"][0]["title"]    = true;
  filter["card"]["content"]["chapters"][0]["duration"] = true;
  PsramAllocator psram;
  JsonDocument doc(&psram);
  DeserializationError err = deserializeJson(doc, (const char*)body, blen, DeserializationOption::Filter(filter));
  free(body);
  if(err) return false;
  JsonObject card = doc["card"];
  s_detail_title = (const char*)(card["title"] | c.title.c_str());
  s_ch_count = 0;
  for(JsonObject ch : card["content"]["chapters"].as<JsonArray>()){
    if(s_ch_count >= MAX_CH) break;
    s_ch[s_ch_count].key   = (const char*)(ch["key"]   | "");
    s_ch[s_ch_count].title = (const char*)(ch["title"] | "");
    s_ch[s_ch_count].dur   = (int)(ch["duration"] | 0);
    s_ch_count++;
  }
  sd_chap_write(c.id);                          // write-through so the next open is instant
  return true;
}

// ---------------- background library warmer ----------------
// Kicks in once at boot (and only re-does work when a NEW book appears — its SD files are simply
// absent), walking the catalog during idle to fetch whatever isn't cached yet: the slim chapter
// list and the detail-size cover. Result: every card's detail screen opens instantly and offline.
// One network fetch per call; the loop() caller gates it to idle home/screensaver time so the
// ~1s blocking fetch never interrupts a tap. A fully-warm library costs only cheap SD.exists()
// checks, so the scan completes in microseconds and latches s_pc_done.
static int  s_pc_cursor = 0;     // next card to inspect
static int  s_pc_fail   = 0;     // consecutive failures on the current item (bounded, then skip)
static bool s_pc_done   = false; // whole library warm — stop scanning until the next reboot

// Internal RAM is the board's tightest budget (a cover fetch = a TLS handshake + a PNG/JPEG
// decode, both hungry for contiguous internal RAM, all while the MQTT TLS session is live). The
// warmer fires back-to-back fetches, so during a cold mass-warm the low-water can dip to a few
// hundred bytes. Gate each step on a healthy largest-free-block: if internal RAM is already tight
// (a recent fetch/decode hasn't released yet, or MQTT is mid-burst), defer to a later idle tick so
// we never stack a fresh handshake onto an already-thin heap.
#define PC_MIN_INTERNAL_BLOCK 12288

static void precache_step(){
  if(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < PC_MIN_INTERNAL_BLOCK) return;
  while(s_pc_cursor < s_card_count){
    Card& c = s_cards[s_pc_cursor];
    if(c.daily){ s_pc_cursor++; s_pc_fail = 0; continue; }   // Daily has no static detail/chapters

    // 1) chapters → {id}.chap
    if(!SD.exists(sd_chap_path(c.id))){
      // fetch_card_detail overwrites the shared s_ch globals that tick_cb reads for playback
      // auto-advance. Single-threaded, so just save them and restore before returning — the next
      // tick (next loop iteration) then sees the original playing-card chapter list untouched.
      static Chapter sv[MAX_CH];
      int svc = s_ch_count; String svt = s_detail_title;
      for(int k=0;k<svc;k++) sv[k] = s_ch[k];
      bool ok = fetch_card_detail(s_pc_cursor);              // writes {id}.chap on success
      for(int k=0;k<svc;k++){ s_ch[k] = sv[k]; sv[k].key = String(); sv[k].title = String(); }
      s_ch_count = svc; s_detail_title = svt;
      if(!ok && ++s_pc_fail >= 3){ s_pc_cursor++; s_pc_fail = 0; }   // give up on this card till reboot
      return;                                                // one fetch per call — yield to the UI
    }
    // 2) detail-size cover → {id}_184.565
    if(!SD.exists(sd_cache_path(c.id, DCOVER_W))){
      if(!s_dcover) s_dcover = (uint8_t*)heap_caps_malloc(DCOVER_SZ, MALLOC_CAP_SPIRAM);
      bool ok = s_dcover && load_cover_cached(c.id, c.cover, DCOVER_W, DCOVER_H, DCOVER_SZ, s_dcover);
      if(!ok && ++s_pc_fail >= 3){ s_pc_cursor++; s_pc_fail = 0; }
      return;
    }
    // 3) now-playing cover → {id}_150.565 (reuse s_dcover as scratch — DCOVER_SZ > NCOVER_SZ,
    //    and no detail screen is live while the warmer runs on home/saver, so it's free)
    if(!SD.exists(sd_cache_path(c.id, NCOVER_W))){
      if(!s_dcover) s_dcover = (uint8_t*)heap_caps_malloc(DCOVER_SZ, MALLOC_CAP_SPIRAM);
      bool ok = s_dcover && load_cover_cached(c.id, c.cover, NCOVER_W, NCOVER_H, NCOVER_SZ, s_dcover);
      if(!ok && ++s_pc_fail >= 3){ s_pc_cursor++; s_pc_fail = 0; }
      return;
    }
    s_pc_cursor++; s_pc_fail = 0;                            // this card fully warm — next
  }
  s_pc_done = true;
}

static void back_cb(lv_event_t* e){
  if(s_scr_home) lv_scr_load(s_scr_home);
  else if(s_scr_error) lv_scr_load(s_scr_error);
  // async: we're deleting the screen that owns the button firing this event
  if(s_scr_detail){ lv_obj_del_async(s_scr_detail); s_scr_detail=nullptr; }
}

// (Re)build the tappable chapter rows from the current s_ch globals into `list`, clearing any
// existing rows first — so the same list can be refilled after a live fetch replaces the cached
// (or empty "loading") placeholder rendered when the screen first opened.
static void detail_fill_chapters(lv_obj_t* list){
  lv_obj_clean(list);
  for(int k=0;k<s_ch_count;k++){
    lv_obj_t* row = lv_obj_create(list);
    lv_obj_set_size(row, lv_pct(100), 62);
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
    lv_obj_add_event_cb(row, [](lv_event_t* e){
      play_from((int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e)));
    }, LV_EVENT_CLICKED, NULL);

    bool is_ch_active = (s_playing && s_play_card == s_cur_card && s_play_ch == k);
    if(is_ch_active){
      lv_obj_set_style_bg_color(row, lv_color_hex(0xE0EDE7), 0);
      lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    }
    lv_obj_t* badge = lv_obj_create(row);
    lv_obj_set_size(badge, 38, 38);
    lv_obj_set_style_radius(badge, 19, 0);
    lv_obj_set_style_bg_color(badge, is_ch_active ? lv_color_hex(0x3AB06A) : lv_color_hex(0xFFB02E), 0);
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* bl = lv_label_create(badge);
    if(is_ch_active) lv_label_set_text(bl, LV_SYMBOL_PLAY);
    else             lv_label_set_text_fmt(bl, "%d", k+1);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(bl, is_ch_active ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x6A4400), 0);
    lv_obj_center(bl);

    lv_obj_t* ct = lv_label_create(row);
    lv_label_set_long_mode(ct, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(ct, 1);
    lv_label_set_text(ct, s_ch[k].title.c_str());
    lv_obj_set_style_text_font(ct, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ct, lv_color_hex(0x23303A), 0);

    char db[16]; fmt_mmss(db, s_ch[k].dur);
    lv_obj_t* cd = lv_label_create(row);
    lv_label_set_text(cd, db);
    lv_obj_set_style_text_font(cd, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(cd, lv_color_hex(0x9A9384), 0);
  }
}

static void detail_set_summary(lv_obj_t* sum){
  if(s_ch_count <= 0){ lv_label_set_text(sum, "loading chapters..."); return; }
  int total=0; for(int k=0;k<s_ch_count;k++) total += s_ch[k].dur;
  char tb[32]; fmt_total(tb, total);
  lv_label_set_text_fmt(sum, "%d chapters   .   %s", s_ch_count, tb);
}

static void open_detail(int i){
  s_cur_card = i;
  Card& c = s_cards[i];

  // 1) Chapters: SD cache first — instant and offline. On a miss, open with an empty
  //    "loading" list and fetch live AFTER the screen is painted (slow path below).
  bool have_ch = sd_chap_read(c.id);
  if(!have_ch){ s_ch_count = 0; s_detail_title = c.title; }

  // 2) Detail cover: SD cache first. On a miss, fall back to the grid thumb (already decoded
  //    in PSRAM) upscaled in place as an instant placeholder; the sharp cover loads below.
  bool cover_sd = false;
  if(!s_dcover) s_dcover = (uint8_t*)heap_caps_malloc(DCOVER_SZ, MALLOC_CAP_SPIRAM);
  if(s_dcover) cover_sd = sd_cache_read(c.id, DCOVER_W, s_dcover, DCOVER_SZ);

  lv_obj_t* scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0xFBF7F0), 0);

  // ---- left white panel: cover + big Play ----
  lv_obj_t* left = lv_obj_create(scr);
  lv_obj_set_size(left, 286, 480);
  lv_obj_set_pos(left, 0, 0);
  lv_obj_set_style_bg_color(left, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_border_width(left, 0, 0);
  lv_obj_set_style_radius(left, 0, 0);
  lv_obj_set_style_pad_all(left, 20, 0);
  lv_obj_set_style_pad_row(left, 18, 0);
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
  } else if(c.thumb && c.dsc.data){
    // instant placeholder: reuse the grid thumb's (read-only) descriptor, upscaled to fill the
    // larger detail frame, until the full-res cover arrives in the slow path below
    lv_img_set_src(cover, &c.dsc);
    lv_img_set_pivot(cover, THUMB_W/2, THUMB_H/2);
    lv_img_set_zoom(cover, (uint16_t)(256 * DCOVER_W / THUMB_W));
  }

  bool is_curr_playing = (s_playing && s_play_card == i);
  lv_obj_t* play = lv_btn_create(left);
  lv_obj_set_size(play, DCOVER_W, 66);
  lv_obj_set_style_bg_color(play, is_curr_playing ? lv_color_hex(0x23303A) : lv_color_hex(0xE5352B), 0);
  lv_obj_set_style_radius(play, 18, 0);
  lv_obj_t* pl = lv_label_create(play);
  lv_label_set_text(pl, is_curr_playing ? (LV_SYMBOL_PAUSE "  Now Playing") : (LV_SYMBOL_PLAY "  Play Book"));
  lv_obj_set_style_text_font(pl, &lv_font_montserrat_24, 0);
  lv_obj_center(pl);
  lv_obj_add_event_cb(play, [](lv_event_t*){ play_from(0); }, LV_EVENT_CLICKED, NULL);

  // ---- right panel: title + chapter list ----
  lv_obj_t* right = lv_obj_create(scr);
  lv_obj_set_size(right, 514, 480);
  lv_obj_set_pos(right, 286, 0);
  lv_obj_set_style_bg_color(right, lv_color_hex(0xFBF7F0), 0);
  lv_obj_set_style_border_width(right, 0, 0);
  lv_obj_set_style_radius(right, 0, 0);
  lv_obj_set_style_pad_left(right, 18, 0);
  lv_obj_set_style_pad_right(right, 18, 0);
  lv_obj_set_style_pad_top(right, 18, 0);
  lv_obj_set_style_pad_bottom(right, 10, 0);
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
  String sub_txt = c.author.length() ? c.author : (c.source == "abs" ? "Audiobookshelf" : "Yoto");
  lv_label_set_text(sub, sub_txt.c_str());
  lv_obj_set_style_text_font(sub, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(sub, lv_color_hex(0x7A7060), 0);

  lv_obj_t* sum = lv_label_create(right);
  detail_set_summary(sum);
  lv_obj_set_style_text_font(sum, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(sum, lv_color_hex(0x3A6B50), 0);
  lv_obj_set_style_pad_bottom(sum, 4, 0);

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

  lv_obj_t* old = s_scr_detail;
  s_scr_detail = scr;
  lv_scr_load(scr);
  if(old) lv_obj_del(old);
  lv_refr_now(lv_disp_get_default());   // paint the screen NOW so the tap feels instant

  // ---- slow path: now that the screen is up, fill in whatever wasn't served from SD ----
  if(!have_ch && fetch_card_detail(i)){         // blocking TLS GET /card/{id} (+ writes SD cache)
    lv_label_set_text(title, s_detail_title.c_str());
    detail_set_summary(sum);
    detail_fill_chapters(list);
    lv_refr_now(lv_disp_get_default());
  }
  if(!cover_sd && s_dcover &&
     load_cover_cached(c.id, c.cover, DCOVER_W, DCOVER_H, DCOVER_SZ, s_dcover)){
    lv_img_set_zoom(cover, 256);                // undo the thumb-placeholder upscale
    lv_img_set_pivot(cover, 0, 0);
    set_img(s_dcover_dsc, cover, s_dcover, DCOVER_W, DCOVER_H, DCOVER_SZ);
    lv_refr_now(lv_disp_get_default());
  }
}

// ---------------- now-playing screen + playback ----------------
static int send_cmd(const char* name, const String& body){
  JsonDocument in;
  deserializeJson(in, body);
  String nm(name);

  // If running in Core Managed mode and Core URL is set, dispatch through StoryDeck Core
  if(cfg::op_mode == "core" && cfg::core_url.length() > 0){
    if(nm == "play"){
      const char* cid = in["id"];
      int chIdx = 0;
      const char* ck = in["chapterKey"];
      if(ck && *ck) chIdx = atoi(ck);
      return core::play_media(cfg::core_url, "default", "yoto", cid ? cid : "", cfg::device_id, chIdx);
    } else if(nm == "volume"){
      int v = (int)(in["volume"] | 30); if(v < 0) v = 0; if(v > cfg::vol_max) v = cfg::vol_max;
      return core::player_control(cfg::core_url, cfg::device_id, "volume", v);
    } else {
      return core::player_control(cfg::core_url, cfg::device_id, nm.c_str());
    }
  }

  // Standalone mode: Translate the slim app command into a direct Yoto device command
  String cmd, payload;
  if(nm == "play"){
    cmd = "card/start";
    JsonDocument p;
    p["uri"] = String("https://yoto.io/") + (const char*)(in["id"] | "");
    const char* ck = in["chapterKey"]; if(ck && *ck) p["chapterKey"] = ck;
    const char* tk = in["trackKey"];   if(tk && *tk) p["trackKey"]   = tk;
    if(!in["secondsIn"].isNull()) p["secondsIn"] = in["secondsIn"];
    serializeJson(p, payload);
  } else if(nm == "pause" || nm == "resume" || nm == "stop"){
    cmd = "card/" + nm;
  } else if(nm == "volume"){
    int v = (int)(in["volume"] | 30); if(v < 0) v = 0; if(v > cfg::vol_max) v = cfg::vol_max;
    cmd = "volume/set";
    JsonDocument p; p["volume"] = v; serializeJson(p, payload);
  } else {
    return 404;
  }
  String path = String("/device-v2/") + cfg::device_id + "/command/" + cmd;
  String out;
  return yoto::post(path, payload.length() ? payload : String("{}"), out);
}

static void np_update_progress(){
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

// Sleep-timer countdown as label text ("Off" / "23 min left").
static void fmt_sleep(char* b, size_t n){
  if(s_sleep_sec <= 0)       snprintf(b, n, "Off");
  else if(s_sleep_sec >= 60) snprintf(b, n, "%d min left", (s_sleep_sec + 59) / 60);
  else                       snprintf(b, n, "%d sec left", s_sleep_sec);
}

static void saver_tick();   // fwd (defined with the screensaver below)

// ---- live device state (MQTT) -> the optimistic playback vars the UI reads ----
// Find the grid index of a card by its Yoto cardId (-1 if not in the library / unknown).
static int card_index_by_id(const char* id){
  if(!id || !*id) return -1;
  for(int i = 0; i < s_card_count; i++) if(s_cards[i].id == id) return i;
  return -1;
}

// Pull the latest MQTT snapshot (only when it has advanced) and snap our local playback state
// to the device's truth — this is what makes "now playing" reflect playback started from the
// iOS app or the player's buttons, not just from this panel. Live is authoritative; the 1 Hz
// optimistic clock then fills the seconds between events. Runs on the LVGL thread (from
// tick_cb), so touching LVGL widgets here is safe. The home bar is a full rebuild, so only
// redraw it when the IDENTITY changes (card/chapter/play-state) — position alone just nudges
// the cheap progress widgets, even if the player streams a position event every second.
static uint32_t s_live_seq_seen = 0;
static void apply_live_state(){
  if(ymqtt::seq() == s_live_seq_seen) return;       // nothing new since last tick
  ymqtt::Live lv; ymqtt::snapshot(lv);
  s_live_seq_seen = lv.seq;

  bool   was_playing  = s_playing;
  int    prev_card    = s_play_card;
  String prev_chapter = s_play_chapter;

  if(lv.status == ymqtt::ST_PLAYING)                                  s_playing = true;
  else if(lv.status == ymqtt::ST_PAUSED || lv.status == ymqtt::ST_STOPPED) s_playing = false;

  int idx = card_index_by_id(lv.cardId);
  if(idx >= 0){ s_play_card = idx; s_play_title = s_cards[idx].title; }
  if(lv.chapterTitle[0]) s_play_chapter = lv.chapterTitle;
  if(lv.chapterKey[0]){ int k = atoi(lv.chapterKey) - 1; if(k >= 0) s_play_ch = k; }  // "01"->0
  if(lv.trackLength > 0) s_play_dur = lv.trackLength;
  if(lv.position >= 0){ s_pos = lv.position; if(s_play_dur > 0 && s_pos > s_play_dur) s_pos = s_play_dur; }
  if(lv.volume > 0) s_vol = lv.volume;
  if(s_cur_card == s_play_card && s_play_ch >= 0) s_cur_ch = s_play_ch;  // keep now-playing labels aligned

  np_set_chapter_label(); np_set_playicon(); np_update_progress();
  if(s_playing != was_playing || s_play_card != prev_card || s_play_chapter != prev_chapter)
    update_home_bar();
}

// 1 Hz heartbeat: optimistic playback clock (ticks on EVERY screen now, not only while the
// now-playing screen is visible — it's the source of truth the widgets read), chapter
// auto-advance, the bedtime sleep timer, and screensaver arming.
static void tick_cb(lv_timer_t*){
  apply_live_state();                       // device truth first; the optimistic clock fills gaps

  // periodic resync / keepalive: re-request state every ~4 min (also keeps the broker from
  // dropping the idle connection — belt-and-suspenders alongside esp-mqtt's own PINGREQ).
  static uint32_t s_ka = 0;
  if(millis() - s_ka > 240000u){ s_ka = millis(); ymqtt::request_state(); }

  if(s_playing && s_play_dur > 0){
    if(s_pos < s_play_dur) s_pos++;
    if(s_pos >= s_play_dur){
      // the real player rolls into the next chapter by itself — mirror it optimistically.
      // (we only know the chapter list while the browsed card IS the playing card;
      //  otherwise hold at 100% like before.)
      if(s_cur_card == s_play_card && s_play_ch >= 0 && s_play_ch < s_ch_count-1){
        s_play_ch++; s_cur_ch = s_play_ch; s_pos = 0;
        s_play_chapter = s_ch[s_play_ch].title;
        s_play_dur     = s_ch[s_play_ch].dur;
        np_set_chapter_label(); update_home_bar();
      } else if(s_cur_card == s_play_card){
        s_playing = false; np_set_playicon(); update_home_bar();  // last chapter finished
      }
    }
    np_update_progress();
  }

  // bedtime sleep timer — stop the player when it reaches zero
    // Hotspot watchdog: if hotspot is supposed to be active, ensure it hasn't dropped
  ensure_hotspot_active();

  // If error/setup screen is visible with active hotspot, live-refresh connected stations count
  if(s_err_hs_client_lbl && lv_scr_act() == s_scr_error && (WiFi.getMode() & WIFI_MODE_AP)){
    static int s_last_sta_count = -1;
    int cur_sta = WiFi.softAPgetStationNum();
    if(cur_sta != s_last_sta_count){
      s_last_sta_count = cur_sta;
      if(cur_sta > 0){
        lv_label_set_text_fmt(s_err_hs_client_lbl, "%d device(s) connected! Open http://192.168.4.1/settings", cur_sta);
        lv_obj_set_style_text_color(s_err_hs_client_lbl, lv_color_hex(0x137333), 0);
      } else {
        lv_label_set_text(s_err_hs_client_lbl, "Broadcasting... Scan QR or connect to 'Yoto-Setup' (No password)");
        lv_obj_set_style_text_color(s_err_hs_client_lbl, lv_color_hex(0x5F6368), 0);
      }
    }
  }

  if(s_sleep_sec > 0){
    s_sleep_sec--;
    if(s_sleep_sec == 0){
      if(s_playing){ s_playing = false; np_set_playicon(); update_home_bar(); }
      send_cmd("stop", "{}");
    }
    if(s_set_sleep_lbl){ char b[24]; fmt_sleep(b, sizeof b); lv_label_set_text(s_set_sleep_lbl, b); }
  }
  if(s_np_sleep){
    if(s_sleep_sec > 0) lv_label_set_text_fmt(s_np_sleep, "Zz %d min", (s_sleep_sec + 59) / 60);
    else                lv_label_set_text(s_np_sleep, "");
  }

  // night-clock screensaver after idle (any touch resets LVGL's inactivity clock).
  // s_scr_home gate: never cover the boot/status screen — go_home() needs a home to return to.
  if(cfg::saver_min > 0 && s_scr_home && !s_scr_saver &&
     lv_disp_get_inactive_time(NULL) > (uint32_t)cfg::saver_min * 60000u) saver_open();
  if(s_scr_saver) saver_tick();
}

// Every command below paints its new UI state FIRST (lv_refr_now) and only then does the
// blocking TLS POST — taps feel instant instead of "dead" for the network round-trip,
// which is exactly when a 7yo taps again and double-fires.
static void np_goto_chapter(int k){
  if(k<0 || k>=s_ch_count) return;
  s_cur_ch=k; s_pos=0; s_playing=true;
  s_play_card=s_cur_card; s_play_ch=k;                 // keep the mini-player in sync
  s_play_author=s_cards[s_cur_card].author;
  s_play_chapter=s_ch[k].title; s_play_dur=s_ch[k].dur;
  np_set_chapter_label(); np_set_playicon(); np_update_progress(); update_home_bar();
  lv_refr_now(lv_disp_get_default());
  send_cmd("play", String("{\"id\":\"")+s_cards[s_cur_card].id+"\",\"chapterKey\":\""+s_ch[k].key+"\"}");
}
static void np_toggle(){
  s_playing = !s_playing;
  np_set_playicon(); update_home_bar();
  lv_refr_now(lv_disp_get_default());
  send_cmd(s_playing ? "resume" : "pause", "{}");
}
static void np_next(){ if(s_cur_ch < s_ch_count-1) np_goto_chapter(s_cur_ch+1); }
static void np_prev(){ if(s_cur_ch > 0)           np_goto_chapter(s_cur_ch-1); }
static void np_volume(int v){ s_vol=v; send_cmd("volume", String("{\"volume\":")+v+"}"); }

void go_home(){
  s_np_barfill=s_np_tcur=s_np_trem=s_np_playicon=s_np_chapter=s_np_sleep=nullptr;
  s_set_sleep_lbl=nullptr;
  if(s_scr_home) lv_scr_load(s_scr_home);
  else if(s_scr_error) lv_scr_load(s_scr_error);
  if(s_scr_detail){   lv_obj_del_async(s_scr_detail);   s_scr_detail=nullptr; }
  if(s_scr_now){      lv_obj_del_async(s_scr_now);      s_scr_now=nullptr; }
  if(s_scr_settings){ lv_obj_del_async(s_scr_settings); s_scr_settings=nullptr; }
  if(s_scr_pin){ lv_obj_del_async(s_scr_pin); s_scr_pin=nullptr; }
  if(s_scr_profile){ lv_obj_del_async(s_scr_profile); s_scr_profile=nullptr; }
}

// A one-second burst of soft confetti dots floating up the now-playing screen — the
// "your story is starting!" moment. Objects delete themselves when the rise finishes
// (lv_obj_del also kills the paired fade anim, so no dangling animations).
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
    lv_anim_t b = a;                            // matching fade-out, no delete callback
    lv_anim_set_exec_cb(&b, confetti_o);
    lv_anim_set_values(&b, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_ready_cb(&b, NULL);
    lv_anim_start(&b);
  }
}

static void play_from(int chapterIdx){
  if(s_cur_card<0 || chapterIdx<0 || chapterIdx>=s_ch_count) return;
  s_cur_ch=chapterIdx; s_pos=0; s_playing=true;
  // snapshot what's now playing for the home mini-player (independent of later browsing)
  s_play_card=s_cur_card; s_play_ch=chapterIdx;
  s_play_title=s_detail_title; s_play_chapter=s_ch[chapterIdx].title;
  s_play_dur=s_ch[chapterIdx].dur;
  update_home_bar();
  open_now();                                  // show the screen (and confetti) right away…
  lv_refr_now(lv_disp_get_default());
  send_cmd("play", String("{\"id\":\"")+s_cards[s_cur_card].id+"\",\"chapterKey\":\""+s_ch[chapterIdx].key+"\"}");
  if(s_scr_now) confetti(s_scr_now);           // little celebration once the player obeys
}

// Yoto Daily: dig into card 3nC80's "daily" chapter and pick the track for `today` (YYYYMMDD).
// If today's isn't published yet, fall back to the most recent track on/before today (never a
// future-dated one). Returns the chosen trackKey + its title/duration.
static bool fetch_daily_track(const String& today, String& outKey, String& outTitle, int& outDur){
  size_t blen = 0; char* body = nullptr;
  {
    tlsarena::Guard tg;
    WiFiClientSecure cli; HTTPClient http;
    int code = yoto::get(cli, http, String("/card/") + YOTO_DAILY_ID);
    if(code != 200){ if(code > 0) http.end(); return false; }
    body = read_body_psram(http, blen);
    http.end();
  }
  if(!body) return false;
  JsonDocument filter;
  filter["card"]["content"]["chapters"][0]["key"]                  = true;
  filter["card"]["content"]["chapters"][0]["tracks"][0]["key"]      = true;
  filter["card"]["content"]["chapters"][0]["tracks"][0]["title"]    = true;
  filter["card"]["content"]["chapters"][0]["tracks"][0]["duration"] = true;
  PsramAllocator psram;
  JsonDocument doc(&psram);
  DeserializationError err = deserializeJson(doc, (const char*)body, blen, DeserializationOption::Filter(filter));
  free(body);
  if(err) return false;
  String bestKey, bestTitle; int bestDur = 0;
  for(JsonObject ch : doc["card"]["content"]["chapters"].as<JsonArray>()){
    if(String((const char*)(ch["key"] | "")) != "daily") continue;
    for(JsonObject tr : ch["tracks"].as<JsonArray>()){
      String k = (const char*)(tr["key"] | "");
      if(k.length() == 0) continue;
      if(k == today){                                   // exact: today's episode
        outKey = k; outTitle = (const char*)(tr["title"] | "Today's episode");
        outDur = (int)(tr["duration"] | 0);
        return true;
      }
      if(k <= today && k > bestKey){                    // newest published on/before today
        bestKey = k; bestTitle = (const char*)(tr["title"] | "Today's episode");
        bestDur = (int)(tr["duration"] | 0);
      }
    }
    break;
  }
  if(bestKey.length()){ outKey = bestKey; outTitle = bestTitle; outDur = bestDur; return true; }
  return false;
}

// One-tap Yoto Daily: resolve today's track and start it on the player. We present it as a
// single-"chapter" card so the now-playing screen + home mini-player render with no special cases.
static void play_yoto_daily(int idx){
  struct tm tmv;
  if(!getLocalTime(&tmv, 500)){ status("Clock not synced yet — try again in a moment"); return; }
  char today[9]; strftime(today, sizeof today, "%Y%m%d", &tmv);

  String key, title; int dur = 0;
  if(!fetch_daily_track(String(today), key, title, dur)){ status("Couldn't load Yoto Daily"); return; }

  s_cur_card = idx; s_detail_title = "Yoto Daily";
  s_ch_count = 1; s_cur_ch = 0;
  s_ch[0].key = "daily"; s_ch[0].title = title; s_ch[0].dur = dur;
  s_pos = 0; s_playing = true;
  s_play_card = idx; s_play_ch = 0;
  s_play_title = "Yoto Daily"; s_play_chapter = title; s_play_dur = dur;
  update_home_bar();
  open_now();
  lv_refr_now(lv_disp_get_default());
  send_cmd("play", String("{\"id\":\"") + YOTO_DAILY_ID + "\",\"chapterKey\":\"daily\",\"trackKey\":\"" + key + "\"}");
  if(s_scr_now) confetti(s_scr_now);
}

static void open_now(){
  bool okv=false;
  if(!s_ncover) s_ncover=(uint8_t*)heap_caps_malloc(NCOVER_SZ, MALLOC_CAP_SPIRAM);
  if(s_ncover){
    okv=load_cover_cached(s_cards[s_cur_card].id, s_cards[s_cur_card].cover, NCOVER_W, NCOVER_H, NCOVER_SZ, s_ncover);
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

  // cover
  lv_obj_t* cover=lv_img_create(scr);
  lv_obj_set_size(cover, NCOVER_W, NCOVER_H); lv_obj_set_pos(cover, 44, 120);
  lv_obj_set_style_radius(cover, 14, 0); lv_obj_set_style_clip_corner(cover, true, 0);
  lv_obj_set_style_bg_color(cover, lv_color_hex(0xB3211A), 0);
  lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
  if(okv) set_img(s_ncover_dsc, cover, s_ncover, NCOVER_W, NCOVER_H, NCOVER_SZ);

  const int ix=224, iw=536;
  lv_obj_t* tt=lv_label_create(scr);
  lv_label_set_long_mode(tt, LV_LABEL_LONG_DOT);
  lv_obj_set_width(tt, iw); lv_obj_set_pos(tt, ix, 124);
  lv_label_set_text(tt, s_detail_title.c_str());
  lv_obj_set_style_text_font(tt, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(tt, lv_color_hex(0xFFFFFF), 0);

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
  lv_slider_set_range(slider, 0, cfg::vol_max);
  lv_slider_set_value(slider, s_vol > cfg::vol_max ? cfg::vol_max : s_vol, LV_ANIM_OFF);
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

// ---------------- grown-ups settings screen ----------------
// Opened by HOLDING the gear in the home bar for 2.5s. Parent knobs live here; WiFi and
// Yoto credentials live on the web portal instead (typing a 64-char client secret on a
// wall panel is nobody's idea of fun).
static lv_obj_t* s_set_vol_lbl=nullptr;     // only touched by callbacks living on the
static lv_obj_t* s_set_saver_lbl=nullptr;   // settings screen itself, so they can't dangle
static lv_obj_t* s_set_wake_lbl=nullptr;    // (s_set_sleep_lbl is tick-updated -> go_home nulls it)
static lv_obj_t* s_set_night_lbl=nullptr;
static lv_obj_t* s_set_size_lbl=nullptr;

static void fmt_wake(char* b, size_t n){
  int h12 = ((cfg::wake_h + 11) % 12) + 1;
  snprintf(b, n, "%d:%02d %s", h12, cfg::wake_m, cfg::wake_h < 12 ? "am" : "pm");
}

static lv_obj_t* set_section(lv_obj_t* p, const char* t){
  lv_obj_t* l = lv_label_create(p);
  lv_label_set_text(l, t);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(0x3A6B50), 0);
  lv_obj_set_style_pad_top(l, 12, 0);
  return l;
}
static lv_obj_t* set_row(lv_obj_t* p){
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
static lv_obj_t* pill_btn(lv_obj_t* row, const char* txt, int ud, lv_event_cb_t cb){
  lv_obj_t* b = lv_btn_create(row);
  lv_obj_set_size(b, 88, 56);
  lv_obj_set_style_radius(b, 14, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(0xF3E9D8), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_border_width(b, 1, 0);
  lv_obj_set_style_border_color(b, lv_color_hex(0xE7E0D2), 0);
  lv_obj_set_user_data(b, (void*)(intptr_t)ud);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(0x23303A), 0);
  lv_obj_center(l);
  return b;
}

static void set_sleep_cb(lv_event_t* e){
  s_sleep_sec = 60 * (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
  if(s_set_sleep_lbl){ char b[24]; fmt_sleep(b, sizeof b); lv_label_set_text(s_set_sleep_lbl, b); }
}
static void set_saver_cb(lv_event_t* e){
  cfg::saver_min = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
  cfg::save();
  if(s_set_saver_lbl){
    if(cfg::saver_min) lv_label_set_text_fmt(s_set_saver_lbl, "after %d min", cfg::saver_min);
    else               lv_label_set_text(s_set_saver_lbl, "Off");
  }
}
static void set_wake_cb(lv_event_t* e){
  int t = cfg::wake_h * 60 + cfg::wake_m + (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
  if(t < 5*60)  t = 5*60;                    // sane bedroom range: 5:00 – 10:00 am
  if(t > 10*60) t = 10*60;
  cfg::wake_h = t / 60; cfg::wake_m = t % 60;
  cfg::save();
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
  if(s_set_night_lbl){ char b[16]; fmt_night(b, sizeof b); lv_label_set_text(s_set_night_lbl, b); }
}
static const char* SV_SIZE_NAME[3] = {"Small", "Medium", "Large"};
static void set_size_cb(lv_event_t* e){
  int v = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
  cfg::saver_size = v < 0 ? 0 : v > 2 ? 2 : v;
  cfg::save();                               // applies the next time the clock comes on
  if(s_set_size_lbl) lv_label_set_text(s_set_size_lbl, SV_SIZE_NAME[cfg::saver_size]);
}
static void set_vol_drag_cb(lv_event_t* e){   // live readout while dragging
  if(s_set_vol_lbl) lv_label_set_text_fmt(s_set_vol_lbl, "%d", lv_slider_get_value(lv_event_get_target(e)));
}
static void set_vol_done_cb(lv_event_t* e){   // persist + apply on release
  int v = lv_slider_get_value(lv_event_get_target(e));
  cfg::vol_max = v; cfg::save();
  if(s_vol > v) np_volume(v);                 // pull the live volume under the new cap
}


// ---------------- Hotspot Watchdog & Visual UI Indicators ----------------
static lv_obj_t* s_err_hs_badge = nullptr;
static lv_obj_t* s_err_qr = nullptr;

static void start_hotspot(){
  s_hotspot_wanted = true;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("Yoto-Setup");
  Serial.println("[WIFI] Setup Hotspot asserted: 'Yoto-Setup' (192.168.4.1)");
}

static void ensure_hotspot_active(){
  if(s_hotspot_wanted && !s_lib_ok){
    // If we want the hotspot and are not yet fully online with a valid library, ensure AP mode is alive
    if(!(WiFi.getMode() & WIFI_MODE_AP) || WiFi.softAPIP() == IPAddress(0,0,0,0)){
      Serial.println("[WIFI] Watchdog: Hotspot went down! Re-enabling 'Yoto-Setup'...");
      WiFi.mode(WIFI_AP_STA);
      WiFi.softAP("Yoto-Setup");
    }
  }
}

static void start_hotspot_cb(lv_event_t*){
  start_hotspot();
  show_error("Setup Hotspot Active",
             "Connect your phone to WiFi 'Yoto-Setup' or scan the QR code to join.\nNavigate to http://192.168.4.1/settings to set up WiFi.",
             "Hotspot is live. Open browser to configure.");
}

static void show_error(const char* title, const char* message, const char* action_hint){
  if(s_scr_home && s_lib_ok) return; // don't cover home if already healthy
  if(!s_scr_error){
    s_scr_error = lv_obj_create(NULL);
  }
  lv_obj_clean(s_scr_error);
  lv_obj_set_style_bg_color(s_scr_error, lv_color_hex(0xFBF7F0), 0);
  s_err_hs_badge = nullptr;
  s_err_hs_client_lbl = nullptr;
  s_err_qr = nullptr;

  bool is_ap = (WiFi.getMode() & WIFI_MODE_AP);

  // Red accent header
  lv_obj_t* t = lv_label_create(s_scr_error);
  lv_label_set_text(t, title ? title : (is_ap ? "Setup Hotspot Active" : "Setup / Connection Needed"));
  lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(t, is_ap ? lv_color_hex(0x23303A) : lv_color_hex(0xE5352B), 0);
  lv_obj_set_pos(t, 24, 20);

  // Left Content Container (Width: 510px)
  lv_obj_t* left_box = lv_obj_create(s_scr_error);
  lv_obj_set_pos(left_box, 24, 64);
  lv_obj_set_size(left_box, 500, 310);
  lv_obj_set_style_bg_opa(left_box, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(left_box, 0, 0);
  lv_obj_set_style_pad_all(left_box, 0, 0);
  lv_obj_clear_flag(left_box, LV_OBJ_FLAG_SCROLLABLE);

  // Message body
  lv_obj_t* m = lv_label_create(left_box);
  lv_label_set_text(m, message ? message : "");
  lv_obj_set_style_text_font(m, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(m, lv_color_hex(0x23303A), 0);
  lv_obj_set_width(m, 500);

  if(is_ap){
    // Hotspot Active Card Badge
    lv_obj_t* hs_card = lv_obj_create(left_box);
    lv_obj_set_width(hs_card, 500);
    lv_obj_set_height(hs_card, 110);
    lv_obj_set_style_bg_color(hs_card, lv_color_hex(0xE6F4EA), 0); // Soft sage green
    lv_obj_set_style_border_width(hs_card, 1, 0);
    lv_obj_set_style_border_color(hs_card, lv_color_hex(0x3AB06A), 0);
    lv_obj_set_style_radius(hs_card, 12, 0);
    lv_obj_set_style_pad_all(hs_card, 12, 0);
    lv_obj_align(hs_card, LV_ALIGN_TOP_LEFT, 0, 60);

    lv_obj_t* hs_title = lv_label_create(hs_card);
    lv_label_set_text(hs_title, LV_SYMBOL_WIFI " Hotspot Active: 'Yoto-Setup'");
    lv_obj_set_style_text_font(hs_title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(hs_title, lv_color_hex(0x137333), 0);

    s_err_hs_client_lbl = lv_label_create(hs_card);
    int stations = WiFi.softAPgetStationNum();
    if(stations > 0){
      lv_label_set_text_fmt(s_err_hs_client_lbl, "%d device(s) connected! Open http://192.168.4.1/settings", stations);
      lv_obj_set_style_text_color(s_err_hs_client_lbl, lv_color_hex(0x137333), 0);
    } else {
      lv_label_set_text(s_err_hs_client_lbl, "Broadcasting... Scan QR or connect to 'Yoto-Setup' (No password)");
      lv_obj_set_style_text_color(s_err_hs_client_lbl, lv_color_hex(0x5F6368), 0);
    }
    lv_obj_set_style_text_font(s_err_hs_client_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(s_err_hs_client_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  }

  // Network Details / Hint
  String details = "";
  if(WiFi.status() == WL_CONNECTED){
    details += "WiFi Connected: " + WiFi.SSID() + " (" + String(WiFi.RSSI()) + " dBm)\n";
    details += "Device IP: " + WiFi.localIP().toString() + "\n";
    details += "Web Settings: http://" + WiFi.localIP().toString() + "/settings\n";
  } else if(!is_ap){
    details += "WiFi not connected. Background reconnection active.\n";
  }
  if(action_hint && action_hint[0]){
    details += String("Hint: ") + action_hint;
  }

  lv_obj_t* det = lv_label_create(left_box);
  lv_label_set_text(det, details.c_str());
  lv_obj_set_style_text_font(det, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(det, lv_color_hex(0x7A7060), 0);
  lv_obj_set_width(det, 500);
  lv_obj_align(det, LV_ALIGN_TOP_LEFT, 0, is_ap ? 180 : 80);

  // Right QR Code Card (Width: 230px, Height: 290px)
  lv_obj_t* qr_card = lv_obj_create(s_scr_error);
  lv_obj_set_pos(qr_card, 545, 64);
  lv_obj_set_size(qr_card, 230, 290);
  lv_obj_set_style_bg_color(qr_card, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_border_width(qr_card, 1, 0);
  lv_obj_set_style_border_color(qr_card, lv_color_hex(0xE7E0D2), 0);
  lv_obj_set_style_radius(qr_card, 16, 0);
  lv_obj_set_style_pad_all(qr_card, 10, 0);
  lv_obj_clear_flag(qr_card, LV_OBJ_FLAG_SCROLLABLE);

  // Generate QR Code data
  String qr_data = is_ap ? "WIFI:T:nopass;S:Yoto-Setup;;" : ("http://" + WiFi.localIP().toString() + "/settings");
  lv_obj_t* qr = lv_qrcode_create(qr_card, 160, lv_color_hex(0x23303A), lv_color_hex(0xFFFFFF));
  lv_qrcode_update(qr, qr_data.c_str(), qr_data.length());
  lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 8);
  s_err_qr = qr;

  lv_obj_t* qr_label = lv_label_create(qr_card);
  lv_label_set_text(qr_label, is_ap ? "Scan to Join Hotspot\nWiFi: Yoto-Setup" : "Scan for Settings\nWeb Portal");
  lv_obj_set_style_text_font(qr_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(qr_label, lv_color_hex(0x7A7060), 0);
  lv_obj_set_style_text_align(qr_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(qr_label, LV_ALIGN_BOTTOM_MID, 0, -4);

  // Buttons container (Row at bottom)
  lv_obj_t* bcont = set_row(s_scr_error);
  lv_obj_set_pos(bcont, 24, 395);
  lv_obj_set_style_pad_column(bcont, 16, 0);

  lv_obj_t* b_set = pill_btn(bcont, "Open Settings", 0, [](lv_event_t*){ open_settings(); });
  lv_obj_set_size(b_set, 210, 56);
  lv_obj_set_style_bg_color(b_set, lv_color_hex(0xE5352B), 0);
  lv_obj_set_style_text_color(lv_obj_get_child(b_set, 0), lv_color_hex(0xFFFFFF), 0);

  lv_obj_t* b_hs = pill_btn(bcont, is_ap ? "Hotspot Active" : "Start Hotspot", 0, start_hotspot_cb);
  lv_obj_set_size(b_hs, 190, 56);
  if(is_ap){
    lv_obj_set_style_bg_color(b_hs, lv_color_hex(0x3AB06A), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(b_hs, 0), lv_color_hex(0xFFFFFF), 0);
  }

  lv_obj_t* b_try = pill_btn(bcont, "Retry", 0, [](lv_event_t*){ net_init_and_fetch(); });
  lv_obj_set_size(b_try, 140, 56);

  lv_scr_load(s_scr_error);
  lv_refr_now(lv_disp_get_default());
}



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

static void open_profile_picker(){
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

// ---------------- Parent PIN Keypad Modal (pin.js parity) ----------------
static String s_pin_entered = "";
static int    s_pin_fails = 0;
static uint32_t s_pin_lockout_until = 0;
static lv_obj_t* s_pin_dots[4] = {nullptr, nullptr, nullptr, nullptr};
static lv_obj_t* s_pin_msg_lbl = nullptr;
static bool s_pin_change_mode = false;
static String s_pin_new_temp = "";

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
  if(s_scr_pin){
    lv_obj_t* p = s_scr_pin;
    s_scr_pin = nullptr;
    if(s_scr_prev) lv_scr_load(s_scr_prev);
    else if(s_scr_home) lv_scr_load(s_scr_home);
    else if(s_scr_error) lv_scr_load(s_scr_error);
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
    if(s_pin_msg_lbl) lv_label_set_text(s_pin_msg_lbl, s_pin_change_mode ? "Enter new 4-digit PIN" : "Enter 4-digit PIN to access Grown-ups settings");
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
    if(s_pin_change_mode){
      // PIN change mode: saves new PIN to NVS
      cfg::parent_pin = s_pin_entered;
      cfg::save();
      if(s_pin_msg_lbl) lv_label_set_text(s_pin_msg_lbl, "PIN changed successfully!");
      lv_refr_now(lv_disp_get_default());
      delay(600);
      close_pin_modal();
      return;
    }

    if(s_pin_entered == cfg::parent_pin){
      s_pin_fails = 0;
      s_pin_entered = "";
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

static void open_pin_modal(bool for_settings_change){
  if(!for_settings_change && !s_scr_prev){
    s_scr_prev = lv_scr_act();
  }
  s_pin_change_mode = for_settings_change;
  s_pin_entered = "";

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
  lv_obj_t* title = lv_label_create(scr);
  lv_label_set_text(title, for_settings_change ? "Change Parent PIN" : "Parent PIN Required");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xE5352B), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

  // Subtitle / message
  s_pin_msg_lbl = lv_label_create(scr);
  lv_label_set_text(s_pin_msg_lbl, for_settings_change ? "Enter new 4-digit PIN" : "Enter 4-digit PIN to access Grown-ups settings");
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

static void open_settings(){
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
    if(s_scr_prev == s_scr_profile && s_scr_profile){
      if(s_scr_settings){ lv_obj_del_async(s_scr_settings); s_scr_settings=nullptr; }
      lv_scr_load(s_scr_profile);
      s_scr_prev = nullptr;
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
    lv_obj_set_style_pad_row(panel, 10, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    s_gu_tab_panels[i] = panel;
  }

  // ==========================================
  // TAB 0: 🌙 SLEEP / BED
  // ==========================================
  lv_obj_t* p0 = s_gu_tab_panels[0];

  set_section(p0, "Sleep timer  (stops player when elapsed)");
  lv_obj_t* r_sleep = set_row(p0);
  pill_btn(r_sleep, "Off", 0,  set_sleep_cb);
  pill_btn(r_sleep, "15m", 15, set_sleep_cb);
  pill_btn(r_sleep, "30m", 30, set_sleep_cb);
  pill_btn(r_sleep, "45m", 45, set_sleep_cb);
  pill_btn(r_sleep, "60m", 60, set_sleep_cb);
  s_set_sleep_lbl = lv_label_create(r_sleep);
  { char b[24]; fmt_sleep(b, sizeof b); lv_label_set_text(s_set_sleep_lbl, b); }
  lv_obj_set_style_text_font(s_set_sleep_lbl, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(s_set_sleep_lbl, lv_color_hex(0x9A9384), 0);

  set_section(p0, "Bedtime Curfew  (clock turns red)");
  lv_obj_t* r_night = set_row(p0);
  pill_btn(r_night, "-15m", -15, set_night_cb);
  s_set_night_lbl = lv_label_create(r_night);
  { char b[16]; fmt_night(b, sizeof b); lv_label_set_text(s_set_night_lbl, b); }
  lv_obj_set_style_text_font(s_set_night_lbl, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(s_set_night_lbl, lv_color_hex(0x23303A), 0);
  pill_btn(r_night, "+15m", 15, set_night_cb);

  set_section(p0, "OK-to-wake Schedule  (clock turns green)");
  lv_obj_t* r_wake = set_row(p0);
  pill_btn(r_wake, "-15m", -15, set_wake_cb);
  s_set_wake_lbl = lv_label_create(r_wake);
  { char b[16]; fmt_wake(b, sizeof b); lv_label_set_text(s_set_wake_lbl, b); }
  lv_obj_set_style_text_font(s_set_wake_lbl, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(s_set_wake_lbl, lv_color_hex(0x23303A), 0);
  pill_btn(r_wake, "+15m", 15, set_wake_cb);

  // ==========================================
  // TAB 1: 🎯 AUDIO TARGET & ROAMING
  // ==========================================
  lv_obj_t* p1 = s_gu_tab_panels[1];

  set_section(p1, "Target Player Roaming vs Locking");
  lv_obj_t* r_lock = set_row(p1);
  lv_obj_t* b_roam = pill_btn(r_lock, "Profile Roaming", 0, [](lv_event_t*){
    cfg::lock_player = false;
    cfg::save();
    open_settings();
  });
  lv_obj_set_size(b_roam, 170, 52);
  if(!cfg::lock_player){
    lv_obj_set_style_bg_color(b_roam, lv_color_hex(0xE5352B), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(b_roam, 0), lv_color_hex(0xFFFFFF), 0);
  }

  lv_obj_t* b_lock = pill_btn(r_lock, "Lock to Player", 1, [](lv_event_t*){
    cfg::lock_player = true;
    cfg::locked_dev_id = cfg::device_id;
    cfg::save();
    open_settings();
  });
  lv_obj_set_size(b_lock, 170, 52);
  if(cfg::lock_player){
    lv_obj_set_style_bg_color(b_lock, lv_color_hex(0xE5352B), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(b_lock, 0), lv_color_hex(0xFFFFFF), 0);
  }

  // Target Player Dropdown Selector
  lv_obj_t* r_drop = set_row(p1);
  lv_obj_t* dd_lbl = lv_label_create(r_drop);
  lv_label_set_text(dd_lbl, "Target Player:");
  lv_obj_set_style_text_font(dd_lbl, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(dd_lbl, lv_color_hex(0x23303A), 0);

  lv_obj_t* dd = lv_dropdown_create(r_drop);
  lv_obj_set_size(dd, 340, 48);
  lv_obj_set_style_text_font(dd, &lv_font_montserrat_16, 0);

  // Populate options from discovered profiles / default player
  String opt_str = "";
  int cur_sel = 0;
  int opt_idx = 0;
  String seen_ids[10];
  int seen_count = 0;

  auto add_opt = [&](const String& pid, const String& pname) {
    if(pid.length() == 0) return;
    for(int s = 0; s < seen_count; s++) if(seen_ids[s] == pid) return;
    seen_ids[seen_count++] = pid;
    if(opt_str.length() > 0) opt_str += "\n";
    opt_str += (pname.length() > 0 ? pname : String("Player ") + pid);
    if(cfg::device_id == pid) cur_sel = opt_idx;
    opt_idx++;
  };

  if(cfg::device_id.length() > 0){
    add_opt(cfg::device_id, get_active_player_name());
  }
  for(int p = 0; p < core::s_profile_count; p++){
    add_opt(core::s_profiles[p].default_player_id, core::s_profiles[p].default_player_name);
  }
  if(opt_str.length() == 0) opt_str = "No players discovered";

  lv_dropdown_set_options(dd, opt_str.c_str());
  lv_dropdown_set_selected(dd, cur_sel);
  lv_obj_add_event_cb(dd, [](lv_event_t* e){
    lv_obj_t* d = lv_event_get_target(e);
    uint16_t sel = lv_dropdown_get_selected(d);
    // Find player ID for this option
    String opts[10];
    int o_cnt = 0;
    auto reg_id = [&](const String& pid){
      if(pid.length() == 0) return;
      for(int s = 0; s < o_cnt; s++) if(opts[s] == pid) return;
      opts[o_cnt++] = pid;
    };
    if(cfg::device_id.length() > 0) reg_id(cfg::device_id);
    for(int p = 0; p < core::s_profile_count; p++) reg_id(core::s_profiles[p].default_player_id);
    if(sel < o_cnt){
      cfg::device_id = opts[sel];
      if(cfg::lock_player) cfg::locked_dev_id = opts[sel];
      cfg::save();
    }
  }, LV_EVENT_VALUE_CHANGED, NULL);

  lv_obj_t* lock_hint = lv_label_create(p1);
  if(cfg::lock_player){
    lv_label_set_text_fmt(lock_hint, "Locked: Output locked to selected player above.\nChanging profiles will update cards, but audio stays on this player.",
                          cfg::device_id.c_str());
  } else {
    lv_label_set_text(lock_hint, "Profile Roaming: Audio dynamically routes to each child profile's bedroom player.\n(Lock to Player locks audio to the selected player above).");
  }
  lv_obj_set_style_text_font(lock_hint, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(lock_hint, lv_color_hex(0x7A7060), 0);

  set_section(p1, "Operational Mode");
  lv_obj_t* r_mode = set_row(p1);
  lv_obj_t* b_mcore = pill_btn(r_mode, "Core Managed", 0, [](lv_event_t*){
    cfg::op_mode = "core";
    cfg::save();
    open_settings();
  });
  lv_obj_set_size(b_mcore, 160, 52);
  if(cfg::op_mode == "core"){
    lv_obj_set_style_bg_color(b_mcore, lv_color_hex(0xE5352B), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(b_mcore, 0), lv_color_hex(0xFFFFFF), 0);
  }

  lv_obj_t* b_mstand = pill_btn(r_mode, "Standalone Cloud", 1, [](lv_event_t*){
    cfg::op_mode = "standalone";
    cfg::save();
    open_settings();
  });
  lv_obj_set_size(b_mstand, 170, 52);
  if(cfg::op_mode != "core"){
    lv_obj_set_style_bg_color(b_mstand, lv_color_hex(0xE5352B), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(b_mstand, 0), lv_color_hex(0xFFFFFF), 0);
  }

  lv_obj_t* mode_hint = lv_label_create(p1);
  lv_label_set_text(mode_hint, cfg::op_mode == "core"
    ? "Core Managed: Hub-assisted multi-child profiles, ABS audiobooks & local proxy."
    : "Standalone: Direct connection to Yoto Cloud without requiring a local Core server.");
  lv_obj_set_style_text_font(mode_hint, &lv_font_montserrat_18, 0);
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
  pill_btn(r_saver, "Off", 0,  set_saver_cb);
  pill_btn(r_saver, "5m",  5,  set_saver_cb);
  pill_btn(r_saver, "10m", 10, set_saver_cb);
  pill_btn(r_saver, "30m", 30, set_saver_cb);
  s_set_saver_lbl = lv_label_create(r_saver);
  if(cfg::saver_min) lv_label_set_text_fmt(s_set_saver_lbl, "after %d min", cfg::saver_min);
  else               lv_label_set_text(s_set_saver_lbl, "Off");
  lv_obj_set_style_text_font(s_set_saver_lbl, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(s_set_saver_lbl, lv_color_hex(0x9A9384), 0);

  set_section(p2, "Bedside clock digit size");
  lv_obj_t* r_size = set_row(p2);
  lv_obj_set_size(pill_btn(r_size, "Small",  0, set_size_cb), 124, 52);
  lv_obj_set_size(pill_btn(r_size, "Medium", 1, set_size_cb), 124, 52);
  lv_obj_set_size(pill_btn(r_size, "Large",  2, set_size_cb), 124, 52);
  s_set_size_lbl = lv_label_create(r_size);
  lv_label_set_text(s_set_size_lbl, SV_SIZE_NAME[cfg::saver_size < 0 ? 0 : cfg::saver_size > 2 ? 2 : cfg::saver_size]);
  lv_obj_set_style_text_font(s_set_size_lbl, &lv_font_montserrat_20, 0);
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
  lv_obj_set_size(rb, 160, 52);
  lv_obj_set_style_text_color(rb, lv_color_hex(0xE5352B), 0);
  lv_obj_set_style_text_color(lv_obj_get_child(rb, 0), lv_color_hex(0xE5352B), 0);

  lv_obj_t* hs = pill_btn(r_maint, "Start Hotspot", 0, start_hotspot_cb);
  lv_obj_set_size(hs, 160, 52);

  // Set initial active tab visibility
  gu_tab_switch(s_gu_active_tab);

  s_scr_settings = scr;
  lv_scr_load(scr);
}

// ---------------- OTA: wireless firmware updates ----------------
// Two doors into the same flash-over-WiFi mechanism (both need WiFi up first; USB-C stays
// the recovery path if a bad image ever wedges the app):
//   1. ArduinoOTA — push from the Mac:  pio run -e waveshare-s3-43b-ota -t upload
//   2. ElegantOTA — open http://yoto-controller.local/update (or http://<board-ip>/update)
//      in any browser and drop in .pio/build/waveshare-s3-43b/firmware.bin
// Updates land in the spare app slot and only swap over on success, and they DON'T touch NVS,
// so the Yoto tokens survive. mDNS advertises the board as "yoto-controller.local".
static WebServer s_ota_web(80);

// "Loading new software" splash for firmware updates. The hard constraint: on the ESP32-S3 every
// flash write/erase momentarily DISABLES the MMU cache, and the LCD framebuffer is read from PSRAM
// THROUGH that cache, so the panel literally scans garbage for the whole transfer no matter what's
// in the FB ("screen goes nuts"). We can't hold a live image during the write. So instead: paint
// the message ONCE (this fires before the first flash write, while the cache is still on, so it
// renders clean), hold it briefly so it's readable, then BLANK the backlight (EXIO_BL_DISP) so the
// transfer runs on a clean dark screen instead of garbage. On success the board reboots and setup()
// turns the backlight back on; on failure ota_restore_display() brings it back (see onError/onEnd).
static lv_obj_t* s_scr_ota = nullptr;
static void ota_splash_and_blank(){
  if(!s_scr_ota){
    s_scr_ota = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_ota, lv_color_hex(0xE5352B), 0);   // red hero bg
    lv_obj_clear_flag(s_scr_ota, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* t = lv_label_create(s_scr_ota);
    lv_label_set_text(t, "Loading new software");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -18);

    lv_obj_t* sub = lv_label_create(s_scr_ota);
    lv_label_set_text(sub, "Please wait...");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(0xFFE3E0), 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 28);

    lv_scr_load(s_scr_ota);
  }
  lv_refr_now(lv_disp_get_default());   // clean paint: runs before the first flash write
  delay(1200);                          // hold long enough to read
  ch422g_set(EXIO_BL_DISP, false);      // blank: dark beats garbage during the cache-disabling writes
}
// Recover the display if an OTA FAILS (no reboot happens, loop() resumes — and its 1 Hz backlight
// self-heal would otherwise keep re-asserting the now-OFF shadow bit, leaving the screen dark).
static void ota_restore_display(){
  ch422g_set(EXIO_BL_DISP, true);
  if(s_scr_home) lv_scr_load(s_scr_home);
  lv_refr_now(lv_disp_get_default());
}

static void ota_begin(){
  ArduinoOTA.setHostname("yoto-controller");
  if(sizeof(OTA_PASSWORD) > 1) ArduinoOTA.setPassword(OTA_PASSWORD);   // empty string => no auth
  ArduinoOTA.onStart([](){ Serial.println("[OTA] push start"); ota_splash_and_blank(); });
  ArduinoOTA.onEnd([](){   Serial.println("[OTA] push done");  });   // success -> board reboots
  ArduinoOTA.onError([](ota_error_t e){ Serial.printf("[OTA] error %u\n", e); ota_restore_display(); });
  ArduinoOTA.begin();

  s_ota_web.on("/", [](){ s_ota_web.send(200, "text/plain",
      "Yoto Controller. Wireless firmware update at /update"); });
  // Telemetry for tuning the draw buffer vs internal-RAM headroom. cards>0 confirms the
  // library loaded; minFree is the boot-time low-water mark (worst dip during TLS handshakes).
  s_ota_web.on("/heap", [](){
    char b[240];
    snprintf(b, sizeof(b),
      "{\"drawLines\":%d,\"cards\":%d,\"freeInternal\":%u,\"largestInternal\":%u,\"minFreeInternal\":%u,\"tlsArena\":%u,\"pcCursor\":%d,\"pcDone\":%d}",
      DRAW_LINES, s_card_count,
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
      (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
      (unsigned)tlsarena::s_size, s_pc_cursor, s_pc_done ? 1 : 0);
    s_ota_web.send(200, "application/json", b);
  });
  // Display-flicker probe. GET /flick?stress=SECONDS injects heavy PSRAM bus load for that long
  // (0..120) so we can SEE whether the left-edge static flicker is PSRAM-bandwidth contention.
  // Watch the panel: edge worsens under load + recovers after = RGB-DMA underrun confirmed.
  // No-arg GET just reports the live display config + remaining stress time + free RAM.
  s_ota_web.on("/flick", [](){
    if(!s_flick_task)
      xTaskCreatePinnedToCore(flick_stress_task, "flick", 4096, nullptr, 1, &s_flick_task, 0);
    if(s_ota_web.hasArg("stress")){
      int sec = s_ota_web.arg("stress").toInt();
      if(sec < 0) sec = 0; if(sec > 120) sec = 120;
      s_flick_until = millis() + (uint32_t)sec*1000u;
    }
    int32_t left = (int32_t)(s_flick_until - millis()); if(left < 0) left = 0;
    char b[256];
    snprintf(b, sizeof(b),
      "{\"bouncePx\":%d,\"pclkHz\":%d,\"fbInPsram\":1,\"numFbs\":1,\"drawLines\":%d,"
      "\"stressMsLeft\":%d,\"freeSpiram\":%u,\"largestSpiram\":%u,\"freeInternal\":%u}",
      LCD_H_RES*10, 16*1000*1000, DRAW_LINES, (int)left,
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    s_ota_web.send(200, "application/json", b);
  });
  // SD cache health: is the card mounted, and is the cover/chapter cache actually populating?
  // Lists the first handful of files in /ythumbs with sizes so we can SEE whether detail
  // opens are writing {id}.chap (chapters) and {id}_184.565 (detail cover) blobs.
  s_ota_web.on("/sd", [](){
    String out = "{\"mounted\":" + String(s_sd_ok ? 1 : 0);
    if(s_sd_ok){
      out += ",\"type\":" + String((int)SD.cardType());
      out += ",\"sizeMB\":" + String((uint32_t)(SD.cardSize()/(1024ULL*1024ULL)));
      out += ",\"usedMB\":" + String((uint32_t)(SD.usedBytes()/(1024ULL*1024ULL)));
      int nchap=0, ncover=0, ntot=0;
      String sample = "[";
      File dir = SD.open(SD_DIR);
      if(dir){
        for(File f = dir.openNextFile(); f; f = dir.openNextFile()){
          String nm = f.name();
          if(nm.endsWith(".chap")) nchap++;
          else if(nm.endsWith(".565")) ncover++;
          if(ntot < 8){ if(ntot) sample += ","; sample += "\"" + nm + ":" + String((uint32_t)f.size()) + "\""; }
          ntot++;
          f.close();
        }
        dir.close();
      }
      sample += "]";
      out += ",\"files\":" + String(ntot) + ",\"chap\":" + String(nchap) +
             ",\"cover\":" + String(ncover) + ",\"sample\":" + sample;
    }
    out += "}";
    s_ota_web.send(200, "application/json", out);
  });
  // Live MQTT state probe: connection health, parsed playback snapshot, and the LAST RAW
  // event payload verbatim (the `event` field is the real wire JSON, embedded as-is) so the
  // parser in yoto_mqtt.h can be confirmed/corrected against what the player actually sends.
  // Play a card from the iOS app or the player's buttons, then curl this to see it land.
  s_ota_web.on("/mqtt", [](){
    if(s_ota_web.hasArg("kick")) ymqtt::kick();   // force reconnect+refresh to exercise stack peak
    ymqtt::Live lv; ymqtt::snapshot(lv);
    const char* raw = ymqtt::last_raw();
    String out = "{";
    out += "\"connected\":" + String(ymqtt::connected() ? 1 : 0);
    out += ",\"supHWM\":" + String(ymqtt::sup_hwm());
    out += ",\"events\":"   + String(ymqtt::events());
    out += ",\"errors\":"   + String(ymqtt::errors());
    out += ",\"seq\":"      + String(lv.seq);
    out += ",\"online\":"   + String(lv.online ? 1 : 0);
    out += ",\"cardId\":\"" + String(lv.cardId) + "\"";
    out += ",\"chapterKey\":\"" + String(lv.chapterKey) + "\"";
    out += ",\"status\":"   + String((int)lv.status);   // 0 unknown 1 stopped 2 paused 3 playing
    out += ",\"pos\":"      + String(lv.position);
    out += ",\"dur\":"      + String(lv.trackLength);
    out += ",\"vol\":"      + String(lv.volume);
    out += ",\"ageMs\":"    + String(lv.ts ? (millis() - lv.ts) : 0);
    out += ",\"topic\":\""  + String(ymqtt::last_topic()) + "\"";
    out += ",\"event\":";   out += (raw && raw[0]) ? raw : "null";
    out += "}";
    s_ota_web.send(200, "application/json", out);
  });
  // Screensaver-arming diagnostics: poll this hands-off and watch inactiveMs climb toward
  // saverMin*60000. If it resets with nobody touching the panel, touchSamples/lastTouchMs
  // will show whether the GT911 reported a phantom press (LVGL resets its inactivity clock
  // on every PRESSED sample). Reads shared state from loop() context — same thread as
  // lv_timer_handler, so no race.
  s_ota_web.on("/saver", [](){
    // resetReason (esp_reset_reason_t): 1 POWERON, 3 SW, 4 PANIC, 5 INT_WDT, 6 TASK_WDT,
    // 7 other WDT, 9 BROWNOUT — the key forensic bit for the 2026-06-10 reboot loop
    char b[360];
    snprintf(b, sizeof(b),
      "{\"uptimeMs\":%lu,\"inactiveMs\":%lu,\"saverMin\":%d,\"haveHome\":%d,\"saverShown\":%d,"
      "\"touchSamples\":%lu,\"lastTouchMs\":%lu,\"tstate\":%d,\"resetReason\":%d,"
      "\"blRecoveries\":%lu,\"freeInternal\":%u,\"minFreeInternal\":%u}",
      (unsigned long)millis(), (unsigned long)lv_disp_get_inactive_time(NULL),
      cfg::saver_min, s_scr_home ? 1 : 0, s_scr_saver ? 1 : 0,
      (unsigned long)s_touch_samples, (unsigned long)s_tlast, (int)s_tstate,
      (int)esp_reset_reason(),
      (unsigned long)s_bl_recoveries,
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
      (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    s_ota_web.send(200, "application/json", b);
  });
  // Live probe of the detail-screen fetch paths (authed /card/{id} + CDN cover), since the
  // failures only reproduce on-device. ?card=N picks a grid index; ?expire=1 corrupts the
  // cached access token first so the 403-refresh-retry path runs NOW instead of in an hour.
  // Blocks the UI for a few seconds while it runs — it's a diagnostic, not a feature.
  s_ota_web.on("/diag", [](){
    String out = "{\"freeInt\":" + String(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)) +
                 ",\"largest\":" + String(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)) +
                 ",\"arena\":" + String(tlsarena::s_size);
    size_t hole;
    {
      // what mbedTLS actually sees when the arena opens for a call (no String growth in
      // here — an alloc landing inside the open hole would split it permanently)
      tlsarena::Guard g;
      hole = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    }
    out += ",\"holeLargest\":" + String(hole);
    if(s_card_count == 0){
      out += ",\"err\":\"no cards loaded\"}";
      s_ota_web.send(200, "application/json", out);
      return;
    }
    int i = s_ota_web.hasArg("card") ? s_ota_web.arg("card").toInt() : 0;
    if(i < 0 || i >= s_card_count) i = 0;
    Card& c = s_cards[i];
    out += ",\"id\":\"" + c.id + "\"";
    if(s_ota_web.arg("expire") == "1"){
      yoto::s_access = "expired-on-purpose";   // NOT persisted; next refresh overwrites
      out += ",\"forcedExpiry\":true";
    }
    int apiCode; size_t blen = 0; uint32_t apiMs;
    {
      tlsarena::Guard tg;
      WiFiClientSecure cli; HTTPClient http;
      uint32_t t0 = millis();
      apiCode = yoto::get(cli, http, "/card/" + c.id);
      if(apiCode == 200){ char* b = read_body_psram(http, blen); if(b) free(b); }
      if(apiCode > 0) http.end();
      apiMs = millis() - t0;
    }
    out += ",\"api\":" + String(apiCode) + ",\"apiBytes\":" + String((unsigned)blen) +
           ",\"apiMs\":" + String(apiMs);
    out += ",\"freeIntMid\":" + String(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)) +
           ",\"largestMid\":" + String(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    {
      // Plain-HTTP reachability (any code > 0 = DNS+TCP fine, isolating the failure to TLS)
      WiFiClient pc; HTTPClient ph;
      ph.begin(pc, "http://api.yotoplay.com/");
      ph.setTimeout(8000);
      out += ",\"plainHttp\":" + String(ph.GET());
      ph.end();
    }
    bool tlsOk; char eb[120] = "";
    {
      // Raw TLS connect + the mbedtls error string ("memory allocation failed" vs DNS etc.)
      tlsarena::Guard tg;
      WiFiClientSecure tc; tc.setInsecure();
      tlsOk = tc.connect("api.yotoplay.com", 443);
      if(!tlsOk) tc.lastError(eb, sizeof(eb));
      tc.stop();
    }
    out += ",\"tlsConnect\":" + String(tlsOk ? 1 : 0) + ",\"tlsErr\":\"" + String(eb) + "\"";
    if(s_ota_web.hasArg("status")){
      // Probe GET /device-v2/{id}/status with the board's CURRENT token — settles whether
      // live device state needs any extra scope or works with what we already hold.
      int stCode; String snip;
      {
        tlsarena::Guard tg;
        WiFiClientSecure cli; HTTPClient http;
        stCode = yoto::get(cli, http, "/device-v2/" + cfg::device_id + "/status");
        size_t sl = 0; char* sb = nullptr;
        if(stCode == 200){ sb = read_body_psram(http, sl); }
        if(stCode > 0) http.end();
        if(sb){ snip = String(sb).substring(0, 220); free(sb); }
      }
      snip.replace("\\", ""); snip.replace("\"", "'");   // keep the diag JSON parseable
      out += ",\"status\":" + String(stCode) + ",\"statusBody\":\"" + snip + "\"";
    }
    if(s_ota_web.hasArg("daily")){
      // Reproduce the exact Yoto Daily play path and surface what the firmware throws away:
      // the resolved trackKey AND the card/start response code+body. Settles whether the
      // failure is a bad/absent trackKey or the API rejecting a card/start for 3nC80.
      out += ",\"dailyId\":\"" + String(YOTO_DAILY_ID) + "\"";
      struct tm tmv;
      if(!getLocalTime(&tmv, 500)){
        out += ",\"dailyErr\":\"clock not synced\"";
      } else {
        char today[9]; strftime(today, sizeof today, "%Y%m%d", &tmv);
        out += ",\"today\":\"" + String(today) + "\"";
        String key, title; int dur = 0;
        bool found = fetch_daily_track(String(today), key, title, dur);
        out += ",\"dailyFound\":" + String(found ? 1 : 0) +
               ",\"trackKey\":\"" + key + "\",\"trackDur\":" + String(dur);
        if(found){
          // identical payload to send_cmd("play", {...}) for the daily tile
          String payload = String("{\"uri\":\"https://yoto.io/") + YOTO_DAILY_ID +
                           "\",\"chapterKey\":\"daily\",\"trackKey\":\"" + key + "\"}";
          String path = String("/device-v2/") + cfg::device_id + "/command/card/start";
          String rb; int sc;
          { tlsarena::Guard tg; sc = yoto::post(path, payload, rb); }
          rb.replace("\\", ""); rb.replace("\"", "'");   // keep diag JSON parseable
          out += ",\"startCode\":" + String(sc) + ",\"startBody\":\"" + rb.substring(0, 220) + "\"";
        }
      }
    }
    {
      uint8_t* buf = (uint8_t*)heap_caps_malloc(DCOVER_SZ, MALLOC_CAP_SPIRAM);
      bool ok = false;
      s_thumb_diag = "";
      uint32_t t0 = millis();
      if(buf){ ok = fetch_cover(c.cover, DCOVER_W, DCOVER_H, buf); free(buf); }
      else s_thumb_diag = "psram alloc fail";
      out += ",\"cover\":" + String(ok ? 1 : 0) + ",\"coverDiag\":\"" + s_thumb_diag +
             "\",\"coverMs\":" + String(millis() - t0);
    }
    out += "}";
    s_ota_web.send(200, "application/json", out);
  });
  s_ota_web.on("/bl", [](){
    // Manually re-assert the CH422G outputs (EXIO2 = backlight + DISP enable). If the screen is
    // black and hitting this brings it back, the expander's output latch was cleared by a supply
    // glitch (the ESP rides through the dip, so it never reboots) — not a dead backlight rail.
    // Full re-init (WR_SET + settle + WR_IO) so it recovers even a complete chip reset; the WR_SET
    // write momentarily blips EXIO1 (touch reset), acceptable for a manual one-shot diagnostic.
    // (The periodic keepalive in loop() deliberately does WR_IO only — see the note there.)
    ch422g_write(CH422G_WR_SET, 0x01);
    delay(20);
    ch422g_flush();
    s_ota_web.send(200, "application/json",
      String("{\"reasserted\":1,\"ioShadow\":") + String(s_io_shadow) + "}");
  });
  portal::begin(s_ota_web);   // /settings + /reboot ride the same server (see web_portal.h)
  if(sizeof(OTA_PASSWORD) > 1) ElegantOTA.setAuth("admin", OTA_PASSWORD);
  ElegantOTA.onStart([](){ Serial.println("[OTA] web upload start"); ota_splash_and_blank(); });
  ElegantOTA.onEnd([](bool ok){ Serial.printf("[OTA] web upload %s\n", ok ? "ok" : "FAILED"); if(!ok) ota_restore_display(); });
  ElegantOTA.begin(&s_ota_web);
  s_ota_web.begin();
}

// Bring up time + tokens + library once WiFi is available. Runs at boot when WiFi connects
// right away, and again from the retry timer if it only shows up later (router reboot,
// hotspot reconfiguration, transient outage).
static void net_init_and_fetch(){
  if(!s_net_ready){
    configTzTime(cfg::tz.c_str(), "pool.ntp.org", "time.nist.gov");   // screensaver clock
    if(cfg::op_mode != "core"){
      yoto::refresh();                        // mint a fresh access token before the first call
    }
    s_net_ready = true;
  }

  // Core mode registration handshake
  if(cfg::op_mode == "core" && cfg::core_url.length() > 0 && !core::s_is_registered){
    status("Registering with StoryDeck Core...");
    String err;
    if(!core::register_device(cfg::core_url, err)){
      Serial.printf("Registration note: %s (is_reg=%d is_appr=%d)\n", err.c_str(), (int)core::s_is_registered, (int)core::s_is_approved);
      if(!core::s_is_registered){
        show_error("Core Kiosk Registration",
                   err.c_str(),
                   "Verify the StoryDeck Core URL or network link in Settings.");
        return;
      }
    }
  }

  if(!s_lib_ok){
    if(cfg::op_mode == "core" && cfg::core_url.length() > 0){
      status("Loading StoryDeck Profiles...");
      if(core::fetch_config_and_profiles(cfg::core_url)){
        if(!cfg::lock_player && core::s_profiles[core::s_active_profile_idx].default_player_id.length() > 0){
          cfg::device_id = core::s_profiles[core::s_active_profile_idx].default_player_id;
        }
      }
    }

    status("Fetching library...");
    bool lib_fetched = false;
    if(cfg::op_mode == "core"){
      if(cfg::core_url.length() > 0){
        lib_fetched = fetch_library_core();
        if(!lib_fetched){
          show_error("Core Catalog Failed",
                     "Could not load media catalog from StoryDeck Core.",
                     "Check Core server health and network link in Settings.");
        }
      } else {
        show_error("Core URL Needed",
                   "StoryDeck Core Managed mode is active, but Core URL is empty.",
                   "Open Settings below or visit http://<ip>/settings to set Core URL.");
      }
    } else {
      lib_fetched = fetch_library();
    }

    if(lib_fetched){
      build_grid();
      load_thumbs();
      s_lib_ok = true;
      if(cfg::op_mode == "core" && core::s_profile_count > 1){
        open_profile_picker();
      } else {
        lv_scr_load(s_scr_home);
      }
    } else if(cfg::op_mode == "core" && core::s_profile_count > 0){
      // Device might be quarantined or catalog empty; still allow profile selection / grown-ups screen
      build_grid();
      if(core::s_profile_count > 1){
        open_profile_picker();
      } else {
        lv_scr_load(s_scr_home);
      }
    }
  }
  // Live device state over MQTT — start once the library is in (so event cardIds resolve to
  // grid tiles) and a token exists. begin() self-guards against a second start.
  if(s_lib_ok && cfg::op_mode != "core") ymqtt::begin(cfg::device_id);
  if(s_lib_ok && (WiFi.getMode() & WIFI_MODE_AP)){
    s_hotspot_wanted = false;
    WiFi.softAPdisconnect(true);            // drop the setup hotspot once we're properly online
    WiFi.mode(WIFI_STA);
  }
}
static void net_retry_cb(lv_timer_t* t){
  if(s_lib_ok){ lv_timer_del(t); return; }
  if(WiFi.status() == WL_CONNECTED) net_init_and_fetch();
}

void setup(){
  Serial.begin(115200); Serial.setTxTimeoutMs(0);
  // Route mbedTLS's big allocations (the two ~17KB SSL buffers per connection) to PSRAM.
  // Internal RAM fragments once the UI is up and can no longer host them — every runtime
  // HTTPS call died with "SSL - Memory allocation failed" (chapters, covers, commands).
  // Small hot allocations (bignum math during the handshake) stay in internal RAM for speed.
  mbedtls_platform_set_calloc_free(tls_psram_calloc, free);
  cfg::load();                              // NVS-backed settings (secrets.h = defaults)

  // Start WiFi early while internal SRAM is completely unfragmented
  WiFi.mode(WIFI_STA);
  {
    String hname = cfg::kiosk_name;
    hname.replace(" ", "-");
    if(hname.length() > 0) WiFi.setHostname(hname.c_str());
    else WiFi.setHostname("yoto-controller");
  }
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfg::wifi_ssid.c_str(), cfg::wifi_pass.c_str());

  // hardware: backlight + panel + touch
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000); delay(10);
  ch422g_write(CH422G_WR_SET, 0x01);
  s_io_shadow=(1u<<EXIO_BL_DISP)|(1u<<EXIO_TP_RST); ch422g_flush(); delay(20);
  ch422g_set(EXIO_LCD_RST,true); delay(120);
  rgb_panel_init();
  gt_init();

  // LVGL
  lv_init();
  size_t bufpx = LCD_H_RES*DRAW_LINES;
  s_buf1=(lv_color_t*)heap_caps_malloc(bufpx*sizeof(lv_color_t), MALLOC_CAP_INTERNAL);
  if(!s_buf1) s_buf1=(lv_color_t*)heap_caps_malloc(bufpx*sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  lv_disp_draw_buf_init(&s_draw_buf, s_buf1, NULL, bufpx);
  static lv_disp_drv_t dd; lv_disp_drv_init(&dd);
  dd.hor_res=LCD_H_RES; dd.ver_res=LCD_V_RES; dd.flush_cb=flush_cb; dd.draw_buf=&s_draw_buf;
  lv_disp_drv_register(&dd);
  static lv_indev_drv_t id; lv_indev_drv_init(&id);
  id.type=LV_INDEV_TYPE_POINTER; id.read_cb=touch_cb; lv_indev_drv_register(&id);
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0xFBF7F0), 0);

  // MicroSD (cover cache) — best-effort. A write/read self-test proves the wiring; on failure
  // the app still runs, covers just fetch live each boot as before.
  if(sd_begin()){
    uint32_t mb = (uint32_t)(SD.cardSize() / (1024ULL * 1024ULL));
    const char* type = SD.cardType()==CARD_SDHC ? "SDHC" : SD.cardType()==CARD_SD ? "SD" : SD.cardType()==CARD_MMC ? "MMC" : "?";
    bool rw = false;                                   // round-trip a marker file
    File f = SD.open(SD_DIR "/.selftest", FILE_WRITE);
    if(f){ rw = (f.print("ok") == 2); f.close(); SD.remove(SD_DIR "/.selftest"); }
    Serial.printf("SD: mounted %s %u MB, used %llu MB, RW=%s\n",
                  type, mb, SD.usedBytes()/(1024ULL*1024ULL), rw ? "yes" : "NO");
    status((String("SD ") + type + "  " + mb + " MB  " + (rw ? "(cache ready)" : "(read-only?)")).c_str());
  } else {
    Serial.println("SD: not mounted — covers will fetch live each boot");
    status("SD not found - covers load over WiFi");
  }
  delay(700);                                          // let the self-test line be visible

  // WiFi — never dead-end here: if we can't join, raise a setup hotspot carrying the web
  // portal and keep retrying STA in the background. OTA + portal start either way, so the
  // board is ALWAYS wirelessly reachable (the old code returned on failure, which made a
  // wrong WiFi password recoverable only over USB at the wall).
  status("Connecting to WiFi...");
  uint32_t t0=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-t0 < 20000){ delay(200); }

  if(cfg::op_mode != "core"){
    yoto::begin();                          // load tokens from NVS (standalone direct Yoto cloud)
  }
  lv_timer_create(tick_cb, 1000, NULL);     // 1 Hz heartbeat: progress/sleep timer/screensaver

  if(WiFi.status()==WL_CONNECTED){
    status((String("WiFi OK: ") + WiFi.localIP().toString() + "\nFetching library...").c_str());
    net_init_and_fetch();
    // (on fetch failure, status() already shows the reason; the retry timer keeps trying)
  } else {
    start_hotspot();
    WiFi.begin(cfg::wifi_ssid.c_str(), cfg::wifi_pass.c_str());
    show_error("WiFi Connection Needed",
               "Unable to join the configured WiFi network (2.4GHz only).\nSetup hotspot 'Yoto-Setup' is active.",
               "Join WiFi 'Yoto-Setup' on phone/PC or tap 'Open Settings' below.");
  }
  lv_timer_create(net_retry_cb, 30000, NULL);   // self-deletes once the library is in

  // Start OTA LAST: the TLS handshakes to Yoto (refresh/library/covers above) are
  // internal-RAM hungry, and mDNS + the web server compete for that same RAM. Bringing
  // them up after the heavy boot fetch keeps the handshake headroom intact. OTA still
  // starts here even if the library failed, so a bad build is always recoverable wirelessly.
  ota_begin();
}

void loop(){
  ArduinoOTA.handle();        // cheap polls when idle; block only during an actual update
  s_ota_web.handleClient();
  ElegantOTA.loop();
  lv_timer_handler();

  // Warm the library cache during idle: only on the home screen or screensaver, and only after
  // a short lull, so the ~1s blocking fetch lands when nobody's mid-tap. Runs after
  // lv_timer_handler so any pending async screen deletes are already settled. Throttled to one
  // fetch every ~2.5s so internal RAM (shared with the concurrent MQTT TLS session) recovers
  // between handshakes instead of being held at the knife's edge by back-to-back fetches.
  static uint32_t s_pc_t = 0;
  if(s_lib_ok && s_sd_ok && !s_pc_done && millis() - s_pc_t > 2500){
    lv_obj_t* act = lv_scr_act();
    // Safety valve: only start a warming fetch with comfortable internal-RAM headroom. A TLS
    // handshake transiently consumes most of free internal RAM; if MQTT (or anything) already has
    // it tight, skip this round and let it recover rather than stacking a fetch on top and dipping
    // to a near-zero low-water. At-rest free is ~24-28KB, so a 16KB floor passes normally but
    // backs off during contention. (Rechecked next loop; this defers warming, never aborts it.)
    bool ram_ok = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) > 16000;
    if(ram_ok && (act == s_scr_home || act == s_scr_saver) && lv_disp_get_inactive_time(NULL) > 1500){
      precache_step();
      s_pc_t = millis();
    }
  }
  // Backlight/DISP self-heal. A dip from the 12V supply can disturb the CH422G I2C expander —
  // EXIO2 (EXIO_BL_DISP) drives the backlight + panel DISP enable, EXIO1 (EXIO_TP_RST) the GT911
  // reset — while the higher-headroom ESP rides through and never reboots, so the screen goes
  // black with no reset. Two failure modes seen on-device:
  //   (1) output LATCH cleared  -> a bare WR_IO re-write (ch422g_flush) restores it. Touch-safe,
  //       so we do it every second while healthy.
  //   (2) full CHIP reset (output-enable config lost) -> WR_IO alone does nothing; the chip needs
  //       WR_SET re-issued. But WR_SET momentarily drops EVERY expander line (backlight blink +
  //       touch reset + panel reset), so it must NOT run on a blind timer — that was what killed
  //       touch when the keepalive re-issued it every second. Instead we DETECT it: a reset holds
  //       EXIO1 low, so the GT911 stops ACKing on I2C. On two consecutive missed probes we run the
  //       full re-init (WR_SET + shadow + gt_init for the just-reset touch chip). The RGB panel
  //       self-recovers once LCD_RST is driven high again (proven by the /bl endpoint).
  // NOTE: this recovers AFTER each brownout; it does not stop the rail from dipping. The real fix
  // is an adequate/regulated 12V supply (or USB-C 5V).
  static uint32_t s_bl_t = 0;
  static uint8_t  s_bl_fail = 0;
  if(millis() - s_bl_t > 1000){
    s_bl_t = millis();
    uint8_t id[4];
    if(gt_read(0x8140, id, 4)){          // GT911 reachable -> expander config intact
      s_bl_fail = 0;
      ch422g_flush();                    // refresh the output latch (covers a latch-only BL drop)
    } else if(++s_bl_fail >= 2){         // two consecutive misses -> the expander fully reset
      s_bl_fail = 0;
      ch422g_write(CH422G_WR_SET, 0x01); delay(20);                    // re-enable push-pull outputs
      s_io_shadow = (1u<<EXIO_BL_DISP)|(1u<<EXIO_TP_RST)|(1u<<EXIO_LCD_RST);
      ch422g_flush(); delay(20);                                       // backlight/DISP + resets high
      gt_init();                                                       // re-init the just-reset GT911
      s_bl_recoveries++;
    }
  }
  delay(5);
}
