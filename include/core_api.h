#pragma once
// StoryDeck Core Client for ESP32
// Interacts with StoryDeck Core hub (Docker container, default port 5055/5060)
// providing device registration, child profiles, and merged catalog.
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "app_config.h"
#include "app_log.h"

inline int sd_prune_stale_avatars(const String* valid_keys, int key_count);

namespace core {

struct PlayerInfo {
  String id;
  String name;
  bool   online        = false;
  bool   is_home       = true;
  bool   is_playing    = false;
  bool   has_card      = false;
  String item_id       = "";
  String now_playing   = "";
  String author        = "";
  String chapter_title = "";
  int    position      = 0;
  int    duration      = 0;
  int    volume        = 50;
  int    volume_max    = 100;
};

#define MAX_PROF_PLAYERS 8

struct Profile {
  String id;
  String display_name;
  String avatar_url;
  bool   has_custom_avatar = false;
  String avatar_cache_key;
  uint32_t avatar_color = 0xE5352B;
  String avatar_initial;
  String default_player_id;
  String default_player_name;
  PlayerInfo players[MAX_PROF_PLAYERS];
  int    player_count = 0;
  uint8_t* avatar_buf = nullptr;
  lv_img_dsc_t avatar_dsc{};
};

// Populated from POST /api/play response when resuming a book
static int    s_last_play_resp_ch = -1;
static String s_last_play_resp_ch_title = "";
static int    s_last_play_resp_pos = 0;

#define MAX_PROFILES 8
static Profile s_profiles[MAX_PROFILES];
static int     s_profile_count = 0;
static int     s_active_profile_idx = 0;
static String  s_assigned_profile = "";
static bool    s_allow_profile_switch = true;
static bool    s_exclude_single_series = true;

// Standalone mode discovered players (from /device-v2/devices/mine)
static PlayerInfo s_standalone_players[MAX_PROF_PLAYERS];
static int        s_standalone_player_count = 0;

static String s_access_token = "";
static bool   s_is_registered = false;
static bool   s_is_approved = false;

inline String get_mac(){
  return WiFi.macAddress();
}

// Helper to correctly begin HTTP or HTTPS requests without panicking or requiring CA certs
inline bool http_begin_url(HTTPClient& http, WiFiClientSecure& scli, WiFiClient& pcli, const String& url){
  if(url.startsWith("https://")){
    scli.setInsecure();
    return http.begin(scli, url);
  }
  return http.begin(pcli, url);
}

// 1. Health Probe
inline bool ping(const String& base_url){
  if(base_url.length() == 0) return false;
  WiFiClientSecure scli; WiFiClient pcli; HTTPClient http;
  if(!http_begin_url(http, scli, pcli, base_url + "/health")) return false;
  http.setTimeout(4000);
  int code = http.GET();
  http.end();
  return (code == 200);
}

// ---- PSRAM-backed response handling ----
// getString() + a default JsonDocument allocate from INTERNAL RAM. Core's config/catalog
// bodies are tens of KB; together with the LVGL draw buffer they exhaust internal heap and
// the WiFi PHY's tiny esp_timer_create() (modem-sleep wake) aborts with ESP_ERR_NO_MEM.
struct PsramAlloc : ArduinoJson::Allocator {
  void* allocate(size_t n) override           { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
  void  deallocate(void* p) override          { heap_caps_free(p); }
  void* reallocate(void* p, size_t n) override{ return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};

// Read an OPEN response fully into a NUL-terminated PSRAM buffer (caller free()s).
inline char* read_body(HTTPClient& http, size_t& outLen){
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

// 2. Kiosk Device Registration
inline bool register_device(const String& base_url, String& err_out){
  if(base_url.length() == 0){
    err_out = "Core URL not set";
    return false;
  }
  WiFiClientSecure scli; WiFiClient pcli; HTTPClient http;
  if(!http_begin_url(http, scli, pcli, base_url + "/api/devices/register")){
    err_out = "Failed to initialize connection to Core";
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(1200);

  JsonDocument doc;
  doc["mac"]              = get_mac();
  doc["mac_address"]      = get_mac();
  doc["friendly_name"]    = cfg::kiosk_name.length() > 0 ? cfg::kiosk_name : "StoryDeck Touch (ESP32-S3)";
  doc["ip_address"]       = WiFi.localIP().toString();
  doc["firmware_version"] = STORYDECK_VERSION;

  String payload;
  serializeJson(doc, payload);

  int code = http.POST(payload);
  String resp = (code > 0) ? http.getString() : "";
  http.end();

  if(code == 202){
    s_is_registered = true;
    s_is_approved = false;
    err_out = "Pending Admin Approval in StoryDeck Core";
    return false;
  }
  if(code == 200 || code == 201){
    JsonDocument rdoc;
    if(!deserializeJson(rdoc, resp)){
      const char* tok = rdoc["tokens"]["access_token"];
      if(tok && *tok){
        s_access_token = tok;
      }
    }
    s_is_registered = true;
    s_is_approved = true;
    return true;
  }

  err_out = String("Registration failed (HTTP ") + code + ")";
  return false;
}

// 3. Fetch Device Config & Child Profiles
inline bool fetch_config_and_profiles(const String& base_url, bool prune_avatars = false){
  if(base_url.length() == 0) return false;
  WiFiClientSecure scli; WiFiClient pcli; HTTPClient http;
  if(!http_begin_url(http, scli, pcli, base_url + "/api/device/config?device_id=" + get_mac())) return false;
  if(s_access_token.length() > 0){
    http.addHeader("Authorization", String("Bearer ") + s_access_token);
  }
  http.setTimeout(1200);
  int code = http.GET();
  if(code != 200){ http.end(); return false; }

  size_t blen = 0; char* body = read_body(http, blen);
  http.end();
  if(!body) return false;
  PsramAlloc palloc; JsonDocument doc(&palloc);
  DeserializationError jerr = deserializeJson(doc, (const char*)body, blen);
  free(body);
  if(jerr) return false;

  bool cfg_changed = false;
  const char* pin = doc["parent_pin"];
  if(pin && strlen(pin) == 4 && cfg::parent_pin != pin){
    cfg::parent_pin = pin;
    cfg_changed = true;
  }
  const char* lpid = doc["locked_player_id"];
  if(lpid && *lpid && cfg::locked_dev_id != lpid){
    cfg::locked_dev_id = lpid;
    cfg::device_id = lpid;
    cfg::lock_player = true;
    cfg_changed = true;
  } else if(!doc["lock_player"].isNull()){
    bool lp = (bool)doc["lock_player"];
    if(cfg::lock_player != lp){ cfg::lock_player = lp; cfg_changed = true; }
  }
  if(!doc["settings"].isNull() && doc["settings"].is<JsonObject>()){
    JsonObject st = doc["settings"].as<JsonObject>();
    if(!st["sleepTimerMin"].isNull()){
      int sm = (int)st["sleepTimerMin"];
      if(cfg::sleep_min != sm){ cfg::sleep_min = sm; cfg_changed = true; }
    }
    if(!st["sleepTimerMode"].isNull()){
      bool sb = (String((const char*)st["sleepTimerMode"]) != "always");
      if(cfg::sleep_bedtime != sb){ cfg::sleep_bedtime = sb; cfg_changed = true; }
    }
    if(!st["screensaverMin"].isNull()){
      int smin = (int)st["screensaverMin"];
      if(cfg::saver_min != smin){ cfg::saver_min = smin; cfg_changed = true; }
    }
    if(!st["clockSize"].isNull()){
      int cs = (int)st["clockSize"];
      if(cfg::saver_size != cs){ cfg::saver_size = cs; cfg_changed = true; }
    }
    if(!st["blockRemotePlayback"].isNull()){
      bool br = (bool)st["blockRemotePlayback"];
      if(cfg::block_remote != br){ cfg::block_remote = br; cfg_changed = true; }
    }
    if(!st["bedtime"].isNull()){
      String bt = (const char*)st["bedtime"];
      int colon = bt.indexOf(':');
      if(colon > 0){
        int nh = bt.substring(0, colon).toInt();
        int nm = bt.substring(colon + 1).toInt();
        if(cfg::night_h != nh || cfg::night_m != nm){
          cfg::night_h = nh; cfg::night_m = nm; cfg_changed = true;
        }
      }
    }
    if(!st["wakeTime"].isNull()){
      String wt = (const char*)st["wakeTime"];
      int colon = wt.indexOf(':');
      if(colon > 0){
        int wh = wt.substring(0, colon).toInt();
        int wm = wt.substring(colon + 1).toInt();
        if(cfg::wake_h != wh || cfg::wake_m != wm){
          cfg::wake_h = wh; cfg::wake_m = wm; cfg_changed = true;
        }
      }
    }
  }
  if(cfg_changed) cfg::save();

  s_assigned_profile = (const char*)(doc["assigned_profile"] | "");
  if(s_assigned_profile.length() > 0 && cfg::assigned_profile != s_assigned_profile){
    cfg::assigned_profile = s_assigned_profile;
    cfg_changed = true;
  } else if(s_assigned_profile.length() == 0 && cfg::assigned_profile.length() > 0){
    s_assigned_profile = cfg::assigned_profile;
  }
  if(!doc["allow_profile_switch"].isNull()){
    s_allow_profile_switch = (bool)doc["allow_profile_switch"];
  } else {
    s_allow_profile_switch = (s_assigned_profile.length() == 0);
  }
  if(!doc["exclude_single_card_series"].isNull()){
    s_exclude_single_series = (bool)doc["exclude_single_card_series"];
  } else {
    s_exclude_single_series = true;
  }

  s_profile_count = 0;
  JsonArray profs = doc["profiles"].as<JsonArray>();
  for(JsonObject p : profs){
    if(s_profile_count >= MAX_PROFILES) break;
    Profile& pr = s_profiles[s_profile_count];
    pr.id = (const char*)(p["id"] | "");
    pr.display_name = (const char*)(p["display_name"] | "Child");
    String av = (const char*)(p["avatar_url"] | "");
    if(av.startsWith("/")){
      av = base_url + av;
    }
    if(pr.avatar_url != av && pr.avatar_buf){
      free(pr.avatar_buf);
      pr.avatar_buf = nullptr;
    }
    pr.avatar_url = av;
    pr.has_custom_avatar = !p["has_custom_avatar"].isNull()
                           ? (bool)p["has_custom_avatar"]
                           : ((av.indexOf("?t=") >= 0) || (av.length() > 0 && av.indexOf("/api/avatar/") < 0));
    int t_pos = av.indexOf("?t=");
    String ver_tag = (t_pos >= 0) ? av.substring(t_pos + 3) : String("1");
    int amp_pos = ver_tag.indexOf('&');
    if(amp_pos >= 0) ver_tag = ver_tag.substring(0, amp_pos);
    if(ver_tag.length() > 6) ver_tag = ver_tag.substring(ver_tag.length() - 6);
    pr.avatar_cache_key = String("av_") + pr.id + "_" + ver_tag;
    String col_str = (const char*)(p["avatar_color"] | "");
    if(col_str.startsWith("#") && col_str.length() == 7){
      pr.avatar_color = (uint32_t)strtoul(col_str.c_str() + 1, nullptr, 16);
    } else {
      static const uint32_t PALETTE[10] = {0x3B82F6, 0xEF4444, 0x10B981, 0xF59E0B, 0x8B5CF6, 0xEC4899, 0x06B6D4, 0xF97316, 0x14B8A6, 0x6366F1};
      uint32_t h = 0;
      String key = pr.display_name; key.toLowerCase();
      for(size_t ci = 0; ci < key.length(); ci++) h = (h * 31 + (uint8_t)key[ci]);
      pr.avatar_color = PALETTE[h % 10];
    }
    pr.avatar_initial = (const char*)(p["avatar_initial"] | "");
    if(pr.avatar_initial.length() == 0){
      pr.avatar_initial = pr.display_name.length() > 0 ? pr.display_name.substring(0, 1) : String("C");
      pr.avatar_initial.toUpperCase();
    }
    pr.default_player_id = (const char*)(p["default_player_id"] | "");
    pr.default_player_name = (const char*)(p["default_player_name"] | "Yoto Player");

    pr.player_count = 0;
    JsonArray pls = p["players"].as<JsonArray>();
    for(JsonObject pl : pls){
      if(pr.player_count >= MAX_PROF_PLAYERS) break;
      String pid = (const char*)(pl["id"] | "");
      if(pid.length() == 0) continue;
      PlayerInfo& pi = pr.players[pr.player_count++];
      pi.id         = pid;
      pi.name       = (const char*)(pl["name"] | "Yoto Player");
      pi.online        = (bool)(pl["online"] | false);
      pi.is_home       = pl["is_home"].isNull() ? pi.online : (bool)pl["is_home"];
      pi.is_playing    = (bool)(pl["is_playing"] | false);
      pi.has_card      = (bool)(pl["has_card"] | pi.is_playing);
      pi.item_id       = (const char*)(pl["item_id"] | "");
      pi.now_playing   = (const char*)(pl["now_playing"] | "");
      pi.author        = (const char*)(pl["author"] | "");
      pi.chapter_title = (const char*)(pl["chapter_title"] | "");
      pi.position      = (int)(pl["position"] | 0);
      pi.duration      = (int)(pl["duration"] | 0);
      pi.volume        = (int)(pl["volume"] | 50);
      pi.volume_max    = (int)(pl["volume_max"] | 100);
    }
    if(pr.player_count == 0 && pr.default_player_id.length() > 0){
      PlayerInfo& pi = pr.players[pr.player_count++];
      pi.id   = pr.default_player_id;
      pi.name = pr.default_player_name;
    }
    if(pr.default_player_id.length() == 0 && pr.player_count > 0){
      pr.default_player_id   = pr.players[0].id;
      pr.default_player_name = pr.players[0].name;
    }
    s_profile_count++;
  }

  // Prune any superseded or deleted profile avatar .565 files from MicroSD (only on explicit boot/maintenance)
  if(prune_avatars && s_profile_count > 0){
    String valid_av_keys[MAX_PROFILES];
    for(int i = 0; i < s_profile_count; i++) valid_av_keys[i] = s_profiles[i].avatar_cache_key;
    sd_prune_stale_avatars(valid_av_keys, s_profile_count);
  }

  // If Core assigned a specific profile to this hardware controller, select it
  if(s_assigned_profile.length() > 0){
    for(int i = 0; i < s_profile_count; i++){
      if(s_profiles[i].id == s_assigned_profile){
        s_active_profile_idx = i;
        break;
      }
    }
  }
  return (s_profile_count > 0);
}

// Kiosk parity: resolve best active player for a profile (or standalone)
inline String resolve_active_device_id(int prof_idx){
  if(cfg::locked_dev_id.length() > 0){
    return cfg::locked_dev_id;
  }
  if(cfg::device_id.length() > 0 && cfg::device_id != "unknown"){
    return cfg::device_id;
  }
  if(cfg::op_mode != "core"){
    if(s_standalone_player_count > 0){
      for(int i = 0; i < s_standalone_player_count; i++){
        if(s_standalone_players[i].id == cfg::device_id) return cfg::device_id;
      }
      for(int i = 0; i < s_standalone_player_count; i++){
        if(s_standalone_players[i].online) return s_standalone_players[i].id;
      }
      return s_standalone_players[0].id;
    }
    return cfg::device_id;
  }
  if(prof_idx < 0 || prof_idx >= s_profile_count) return cfg::device_id;
  const Profile& pr = s_profiles[prof_idx];
  for(int i = 0; i < pr.player_count; i++){
    if(pr.players[i].is_playing) return pr.players[i].id;
  }
  for(int i = 0; i < pr.player_count; i++){
    if(pr.players[i].online) return pr.players[i].id;
  }
  if(pr.default_player_id.length() > 0) return pr.default_player_id;
  if(pr.player_count > 0) return pr.players[0].id;
  for(int p = 0; p < s_profile_count; p++){
    if(s_profiles[p].default_player_id.length() > 0) return s_profiles[p].default_player_id;
    if(s_profiles[p].player_count > 0) return s_profiles[p].players[0].id;
  }
  return cfg::device_id;
}

// Lookup PlayerInfo by device_id across active profile / all profiles / standalone
inline const PlayerInfo* find_player_info(const String& dev_id){
  if(dev_id.length() == 0) return nullptr;
  if(cfg::op_mode == "core"){
    if(s_active_profile_idx >= 0 && s_active_profile_idx < s_profile_count){
      const Profile& pr = s_profiles[s_active_profile_idx];
      for(int i = 0; i < pr.player_count; i++){
        if(pr.players[i].id == dev_id) return &pr.players[i];
      }
    }
    for(int p = 0; p < s_profile_count; p++){
      for(int i = 0; i < s_profiles[p].player_count; i++){
        if(s_profiles[p].players[i].id == dev_id) return &s_profiles[p].players[i];
      }
    }
  } else {
    for(int i = 0; i < s_standalone_player_count; i++){
      if(s_standalone_players[i].id == dev_id) return &s_standalone_players[i];
    }
  }
  return nullptr;
}


// 3b. Push Grown-Ups Settings & PIN back to StoryDeck Core (/api/device/settings)
inline bool sync_device_settings(const String& base_url){
  if(base_url.length() == 0 || cfg::op_mode != "core") return false;
  WiFiClientSecure scli; WiFiClient pcli; HTTPClient http;
  if(!http_begin_url(http, scli, pcli, base_url + "/api/device/settings")) return false;
  http.addHeader("Content-Type", "application/json");
  if(s_access_token.length() > 0){
    http.addHeader("Authorization", String("Bearer ") + s_access_token);
  }
  http.setTimeout(5000);

  JsonDocument doc;
  doc["device_id"]        = get_mac();
  doc["friendly_name"]    = cfg::kiosk_name;
  doc["parent_pin"]       = cfg::parent_pin;
  doc["lock_player"]      = cfg::lock_player;
  doc["locked_player_id"] = cfg::locked_dev_id;
  doc["assigned_profile"] = cfg::assigned_profile;

  JsonObject st = doc["settings"].to<JsonObject>();
  st["sleepTimerMin"]       = cfg::sleep_min;
  st["sleepTimerMode"]      = cfg::sleep_bedtime ? "bedtime" : "always";
  st["screensaverMin"]      = cfg::saver_min;
  st["clockSize"]           = cfg::saver_size;
  st["blockRemotePlayback"] = cfg::block_remote;
  char bbuf[8], wbuf[8];
  snprintf(bbuf, sizeof(bbuf), "%02d:%02d", cfg::night_h, cfg::night_m);
  snprintf(wbuf, sizeof(wbuf), "%02d:%02d", cfg::wake_h, cfg::wake_m);
  st["bedtime"]  = bbuf;
  st["wakeTime"] = wbuf;

  String payload;
  serializeJson(doc, payload);
  int code = http.PUT(payload);
  http.end();
  return (code == 200);
}

// 4. Playback Dispatch via Core Hub (with automatic 401/403 JWT re-auth + diagnostic logging)
inline int play_media(const String& base_url, const String& child_id, const String& card_type, const String& media_id, const String& device_id, int chapter_idx, const String& chapter_key = "", const String& track_key = ""){
  if(base_url.length() == 0) return -1;

  for(int attempt = 0; attempt < 2; attempt++){
    if(s_access_token.length() == 0 || attempt > 0){
      String rerr;
      register_device(base_url, rerr);
    }
    WiFiClientSecure scli; WiFiClient pcli; HTTPClient http;
    if(!http_begin_url(http, scli, pcli, base_url + "/api/play")) return -1;
    http.addHeader("Content-Type", "application/json");
    if(s_access_token.length() > 0){
      http.addHeader("Authorization", String("Bearer ") + s_access_token);
    }
    http.setTimeout(10000);

    String eff_dev = device_id;
    if(eff_dev.length() == 0) eff_dev = resolve_active_device_id(s_active_profile_idx);

    // Only pass explicit chapter_key / track_key if they are non-numeric hash/special keys
    // (e.g. podcasts or 'daily'). For normal numeric chapter keys ('1', '01', '2', '02'),
    // omit chapter_key so Core and Bridge resolve the card's true chapterKey/trackKey or zero-pad '%02d',
    // matching Kiosk BrokerApi.playMedia() 100%!
    auto is_pure_digits = [](const String& s) -> bool {
      if(s.length() == 0) return true;
      for(size_t i = 0; i < s.length(); i++){
        if(s[i] < '0' || s[i] > '9') return false;
      }
      return true;
    };

    s_last_play_resp_ch = -1;
    s_last_play_resp_ch_title = "";
    s_last_play_resp_pos = 0;

    JsonDocument doc;
    doc["child"]     = child_id.length() ? child_id : "default";
    doc["type"]      = card_type.length() ? card_type : "yoto";
    doc["id"]        = media_id;
    doc["device_id"] = eff_dev;
    if(chapter_idx >= 0){
      doc["chapter"] = chapter_idx + 1; // 1-based chapter when explicitly selected; omitted when resuming book!
      if(chapter_key.length() > 0 && !is_pure_digits(chapter_key)) doc["chapter_key"] = chapter_key;
      if(track_key.length() > 0   && !is_pure_digits(track_key))   doc["track_key"]   = track_key;
    }

    String payload;
    serializeJson(doc, payload);

    int code = http.POST(payload);
    String resp = (code > 0) ? http.getString() : "";
    http.end();
    log_msg("[PLAY] POST /api/play att=%d code=%d pl=%s resp=%s", attempt + 1, code, payload.c_str(), resp.c_str());

    if((code == 401 || code == 403) && attempt == 0){
      continue; // re-authenticate once and retry
    }
    if(code >= 200 && code < 300 && resp.length() > 0){
      JsonDocument rdoc;
      if(!deserializeJson(rdoc, resp)){
        if(!rdoc["chapter"].isNull()){
          int rch = (int)rdoc["chapter"] - 1;
          if(rch >= 0) s_last_play_resp_ch = rch;
        }
        const char* rct = rdoc["chapter_title"];
        if(rct && *rct) s_last_play_resp_ch_title = rct;
        if(!rdoc["position"].isNull()) s_last_play_resp_pos = (int)rdoc["position"];
      }
    }
    return code;
  }
  return -1;
}

// 5. Transport Control (pause, resume, stop, next, prev, volume)
inline int player_control(const String& base_url, const String& device_id, const char* action, int volume = -1){
  if(base_url.length() == 0) return -1;

  for(int attempt = 0; attempt < 2; attempt++){
    if(s_access_token.length() == 0 || attempt > 0){
      String rerr;
      register_device(base_url, rerr);
    }
    WiFiClientSecure scli; WiFiClient pcli; HTTPClient http;
    if(!http_begin_url(http, scli, pcli, base_url + "/api/player/control")) return -1;
    http.addHeader("Content-Type", "application/json");
    if(s_access_token.length() > 0){
      http.addHeader("Authorization", String("Bearer ") + s_access_token);
    }
    http.setTimeout(2500);

    String eff_dev = device_id;
    if(eff_dev.length() == 0) eff_dev = resolve_active_device_id(s_active_profile_idx);

    JsonDocument doc;
    doc["device_id"] = eff_dev;
    doc["action"]    = action;
    if(volume >= 0) doc["volume"] = volume;

    String payload;
    serializeJson(doc, payload);

    int code = http.POST(payload);
    String resp = (code > 0) ? http.getString() : "";
    http.end();
    log_msg("[CTRL] POST /api/player/control att=%d code=%d pl=%s resp=%s", attempt + 1, code, payload.c_str(), resp.c_str());

    if((code == 401 || code == 403) && attempt == 0){
      continue;
    }
    return code;
  }
  return -1;
}

// 6. Fetch Child Catalog (ABS + Yoto merged via Core hub)
inline bool fetch_catalog(const String& base_url, const String& child_id,
  void (*card_cb)(const String& id, const String& title, const String& author, const String& cover, const String& src, const String& series, float seq, const String& seq_disp, bool is_hl, JsonArray series_list)){
  if(base_url.length() == 0 || child_id.length() == 0) return false;

  auto fetch_src = [&](const String& path, const String& src_name){
    WiFiClientSecure scli; WiFiClient pcli; HTTPClient http;
    if(!http_begin_url(http, scli, pcli, base_url + path)) return;
    if(s_access_token.length() > 0){
      http.addHeader("Authorization", String("Bearer ") + s_access_token);
    }
    http.setTimeout(9000);
    int code = http.GET();
    if(code != 200){ http.end(); return; }
    size_t blen = 0; char* body = read_body(http, blen);
    http.end();
    if(!body) return;
    JsonDocument filter;
    JsonObject f0 = filter.add<JsonObject>();
    f0["id"] = true;
    f0["card_id"] = true;
    f0["title"] = true;
    f0["author"] = true;
    f0["cover"] = true;
    f0["thumb"] = true;
    f0["cover_url"] = true;
    f0["source"] = true;
    f0["type"] = true;
    f0["series"] = true;
    f0["sequence"] = true;
    f0["sequence_display"] = true;
    f0["is_highlight"] = true;
    f0["series_list"][0]["name"] = true;
    f0["series_list"][0]["sequence"] = true;
    f0["series_list"][0]["sequence_display"] = true;
    PsramAlloc palloc; JsonDocument doc(&palloc);
    DeserializationError jerr = deserializeJson(doc, (const char*)body, blen, DeserializationOption::Filter(filter));
    free(body);
    if(jerr) return;
    for(JsonObject item : doc.as<JsonArray>()){
      String id = (const char*)(item["id"] | item["card_id"] | "");
      if(id.length() == 0) continue;
      String title = (const char*)(item["title"] | "");
      String cover = (const char*)(item["cover"] | item["thumb"] | item["cover_url"] | "");
      if(cover.length() == 0){
        cover = base_url + "/api/thumb?type=" + src_name + "&id=" + id;
      } else if(cover.startsWith("/")){
        cover = base_url + cover;
      }
      String author = (const char*)(item["author"] | "");
      String series = (const char*)(item["series"] | "");
      JsonArray sl = item["series_list"].as<JsonArray>();
      if(series.length() == 0 && !sl.isNull() && sl.size() > 0){
        series = (const char*)(sl[0]["name"] | "");
      }
      auto parse_lead_float = [](const String& s) -> float {
        for(size_t i = 0; i < s.length(); i++){
          if(s[i] >= '0' && s[i] <= '9'){
            return s.substring(i).toFloat();
          }
        }
        return -1.0f;
      };
      float seq = item["sequence"].isNull() ? -1.0f : (float)item["sequence"];
      String seq_disp = (const char*)(item["sequence_display"] | "");
      if(seq < 0.0f && seq_disp.length() > 0) seq = parse_lead_float(seq_disp);
      if(seq < 0.0f && !sl.isNull() && sl.size() > 0){
        if(!sl[0]["sequence"].isNull()) seq = (float)sl[0]["sequence"];
        if(seq_disp.length() == 0) seq_disp = (const char*)(sl[0]["sequence_display"] | "");
        if(seq < 0.0f && seq_disp.length() > 0) seq = parse_lead_float(seq_disp);
      }
      bool is_hl = (bool)(item["is_highlight"] | false);
      String item_src = (const char*)(item["source"] | item["type"] | src_name.c_str());
      if(item_src.startsWith("yoto")) item_src = "yoto";
      else if(item_src == "abs") item_src = "abs";
      else item_src = src_name;
      card_cb(id, title, author, cover, item_src, series, seq, seq_disp, is_hl, sl);
    }
  };

  fetch_src("/api/kids/" + child_id + "/abs", "abs");
  fetch_src("/api/kids/" + child_id + "/yoto", "yoto");
  return true;
}

// 7. Fetch Media Tracks / Chapters via Core Hub
inline bool fetch_media_tracks(const String& base_url, const String& media_id, String& title_out, void (*track_cb)(const String& key, const String& trk_key, const String& title, int dur)){
  if(base_url.length() == 0 || media_id.length() == 0) return false;
  WiFiClientSecure scli; WiFiClient pcli; HTTPClient http;
  if(!http_begin_url(http, scli, pcli, base_url + "/api/media/" + media_id + "/tracks")) return false;
  if(s_access_token.length() > 0){
    http.addHeader("Authorization", String("Bearer ") + s_access_token);
  }
  http.setTimeout(9000);
  int code = http.GET();
  if(code != 200){ http.end(); return false; }
  size_t blen = 0; char* body = read_body(http, blen);
  http.end();
  if(!body) return false;
  PsramAlloc palloc; JsonDocument doc(&palloc);
  DeserializationError jerr = deserializeJson(doc, (const char*)body, blen);
  free(body);
  if(jerr) return false;
  const char* t = doc["title"];
  if(t && *t) title_out = t;
  JsonArray tracks = doc["tracks"].as<JsonArray>();
  int idx = 1;
  for(JsonObject tr : tracks){
    String key = (const char*)(tr["key"] | "");
    if(key.length() == 0){
      char kbuf[8];
      snprintf(kbuf, sizeof(kbuf), "%02d", idx);
      key = kbuf;
    }
    String trk_key = (const char*)(tr["track_key"] | key.c_str());
    String tr_title = (const char*)(tr["title"] | "");
    if(tr_title.length() == 0) tr_title = String("Chapter ") + idx;
    int dur = (int)(tr["duration_seconds"] | tr["duration"] | 0);
    track_cb(key, trk_key, tr_title, dur);
    idx++;
  }
  return true;
}

} // namespace core
