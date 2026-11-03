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

namespace core {

struct PlayerInfo {
  String id;
  String name;
  bool   online     = false;
  bool   is_home    = true;
  bool   is_playing = false;
  int    volume     = 50;
  int    volume_max = 100;
};

#define MAX_PROF_PLAYERS 8

struct Profile {
  String id;
  String display_name;
  String avatar_url;
  String default_player_id;
  String default_player_name;
  PlayerInfo players[MAX_PROF_PLAYERS];
  int    player_count = 0;
  uint8_t* avatar_buf = nullptr;
  lv_img_dsc_t avatar_dsc{};
};

#define MAX_PROFILES 8
static Profile s_profiles[MAX_PROFILES];
static int     s_profile_count = 0;
static int     s_active_profile_idx = 0;
static String  s_assigned_profile = "";
static bool    s_allow_profile_switch = true;

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
  http.setTimeout(6000);

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
inline bool fetch_config_and_profiles(const String& base_url){
  if(base_url.length() == 0) return false;
  WiFiClientSecure scli; WiFiClient pcli; HTTPClient http;
  if(!http_begin_url(http, scli, pcli, base_url + "/api/device/config?device_id=" + get_mac())) return false;
  if(s_access_token.length() > 0){
    http.addHeader("Authorization", String("Bearer ") + s_access_token);
  }
  http.setTimeout(6000);
  int code = http.GET();
  if(code != 200){ http.end(); return false; }

  size_t blen = 0; char* body = read_body(http, blen);
  http.end();
  if(!body) return false;
  PsramAlloc palloc; JsonDocument doc(&palloc);
  DeserializationError jerr = deserializeJson(doc, (const char*)body, blen);
  free(body);
  if(jerr) return false;

  const char* pin = doc["parent_pin"];
  if(pin && strlen(pin) == 4){
    cfg::parent_pin = pin;
    cfg::save();
  }

  s_assigned_profile = (const char*)(doc["assigned_profile"] | "");
  if(!doc["allow_profile_switch"].isNull()){
    s_allow_profile_switch = (bool)doc["allow_profile_switch"];
  } else {
    s_allow_profile_switch = (s_assigned_profile.length() == 0);
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
    pr.avatar_url = av;
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
      pi.online     = (bool)(pl["online"] | false);
      pi.is_home    = pl["is_home"].isNull() ? pi.online : (bool)pl["is_home"];
      pi.is_playing = (bool)(pl["is_playing"] | false);
      pi.volume     = (int)(pl["volume"] | 50);
      pi.volume_max = (int)(pl["volume_max"] | 100);
    }
    if(pr.player_count == 0 && pr.default_player_id.length() > 0){
      PlayerInfo& pi = pr.players[pr.player_count++];
      pi.id   = pr.default_player_id;
      pi.name = pr.default_player_name;
    }
    s_profile_count++;
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
  if(cfg::lock_player && cfg::locked_dev_id.length() > 0){
    return cfg::locked_dev_id;
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

// 4. Playback Dispatch via Core Hub
inline int play_media(const String& base_url, const String& child_id, const String& card_type, const String& media_id, const String& device_id, int chapter_idx){
  if(base_url.length() == 0) return -1;
  WiFiClientSecure scli; WiFiClient pcli; HTTPClient http;
  if(!http_begin_url(http, scli, pcli, base_url + "/api/play")) return -1;
  http.addHeader("Content-Type", "application/json");
  if(s_access_token.length() > 0){
    http.addHeader("Authorization", String("Bearer ") + s_access_token);
  }
  http.setTimeout(6000);

  JsonDocument doc;
  doc["child"]     = child_id.length() ? child_id : "default";
  doc["type"]      = card_type.length() ? card_type : "yoto";
  doc["id"]        = media_id;
  doc["device_id"] = device_id;
  doc["chapter"]   = chapter_idx + 1; // 1-based chapter for broker

  String payload;
  serializeJson(doc, payload);

  int code = http.POST(payload);
  http.end();
  return code;
}

// 5. Transport Control (pause, resume, stop, volume)
inline int player_control(const String& base_url, const String& device_id, const char* action, int volume = -1){
  if(base_url.length() == 0) return -1;
  WiFiClientSecure scli; WiFiClient pcli; HTTPClient http;
  if(!http_begin_url(http, scli, pcli, base_url + "/api/player/control")) return -1;
  http.addHeader("Content-Type", "application/json");
  if(s_access_token.length() > 0){
    http.addHeader("Authorization", String("Bearer ") + s_access_token);
  }
  http.setTimeout(6000);

  JsonDocument doc;
  doc["device_id"] = device_id;
  doc["action"]    = action;
  if(volume >= 0) doc["volume"] = volume;

  String payload;
  serializeJson(doc, payload);

  int code = http.POST(payload);
  http.end();
  return code;
}

// 6. Fetch Child Catalog (ABS + Yoto merged via Core hub)
inline bool fetch_catalog(const String& base_url, const String& child_id, void (*card_cb)(const String& id, const String& title, const String& author, const String& cover, const String& src, const String& series, int seq)){
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
    PsramAlloc palloc; JsonDocument doc(&palloc);
    DeserializationError jerr = deserializeJson(doc, (const char*)body, blen);
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
      int seq = (int)(item["sequence"] | 0);
      card_cb(id, title, author, cover, src_name, series, seq);
    }
  };

  fetch_src("/api/kids/" + child_id + "/abs", "abs");
  fetch_src("/api/kids/" + child_id + "/yoto", "yoto");
  return true;
}

// 7. Fetch Media Tracks / Chapters via Core Hub
inline bool fetch_media_tracks(const String& base_url, const String& media_id, String& title_out, void (*track_cb)(const String& key, const String& title, int dur)){
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
    String key = (const char*)(tr["key"] | tr["track_key"] | "");
    if(key.length() == 0) key = String(idx);
    String tr_title = (const char*)(tr["title"] | "");
    if(tr_title.length() == 0) tr_title = String("Chapter ") + idx;
    int dur = (int)(tr["duration_seconds"] | tr["duration"] | 0);
    track_cb(key, tr_title, dur);
    idx++;
  }
  return true;
}

} // namespace core
