#pragma once
// Catalog Fetching (Core Hub + Standalone Yoto API), Chapter Detail Loader,
// Cover Thumbnail Streaming, and Idle Background Cache Warmer
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "app_state.h"
#include "storage_sd.h"
#include "cover_decoder.h"
#include "yoto_api.h"
#include "core_api.h"

// Read the full body of an OPEN HTTPClient response into a NUL-terminated PSRAM buffer
// (caller frees). Returns nullptr on alloc failure.
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

// Add single card from Core catalog (preserving multiple series_list memberships for Kiosk parity)
static void add_core_card(const String& id, const String& title, const String& author, const String& cover, const String& src, const String& series, float seq, const String& seq_disp, bool is_hl, JsonArray series_list){
  int target_idx = s_card_count;
  if(target_idx >= MAX_CARDS){
    // Never drop a highlighted card even if catalog exceeds MAX_CARDS: replace last non-highlighted card
    if(!is_hl) return;
    int repl = -1;
    for(int k = MAX_CARDS - 1; k >= 0; k--){
      if(!s_cards[k].is_highlight){ repl = k; break; }
    }
    if(repl < 0) return;
    target_idx = repl;
  } else {
    s_card_count++;
  }
  Card& c = s_cards[target_idx];
  c.id               = id;
  c.title            = title;
  c.author           = author;
  c.cover            = cover;
  c.source           = src;
  c.series           = series;
  c.sequence         = seq;
  c.seq_display      = seq_disp;
  c.extra_series_cnt = 0;
  if(!series_list.isNull() && series_list.size() > 1){
    for(size_t si = 1; si < series_list.size() && c.extra_series_cnt < 2; si++){
      String sname = (const char*)(series_list[si]["name"] | "");
      sname.trim();
      if(sname.length() > 0 && !sname.equalsIgnoreCase(c.series)){
        int idx = c.extra_series_cnt++;
        c.extra_series[idx]   = sname;
        c.extra_seq[idx]      = series_list[si]["sequence"].isNull() ? seq : (float)series_list[si]["sequence"];
        c.extra_seq_disp[idx] = (const char*)(series_list[si]["sequence_display"] | seq_disp.c_str());
      }
    }
  }
  c.is_highlight = is_hl;
  c.daily        = false;
  c.tile         = nullptr;
  c.dsc.data     = nullptr;
}

static void sort_catalog_kiosk_order(){
  // Kiosk CatalogScreen.render() parity:
  // Sort highlighted items first, then alphabetically by title ignoring leading 'The', 'A', 'An'
  for(int a = 0; a < s_card_count; a++){
    for(int b = a + 1; b < s_card_count; b++){
      if(s_cards[a].daily) continue; // keep synthetic Yoto Daily at index 0 in standalone mode
      bool swap_it = false;
      if(s_cards[b].is_highlight != s_cards[a].is_highlight){
        swap_it = s_cards[b].is_highlight;
      } else {
        swap_it = (clean_sort_title(s_cards[b].title) < clean_sort_title(s_cards[a].title));
      }
      if(swap_it){
        Card t = s_cards[a]; s_cards[a] = s_cards[b]; s_cards[b] = t;
      }
    }
  }
}

bool fetch_library_core(){
  s_thumbs_loading = false;
  for(int i = 0; i < MAX_CARDS; i++){
    s_cards[i].tile = nullptr;
    s_cards[i].dsc.data = nullptr;
  }
  s_card_count = 0;
  String prof_id = "default";
  if(core::s_profile_count > 0 && core::s_active_profile_idx < core::s_profile_count){
    prof_id = core::s_profiles[core::s_active_profile_idx].id;
  }
  bool ok = core::fetch_catalog(cfg::core_url, prof_id, add_core_card);
  if(!ok || s_card_count == 0){
    core::fetch_catalog(cfg::core_url, "default", add_core_card);
  }
  sort_catalog_kiosk_order();
  if(s_card_count > 0 && (!core::s_allow_profile_switch || core::s_profile_count <= 1)){
    sd_prune_stale_covers(false);
  }
  sync_active_player_playback();
  return (s_card_count > 0);
}

