#pragma once
// Web settings portal — http://yoto-controller.local/settings (or http://192.168.4.1/settings
// when the board is in Yoto-Setup hotspot mode). Rides the SAME WebServer that ElegantOTA
// uses, so it costs no extra sockets/RAM. Lets you change WiFi + Yoto credentials + timezone
// WITHOUT reflashing: values save to NVS (cfg::), which overrides the secrets.h defaults.
// Pasting a refresh token reseeds yoto:: (e.g. after a re-login that added scopes).
// Plain HTTP on the home LAN; reuses the OTA password as HTTP basic auth when one is set.
// Header-only, included by main.cpp only.
#include <WebServer.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "app_config.h"
#include "yoto_api.h"
#include "core_api.h"

namespace portal {

static bool auth_ok(WebServer& w){
  if(sizeof(OTA_PASSWORD) <= 1) return true;          // empty define => open (trusted LAN)
  if(w.authenticate("admin", OTA_PASSWORD)) return true;
  w.requestAuthentication();
  return false;
}

// Minimal HTML-attribute escape so an SSID with quotes can't break the form.
static String esc(const String& s){
  String o; o.reserve(s.length() + 8);
  for(char c : s){
    if(c=='&') o += "&amp;"; else if(c=='<') o += "&lt;";
    else if(c=='>') o += "&gt;"; else if(c=='"') o += "&quot;";
    else o += c;
  }
  return o;
}

static String text_row(const char* label, const char* name, const String& val, const char* hint=nullptr, const char* list=nullptr){
  String h = "<label>" + String(label) + "<input name='" + name + "' id='" + name + "' value=\"" + esc(val) + "\"";
  if(list){ h += " list='"; h += list; h += "'"; }
  h += ">";
  if(hint){ h += "<small>"; h += hint; h += "</small>"; }
  h += "</label>";
  return h;
}
static String secret_row(const char* label, const char* name, const char* hint){
  return "<label>" + String(label) + "<input name='" + name + "' id='" + name +
         "' placeholder='(unchanged — leave blank to keep)'><small>" + hint + "</small></label>";
}

// Friendly timezone dropdown. Values are the POSIX TZ rule strings the ESP32's C library
// needs (it has no zoneinfo database), but the user only ever sees city names. If NVS holds
// a string that isn't one of the presets (hand-set custom zone), it appears as a selected
// "Custom" entry so saving the form never silently rewrites it.
static String mode_select(){
  String h = "<label>Operational Mode<select name='mode' id='mode_sel' onchange='toggleModeFields()'>";
  h += String("<option value='standalone'") + (cfg::op_mode != "core" ? " selected" : "") + ">Standalone Cloud (Direct Yoto API)</option>";
  h += String("<option value='core'") + (cfg::op_mode == "core" ? " selected" : "") + ">StoryDeck Core Managed (Hub + Profiles)</option>";
  h += "</select><small>Choose whether this screen connects directly to Yoto Cloud or your local StoryDeck Core server.</small></label>";
  return h;
}

static String tz_select(){
  static const struct { const char* label; const char* val; } zones[] = {
    {"US Eastern (New York)",    "EST5EDT,M3.2.0,M11.1.0"},
    {"US Central (Chicago)",     "CST6CDT,M3.2.0,M11.1.0"},
    {"US Mountain (Denver)",     "MST7MDT,M3.2.0,M11.1.0"},
    {"Arizona (no DST)",         "MST7"},
    {"US Pacific (Los Angeles)", "PST8PDT,M3.2.0,M11.1.0"},
    {"Alaska",                   "AKST9AKDT,M3.2.0,M11.1.0"},
    {"Hawaii",                   "HST10"},
  };
  String h = "<label>Timezone<select name='tz'>";
  bool found = false;
  for(auto& z : zones){
    bool sel = (cfg::tz == z.val); found |= sel;
    h += String("<option value=\"") + z.val + "\"" + (sel ? " selected" : "") + ">" + z.label + "</option>";
  }
  if(!found)
    h += "<option value=\"" + esc(cfg::tz) + "\" selected>Custom (" + esc(cfg::tz) + ")</option>";
  h += "</select><small>Sets the night clock and OK-to-wake to local time.</small></label>";
  return h;
}

// Scan nearby 2.4 GHz WiFi networks and build a select picker + input combo
static String wifi_picker(){
  String h = "<label>Network name (SSID)<div style='display:flex;gap:8px;margin-top:4px'>";
  h += "<select id='wf_sel' onchange='var el=document.getElementById(\"wf_in\");if(this.value)el.value=this.value' style='width:50%;margin-top:0'>";
  h += "<option value=''>-- Select nearby WiFi --</option>";

  // If in AP or AP_STA mode, pause background STA retries so the single radio can hop channels
  if(WiFi.getMode() & WIFI_MODE_AP){
    WiFi.disconnect(false);
    delay(50);
  }

  // Active scan without hidden SSIDs (async=false, show_hidden=false, passive=false, max_ms_per_chan=200)
  int n = WiFi.scanNetworks(false, false, false, 200);
  if(n > 0){
    for(int i = 0; i < n; i++){
      String ssid = WiFi.SSID(i);
      if(ssid.length() == 0) continue;
      int32_t rssi = WiFi.RSSI(i);
      h += "<option value=\"" + esc(ssid) + "\">" + esc(ssid) + " (" + String(rssi) + " dBm)</option>";
    }
    WiFi.scanDelete();
  }
  h += "</select>";
  h += "<input id='wf_in' name='ssid' value=\"" + esc(cfg::wifi_ssid) + "\" placeholder='or type SSID' style='width:50%;margin-top:0'>";
  h += "</div><small>Pick a nearby 2.4 GHz network from the dropdown or type manually.</small></label>";
  return h;
}

static void handle_get(WebServer& w){
  if(!auth_ok(w)) return;
  String h =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>StoryDeck Touch</title><style>"
    "body{font-family:-apple-system,system-ui,sans-serif;background:#fbf7f0;color:#23303a;max-width:560px;margin:0 auto;padding:18px}"
    "h1{color:#e5352b;font-size:1.5em}h2{font-size:1.05em;margin:1.4em 0 .3em;color:#3a6b50}"
    "label{display:block;margin:.7em 0;font-weight:600;font-size:.92em}"
    "input,select{display:block;width:100%;box-sizing:border-box;padding:9px;margin-top:4px;border:1px solid #d8d0c0;border-radius:9px;font-size:1em;background:#fff}"
    "small{font-weight:400;color:#9a9384}"
    "button.save-btn{background:#e5352b;color:#fff;border:0;border-radius:11px;padding:12px 22px;font-size:1.05em;font-weight:700;margin-top:14px;cursor:pointer;width:100%}"
    "button.test-btn{background:#fff;color:#23303a;border:1px solid #d8d0c0;border-radius:8px;padding:6px 14px;font-size:.88em;font-weight:600;cursor:pointer;margin-top:6px}"
    "button.test-btn:hover{background:#f3e9d8}"
    "a{color:#e5352b}.info{background:#fff;border-radius:11px;padding:10px 14px;font-size:.9em;color:#666}"
    ".status-badge{display:inline-block;padding:3px 8px;border-radius:6px;font-size:0.85em;margin-left:8px;font-weight:600}"
    ".badge-ok{background:#e6f4ea;color:#137333}.badge-err{background:#fce8e6;color:#c5221f}.badge-wait{background:#e8eaed;color:#5f6368}"
    "</style><script>\n"
    "function toggleModeFields(){\n"
    "  var m = document.getElementById('mode_sel').value;\n"
    "  var isCore = (m === 'core');\n"
    "  var cSec = document.getElementById('core_sec');\n"
    "  var ySec = document.getElementById('yoto_sec');\n"
    "  if(cSec) cSec.style.display = isCore ? 'block' : 'none';\n"
    "  if(ySec) ySec.style.display = isCore ? 'none' : 'block';\n"
    "}\n"
    "function testWifi(){\n"
    "  var s = document.getElementById('wf_in').value;\n"
    "  var p = document.getElementById('pass').value;\n"
    "  var b = document.getElementById('wifi_badge');\n"
    "  b.className = 'status-badge badge-wait'; b.innerText = 'Testing connection...';\n"
    "  fetch('/api/ping/wifi?ssid=' + encodeURIComponent(s) + '&pass=' + encodeURIComponent(p))\n"
    "    .then(r => r.json())\n"
    "    .then(d => {\n"
    "      b.className = 'status-badge ' + (d.ok ? 'badge-ok' : 'badge-err');\n"
    "      b.innerText = d.ok ? ('Connected (IP: ' + d.ip + ')') : ('Failed: ' + (d.error || 'Check credentials'));\n"
    "    })\n"
    "    .catch(e => {\n"
    "      b.className = 'status-badge badge-err'; b.innerText = 'Ping failed: ' + e.message;\n"
    "    });\n"
    "}\n"
    "function testCore(){\n"
    "  var u = document.getElementById('curl').value;\n"
    "  var b = document.getElementById('core_badge');\n"
    "  b.className = 'status-badge badge-wait'; b.innerText = 'Pinging Core...';\n"
    "  fetch('/api/ping/core?url=' + encodeURIComponent(u))\n"
    "    .then(r => r.json())\n"
    "    .then(d => {\n"
    "      b.className = 'status-badge ' + (d.ok ? 'badge-ok' : 'badge-err');\n"
    "      b.innerText = d.ok ? 'StoryDeck Core reachable (HTTP 200)' : ('Unreachable: ' + (d.error || 'Check URL/DNS'));\n"
    "    })\n"
    "    .catch(e => {\n"
    "      b.className = 'status-badge badge-err'; b.innerText = 'Ping request failed: ' + e.message;\n"
    "    });\n"
    "}\n"
    "window.addEventListener('DOMContentLoaded', toggleModeFields);\n"
    "</script></head><body><h1>StoryDeck Touch Settings</h1>"
    "<div class=info>IP " + WiFi.localIP().toString() +
    " &middot; WiFi " + String(WiFi.RSSI()) + " dBm"
    " &middot; free RAM " + String(ESP.getFreeHeap()/1024) + " KB"
    " &middot; v" STORYDECK_VERSION " &middot; <a href='/update'>firmware update</a></div>"
    "<form method='POST' action='/settings'>"
    "<h2>Device Identity</h2>" +
    text_row("Device Friendly Name", "kname", cfg::kiosk_name, "Host and display name for this controller panel.") +
    "<h2>WiFi (2.4 GHz only)</h2>" +
    wifi_picker() +
    secret_row("Password", "pass", "Saved on the board, used at next boot.") +
    "<div><button type='button' class='test-btn' onclick='testWifi()'>Test WiFi Connection</button><span id='wifi_badge'></span></div>"
    "<h2>StoryDeck Integration</h2>" +
    mode_select() +
    "<div id='core_sec'>" +
    text_row("StoryDeck Core URL", "curl", cfg::core_url, "Hub address (e.g. https://storydeck.ohiodeans.us or http://192.168.1.11:5065). Required for Core Managed mode.") +
    "<div><button type='button' class='test-btn' onclick='testCore()'>Test StoryDeck Core URL</button><span id='core_badge'></span></div>"
    "</div>" +
    "<div id='yoto_sec'>" +
    "<h2>Yoto account</h2>" +
    text_row("Client ID", "cid", cfg::client_id) +
    secret_row("Client secret", "csec", "From the Yoto developer dashboard.") +
    secret_row("Refresh token", "rt", "Paste a fresh one (from .yoto_tokens.json after oauth_login.py) to re-seed.") +
    "</div>" +
    "<h2>Clock</h2>" +
    tz_select() +
    "<h2>Security &amp; PIN</h2>" +
    text_row("Parent 4-Digit PIN", "pin", cfg::parent_pin, "Protects Grown-ups settings on the device touch screen (default 1234).") +
    "<button type='submit' class='save-btn'>Save Settings</button></form>"
    "<p><small>WiFi and credential changes take effect after a reboot: "
    "<a href='/reboot'>reboot now</a>.</small></p>"
    "</body></html>";
  w.send(200, "text/html; charset=utf-8", h);
}

static void handle_post(WebServer& w){
  if(!auth_ok(w)) return;
  // Blank secret fields mean "keep what's stored"; visible fields update when non-empty.
  String v;
  v = w.arg("kname"); if(v.length()) cfg::kiosk_name    = v;
  v = w.arg("ssid");  if(v.length()) cfg::wifi_ssid     = v;
  v = w.arg("pass");  if(v.length()) cfg::wifi_pass     = v;
  v = w.arg("cid");   if(v.length()) cfg::client_id     = v;
  v = w.arg("csec");  if(v.length()) cfg::client_secret = v;
  v = w.arg("mode");  if(v.length()) cfg::op_mode       = v;
  v = w.arg("curl");  cfg::core_url = v;
  v = w.arg("tz");    if(v.length()) cfg::tz            = v;
  v = w.arg("pin");   if(v.length() == 4) cfg::parent_pin   = v;
  cfg::save();
  v = w.arg("rt");
  if(v.length()) yoto::set_refresh(v);                  // also clears the cached access token
  w.send(200, "text/html; charset=utf-8",
    "<!doctype html><body style='font-family:system-ui;background:#fbf7f0;padding:24px'>"
    "<h2 style='color:#3a6b50'>Saved.</h2>"
    "<p>WiFi / credential changes apply at next boot.</p>"
    "<p><a href='/reboot'>Reboot now</a> &middot; <a href='/settings'>Back to settings</a></p></body>");
}

static void handle_reboot(WebServer& w){
  if(!auth_ok(w)) return;
  w.send(200, "text/html; charset=utf-8",
    "<!doctype html><body style='font-family:system-ui;background:#fbf7f0;padding:24px'>"
    "<h2>Rebooting&hellip;</h2><p>Give it ~15 seconds, then <a href='/settings'>reload</a>.</p></body>");
  delay(300);                                           // let the response flush
  ESP.restart();
}

// Test ping for WiFi credentials
static void handle_ping_wifi(WebServer& w){
  if(!auth_ok(w)) return;
  String s = w.arg("ssid");
  String p = w.arg("pass");
  if(s.length() == 0) s = cfg::wifi_ssid;
  if(p.length() == 0) p = cfg::wifi_pass;

  if(s.length() == 0){
    w.send(200, "application/json", "{\"ok\":false,\"error\":\"SSID empty\"}");
    return;
  }

  // If already connected to this SSID, return current IP directly
  if(WiFi.status() == WL_CONNECTED && WiFi.SSID() == s){
    String resp = "{\"ok\":true,\"ip\":\"" + WiFi.localIP().toString() + "\"}";
    w.send(200, "application/json", resp);
    return;
  }

  // Attempt connection with timeout
  WiFi.begin(s.c_str(), p.c_str());
  uint32_t t0 = millis();
  while(WiFi.status() != WL_CONNECTED && millis() - t0 < 8000){
    delay(200);
  }

  if(WiFi.status() == WL_CONNECTED){
    String resp = "{\"ok\":true,\"ip\":\"" + WiFi.localIP().toString() + "\"}";
    w.send(200, "application/json", resp);
  } else {
    // Restore configured network
    if(cfg::wifi_ssid.length() > 0){
      WiFi.begin(cfg::wifi_ssid.c_str(), cfg::wifi_pass.c_str());
    }
    w.send(200, "application/json", "{\"ok\":false,\"error\":\"Connection timed out\"}");
  }
}

// Test ping for StoryDeck Core URL
static void handle_ping_core(WebServer& w){
  if(!auth_ok(w)) return;
  String u = w.arg("url");
  if(u.length() == 0) u = cfg::core_url;

  if(u.length() == 0){
    w.send(200, "application/json", "{\"ok\":false,\"error\":\"Core URL empty\"}");
    return;
  }

  bool ok = core::ping(u);
  if(ok){
    w.send(200, "application/json", "{\"ok\":true}");
  } else {
    w.send(200, "application/json", "{\"ok\":false,\"error\":\"HTTP status != 200 or connection failed\"}");
  }
}

static void begin(WebServer& w){
  w.on("/settings", HTTP_GET,  [&w](){ handle_get(w);    });
  w.on("/settings", HTTP_POST, [&w](){ handle_post(w);   });
  w.on("/reboot",   HTTP_GET,  [&w](){ handle_reboot(w); });
  w.on("/api/ping/wifi", HTTP_GET, [&w](){ handle_ping_wifi(w); });
  w.on("/api/ping/core", HTTP_GET, [&w](){ handle_ping_core(w); });
}

} // namespace portal
