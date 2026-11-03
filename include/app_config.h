#pragma once
// Runtime app settings — NVS-backed, editable without reflashing (via the on-device
// grown-ups screen and the web portal at http://yoto-controller.local/settings).
// secrets.h still provides the compile-time DEFAULTS; NVS overrides once saved, so a
// flashed board can be re-pointed at new WiFi / new Yoto credentials in the field.
// Header-only, included by main.cpp only (single TU), matching yoto_api.h style.
#include <Arduino.h>
#include <Preferences.h>
#include "secrets.h"

#ifndef STORYDECK_VERSION
#define STORYDECK_VERSION "dev"
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
static String core_url;        // Base URL of StoryDeck Core container (e.g. http://192.168.1.11:5060)
static bool   lock_player   = false; // Lock audio output to specific player vs profile roaming
static String locked_dev_id = "";    // Locked player ID

// ---- parent knobs (on-device settings screen) ----
static int    vol_max   = 60;  // hard volume cap for little ears
static int    saver_min = 10;  // idle minutes before the night-clock screensaver (0 = off)
static int    wake_h    = 7;   // OK-to-wake: clock turns green at this time...
static int    wake_m    = 0;
static int    night_h   = 19;  // bedtime: clock turns red from this time...
static int    night_m   = 0;
static int    saver_size= 2;   // night-clock digit size: 0 small / 1 medium / 2 large
static String tz;              // POSIX TZ for the clock (default US Eastern)
static String parent_pin;      // 4-digit Grown-ups security PIN (default "1234")

static void load(){
  s_prefs.begin("appcfg", false);
  kiosk_name    = s_prefs.getString("kname", "StoryDeck Touch");
  wifi_ssid     = s_prefs.getString("ssid",  WIFI_SSID);
  wifi_pass     = s_prefs.getString("pass",  WIFI_PASSWORD);
  client_id     = s_prefs.getString("cid",   YOTO_CLIENT_ID);
  client_secret = s_prefs.getString("csec",  YOTO_CLIENT_SECRET);
  device_id     = s_prefs.getString("devid", YOTO_DEVICE_ID);
  op_mode       = s_prefs.getString("mode",  "standalone");
  core_url      = s_prefs.getString("curl",   "");
  lock_player   = s_prefs.getBool("lockp",   false);
  locked_dev_id = s_prefs.getString("lockid", "");
  tz            = s_prefs.getString("tz",    "EST5EDT,M3.2.0,M11.1.0");
  parent_pin    = s_prefs.getString("pin",   "1234");
  vol_max       = s_prefs.getInt("volmax", 60);
  saver_min     = s_prefs.getInt("saver",  10);
  wake_h        = s_prefs.getInt("wakeh",  7);
  wake_m        = s_prefs.getInt("wakem",  0);
  night_h       = s_prefs.getInt("nighth", 19);
  night_m       = s_prefs.getInt("nightm", 0);
  saver_size    = s_prefs.getInt("svsize", 2);
}

// Persist everything (NVS only rewrites changed keys, so blanket-saving is cheap).
static void save(){
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
  s_prefs.putString("tz",    tz);
  s_prefs.putString("pin",   parent_pin);
  s_prefs.putInt("volmax", vol_max);
  s_prefs.putInt("saver",  saver_min);
  s_prefs.putInt("wakeh",  wake_h);
  s_prefs.putInt("wakem",  wake_m);
  s_prefs.putInt("nighth", night_h);
  s_prefs.putInt("nightm", night_m);
  s_prefs.putInt("svsize", saver_size);
}

} // namespace cfg