static bool fetch_library(){
  size_t blen = 0;
  char* body = nullptr;
  {
    tlsarena::Guard tg;
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
  // inject the "Yoto Daily" tile at the front
  if(s_card_count < 64){
    for(int k=s_card_count; k>0; k--) s_cards[k] = s_cards[k-1];
    s_cards[0] = Card{};
    s_cards[0].id = YOTO_DAILY_ID; s_cards[0].title = "Yoto Daily";
    s_cards[0].cover = YOTO_DAILY_COVER; s_cards[0].daily = true;
    s_card_count++;
  }
  if(s_card_count > 0) sd_prune_stale_covers(false);
  return s_card_count > 0;
}

// Stream one cover thumbnail into a PSRAM buffer and blit it onto the grid tile.
static bool load_thumb(int i){
  Card& c = s_cards[i];
  if(!c.thumb){
    c.thumb = (uint8_t*)heap_caps_malloc(THUMB_SZ, MALLOC_CAP_SPIRAM);
    if(!c.thumb) return false;
  }
  if(!load_cover_cached(c.id, c.cover, THUMB_W, THUMB_H, THUMB_SZ, c.thumb)) return false;
  c.dsc.header.always_zero = 0;
  c.dsc.header.w = THUMB_W;
  c.dsc.header.h = THUMB_H;
  c.dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  c.dsc.data_size = THUMB_SZ;
  c.dsc.data = c.thumb;
  if(c.tile){
    lv_img_set_src(c.tile, &c.dsc);
    decorate_tile(c.tile, i, s_active_tab == TAB_SERIES);
  }
  return true;
}

// Non-blocking cover loader:
// 1. Immediately blits any covers already cached on MicroSD (fast ~10ms reads) so cached libraries appear instantly.
// 2. Defers any uncached HTTP cover fetches to thumb_load_step() called from loop() between lv_timer_handler() passes,
//    guaranteeing that all Catalog top-bar buttons (All, Series, Player Picker, Profile, Gear) and tiles are 100% responsive immediately!
void load_thumbs(){
  s_thumb_ok = 0;
  s_thumb_first_err = "";
  s_thumb_cursor = 0;

  // Immediate first-paint pass: load up to the first 8 visible tiles from MicroSD (~80ms)
  // so the initial viewport is populated immediately without triggering the ESP32 Watchdog Timer.
  if(s_sd_ok){
    int preload_limit = (s_card_count < 8) ? s_card_count : 8;
    for(int i = 0; i < preload_limit; i++){
      if(sd_cache_exists(s_cards[i].id, THUMB_W, s_cards[i].cover)){
        if(load_thumb(i)) s_thumb_ok++;
        delay(1);
      }
    }
    lv_refr_now(lv_disp_get_default());
  }

  if(s_thumb_ok >= s_card_count){
    s_thumbs_loading = false;
    if(s_barlabel) lv_label_set_text(s_barlabel, "Nothing playing  -  tap a book to listen");
  } else {
    s_thumbs_loading = true;
    if(s_barlabel) lv_label_set_text_fmt(s_barlabel, "loading covers  %d/%d", s_thumb_ok, s_card_count);
  }
}

inline void thumb_load_step(){
  if(!s_thumbs_loading) return;
  // Load up to 4 SD-cached covers per loop() tick, or 1 network cover per tick, yielding to FreeRTOS/WDT
  int steps = 0;
  while(steps < 4){
    while(s_thumb_cursor < s_card_count && s_cards[s_thumb_cursor].dsc.data != nullptr){
      s_thumb_cursor++;
    }
    if(s_thumb_cursor >= s_card_count){
      s_thumbs_loading = false;
      if(s_barlabel){
        if(s_thumb_ok < s_card_count && s_thumb_first_err.length() > 0)
          lv_label_set_text_fmt(s_barlabel, "covers %d/%d  -  %s", s_thumb_ok, s_card_count, s_thumb_first_err.c_str());
        else
          lv_label_set_text(s_barlabel, "Nothing playing  -  tap a book to listen");
      }
      return;
    }
    int i = s_thumb_cursor++;
    bool is_cached = s_sd_ok && sd_cache_exists(s_cards[i].id, THUMB_W, s_cards[i].cover);
    if(load_thumb(i)){
      s_thumb_ok++;
    } else if(s_thumb_first_err.length() == 0){
      s_thumb_first_err = s_thumb_diag;
    }
    steps++;
    if(!is_cached) break; // Only do 1 network fetch per loop() pass
    delay(1);
  }
  if(s_barlabel && s_thumbs_loading){
    lv_label_set_text_fmt(s_barlabel, "loading covers  %d/%d", s_thumb_cursor, s_card_count);
  }
}

static bool fetch_card_detail(int i){
  Card& c = s_cards[i];
  if(cfg::op_mode == "core" && cfg::core_url.length() > 0){
    s_ch_count = 0;
    String title_out = c.title;
    bool ok = core::fetch_media_tracks(cfg::core_url, c.id, title_out, [](const String& key, const String& trk_key, const String& tr_title, int dur){
      if(s_ch_count >= MAX_CH) return;
      s_ch[s_ch_count].key       = key;
      s_ch[s_ch_count].track_key = trk_key;
      s_ch[s_ch_count].title     = tr_title;
      s_ch[s_ch_count].dur       = dur;
      s_ch_count++;
    });
    if(ok && s_ch_count > 0){
      s_detail_title = title_out;
      sd_chap_write(c.id);
      return true;
    }
  }
  size_t blen = 0;
  char* body = nullptr;
  {
    tlsarena::Guard tg;
    WiFiClientSecure cli; HTTPClient http;
    int code = yoto::get(cli, http, "/card/" + c.id);
    if(code != 200){ if(code > 0) http.end(); return false; }
    body = read_body_psram(http, blen);
    http.end();
  }
  if(!body) return false;
  JsonDocument filter;
  filter["card"]["title"] = true;
  filter["card"]["content"]["chapters"][0]["key"]                = true;
  filter["card"]["content"]["chapters"][0]["title"]              = true;
  filter["card"]["content"]["chapters"][0]["duration"]           = true;
  filter["card"]["content"]["chapters"][0]["tracks"][0]["key"]   = true;
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
    String ck = (const char*)(ch["key"] | "");
    String tk = ck;
    JsonArray trks = ch["tracks"].as<JsonArray>();
    if(!trks.isNull() && trks.size() > 0){
      const char* t0 = trks[0]["key"];
      if(t0 && *t0) tk = t0;
    }
    s_ch[s_ch_count].key       = ck;
    s_ch[s_ch_count].track_key = tk;
    s_ch[s_ch_count].title     = (const char*)(ch["title"] | "");
    s_ch[s_ch_count].dur       = (int)(ch["duration"] | 0);
    s_ch_count++;
  }
  sd_chap_write(c.id);
  return true;
}

