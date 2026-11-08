#pragma once
// Runtime app settings — NVS-backed, editable without reflashing (via the on-device
// grown-ups screen and the web portal at http://storydeck-touch.local/settings).
// secrets.h still provides the compile-time DEFAULTS; NVS overrides once saved, so a
// flashed board can be re-pointed at new WiFi / new Yoto credentials in the field.
// Header-only, included by main.cpp only (single TU), matching yoto_api.h style.
#include <Arduino.h>
#include <Preferences.h>
#if __has_include("secrets.h")
#include "secrets.h"
#elif __has_include("../secrets.h")
#include "../secrets.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID            ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD        ""
#endif
#ifndef YOTO_CLIENT_ID
#define YOTO_CLIENT_ID       ""
#endif
#ifndef YOTO_CLIENT_SECRET
#define YOTO_CLIENT_SECRET   ""
#endif
#ifndef YOTO_DEVICE_ID
#define YOTO_DEVICE_ID       ""
#endif
#ifndef YOTO_SEED_REFRESH
#define YOTO_SEED_REFRESH    ""
#endif
#ifndef OTA_PASSWORD
#define OTA_PASSWORD         ""
#endif
#ifndef STORYDECK_VERSION
#define STORYDECK_VERSION "0.1.0"
#endif
namespace cfg {
static Preferences s_prefs;
// ---- connectivity / identity (web portal) ----
static String kiosk_name;      // Friendly display name for this controller panel / hostname
static String wifi_ssid;       // 2.4GHz only
static String wifi_pass;
static String client_id;       // Yoto OAuth confidential client
static String client_secret;
static String device_id;       // the Yoto player this panel commands
static String op_mode;         // "core" (StoryDeck Core) vs "standalone" (Direct Yoto Cloud)
static String core_url;        // Base URL of StoryDeck Core container (e.g. http://<server-ip>:5055)
static String assigned_profile = ""; // Assigned child profile ID (Core mode)
static bool   lock_player   = false; // Lock audio output to specific player vs profile roaming
static String locked_dev_id = "";    // Locked player ID
// ---- parent knobs (on-device settings screen) ----
static int    sleep_min    = 0;     // sleep timer preset minutes (0 = Off / Continuous, 15, 30, 45, 60)
static bool   sleep_bedtime= true;  // true = Bedtime Hours Only ("bedtime"), false = All the Time ("always")
static bool   block_remote = true;  // true = Block Away Players, false = Allow / Prompt
static int    saver_min    = 10;    // idle minutes before the night-clock screensaver (0 = off)
static int    wake_h       = 7;     // OK-to-wake: clock turns green at this time...
static int    wake_m       = 0;
static int    night_h      = 19;    // bedtime: clock turns red from this time...
static int    night_m      = 0;
static int    saver_size   = 2;     // night-clock digit size: 0 small / 1 medium / 2 large
static String tz;                   // POSIX TZ for the clock (default US Eastern)
static String parent_pin;           // 4-digit Grown-ups security PIN (default "1234")
static int    catalog_card_limit = 60; // Configurable catalog size ceiling (default 60)
static int    rotation_hour      = 3;  // Daily rotation hour (0-23, default 3 AM)
static void load(){
  s_prefs.begin("appcfg", false);
  kiosk_name    = s_prefs.getString("kname", "StoryDeck Touch");
#ifdef WOKWI_SIMULATION
  wifi_ssid     = s_prefs.getString("ssid",  "Wokwi-GUEST");
  wifi_pass     = s_prefs.getString("pass",  "");
  client_id     = s_prefs.getString("cid",   YOTO_CLIENT_ID);
  client_secret = s_prefs.getString("csec",  YOTO_CLIENT_SECRET);
  device_id     = s_prefs.getString("devid", YOTO_DEVICE_ID);
  op_mode       = s_prefs.getString("mode",  "core");
  #ifndef WOKWI_CORE_URL
  #define WOKWI_CORE_URL "http://host.wokwi.internal:5055"
  #endif
  core_url      = s_prefs.getString("curl",  WOKWI_CORE_URL);
#else
  String default_ssid = (strcmp(WIFI_SSID, "your-2.4GHz-ssid") == 0) ? "" : String(WIFI_SSID);
  String default_pass = (strcmp(WIFI_PASSWORD, "your-wifi-password") == 0) ? "" : String(WIFI_PASSWORD);
  wifi_ssid     = s_prefs.getString("ssid",  default_ssid);
  wifi_pass     = s_prefs.getString("pass",  default_pass);
  client_id     = s_prefs.getString("cid",   "");
  client_secret = s_prefs.getString("csec",  "");
  device_id     = s_prefs.getString("devid", "");
  op_mode       = s_prefs.getString("mode",  "");
  core_url      = s_prefs.getString("curl",  "");
#endif
  lock_player   = s_prefs.getBool("lockp",   false);
  locked_dev_id = s_prefs.getString("lockid", "");
  assigned_profile = s_prefs.getString("prof",   "");
  tz            = s_prefs.getString("tz",    "EST5EDT,M3.2.0,M11.1.0");
  parent_pin    = s_prefs.getString("pin",   "1234");
  catalog_card_limit = s_prefs.getInt("catlim", 60);
  rotation_hour      = s_prefs.getInt("rothr",  3);
  sleep_min     = s_prefs.getInt("slpmin", 0);
  sleep_bedtime = s_prefs.getBool("slpbed", true);
  block_remote  = s_prefs.getBool("blkrem", true);
  saver_min     = s_prefs.getInt("saver",  10);
  wake_h        = s_prefs.getInt("wakeh",  7);
  wake_m        = s_prefs.getInt("wakem",  0);
  night_h       = s_prefs.getInt("nighth", 19);
  night_m       = s_prefs.getInt("nightm", 0);
  saver_size    = s_prefs.getInt("svsize", 2);
}
// Check if device has completed initial setup
static bool is_configured(){
  if(s_prefs.getBool("configured", false)) return true;
  if(op_mode == "core" && core_url.length() > 0 && core_url != "http://localhost:5055") return true;
  if(op_mode == "standalone" && client_id.length() > 0 && client_id != "your-yoto-client-id") return true;
  return false;
}

// Persist everything (NVS only rewrites changed keys, so blanket-saving is cheap).
static void save(){
  s_prefs.putBool("configured", true);
  s_prefs.putString("kname", kiosk_name);
  s_prefs.putString("ssid",  wifi_ssid);
  s_prefs.putString("pass",  wifi_pass);
  s_prefs.putString("cid",   client_id);
  s_prefs.putString("csec",  client_secret);
  s_prefs.putString("devid", device_id);
  s_prefs.putString("mode",  op_mode);
  s_prefs.putString("curl",   core_url);
  s_prefs.putBool("lockp",   lock_player);
  s_prefs.putString("lockid", locked_dev_id);
  s_prefs.putString("prof",   assigned_profile);
  s_prefs.putString("tz",    tz);
  s_prefs.putString("pin",   parent_pin);
  s_prefs.putInt("catlim", catalog_card_limit);
  s_prefs.putInt("rothr",  rotation_hour);
  s_prefs.putInt("slpmin", sleep_min);
  s_prefs.putBool("slpbed", sleep_bedtime);
  s_prefs.putBool("blkrem", block_remote);
  s_prefs.putInt("saver",  saver_min);
  s_prefs.putInt("wakeh",  wake_h);
  s_prefs.putInt("wakem",  wake_m);
  s_prefs.putInt("nighth", night_h);
  s_prefs.putInt("nightm", night_m);
  s_prefs.putInt("svsize", saver_size);
}
} // namespace cfg