// Yoto Daily: dig into card 3nC80's "daily" chapter and pick the track for `today` (YYYYMMDD).
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
      if(k == today){
        outKey = k; outTitle = (const char*)(tr["title"] | "Today's episode");
        outDur = (int)(tr["duration"] | 0);
        return true;
      }
      if(k <= today && k > bestKey){
        bestKey = k; bestTitle = (const char*)(tr["title"] | "Today's episode");
        bestDur = (int)(tr["duration"] | 0);
      }
    }
    break;
  }
  if(bestKey.length()){ outKey = bestKey; outTitle = bestTitle; outDur = bestDur; return true; }
  return false;
}

// ---------------- background library warmer ----------------
static int  s_pc_cursor = 0;     // next card to inspect
static int  s_pc_fail   = 0;     // consecutive failures on the current item (bounded, then skip)
static bool s_pc_done   = false; // whole library warm — stop scanning until the next reboot
#define PC_MIN_INTERNAL_BLOCK 12288

static void precache_step(){
  if(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < PC_MIN_INTERNAL_BLOCK) return;
  while(s_pc_cursor < s_card_count){
    Card& c = s_cards[s_pc_cursor];
    if(c.daily){ s_pc_cursor++; s_pc_fail = 0; continue; }

    // 1) chapters -> {id}.chap
    if(!SD.exists(sd_chap_path(c.id))){
      static Chapter sv[MAX_CH];
      int svc = s_ch_count; String svt = s_detail_title;
      for(int k=0;k<svc;k++) sv[k] = s_ch[k];
      bool ok = fetch_card_detail(s_pc_cursor);
      for(int k=0;k<svc;k++){ s_ch[k] = sv[k]; sv[k].key = String(); sv[k].track_key = String(); sv[k].title = String(); }
      s_ch_count = svc; s_detail_title = svt;
      if(!ok && ++s_pc_fail >= 3){ s_pc_cursor++; s_pc_fail = 0; }
      return;
    }
    // 2) detail-size cover -> {id}_210.565
    if(!sd_cache_exists(c.id, DCOVER_W, c.cover)){
      if(!s_dcover) s_dcover = (uint8_t*)heap_caps_malloc(DCOVER_SZ, MALLOC_CAP_SPIRAM);
      bool ok = s_dcover && load_cover_cached(c.id, c.cover, DCOVER_W, DCOVER_H, DCOVER_SZ, s_dcover);
      if(!ok && ++s_pc_fail >= 3){ s_pc_cursor++; s_pc_fail = 0; }
      return;
    }
    // 3) now-playing cover -> {id}_170.565
    if(!sd_cache_exists(c.id, NCOVER_W, c.cover)){
      if(!s_dcover) s_dcover = (uint8_t*)heap_caps_malloc(DCOVER_SZ, MALLOC_CAP_SPIRAM);
      bool ok = s_dcover && load_cover_cached(c.id, c.cover, NCOVER_W, NCOVER_H, NCOVER_SZ, s_dcover);
      if(!ok && ++s_pc_fail >= 3){ s_pc_cursor++; s_pc_fail = 0; }
      return;
    }
    s_pc_cursor++; s_pc_fail = 0;
  }
  s_pc_done = true;
}
