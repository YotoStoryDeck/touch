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

static String mode_select(){
  String h = "<label>Operational Mode<select name='mode' id='mode_sel' onchange='toggleModeFields()'>";
  h += String("<option value='core'") + (cfg::op_mode == "core" ? " selected" : "") + ">StoryDeck Core Managed (Hub + Profiles)</option>";
  h += String("<option value='standalone'") + (cfg::op_mode != "core" ? " selected" : "") + ">Standalone Cloud (Direct Yoto API)</option>";
  h += "</select><small>Choose whether this screen connects to your local StoryDeck Core server or directly to Yoto Cloud.</small></label>";
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

  if(WiFi.getMode() & WIFI_MODE_AP){
    WiFi.disconnect(false);
    delay(50);
  }

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

// Profile & Target Device Selection Section (Grown-ups & Admin parity)
static String profile_device_picker(){
  String h = "<div id='prof_device_sec'>";
  h += "<h2>Profile &amp; Device Setup</h2>";

  // Child Profile
  h += "<label>Assigned Child Profile (Single-User Touch)";
  h += "<select id='prof_sel' name='prof' onchange='onProfileSelectChange()'>";
  h += "<option value=''>-- Select Profile or type below --</option>";
  for(int i = 0; i < core::s_profile_count; i++){
    bool sel = (cfg::assigned_profile == core::s_profiles[i].id);
    h += "<option value=\"" + esc(core::s_profiles[i].id) + "\" data-player=\"" + esc(core::s_profiles[i].default_player_id) + "\"" + (sel ? " selected" : "") + ">" + esc(core::s_profiles[i].display_name) + " (" + esc(core::s_profiles[i].id) + ")</option>";
  }
  h += "</select>";
  h += "<input id='prof_in' name='prof_custom' value=\"" + esc(cfg::assigned_profile) + "\" placeholder='or type Profile ID (e.g. child-id)' style='margin-top:6px;'>";
  h += "<small>Select from discovered Core profiles or type profile ID. The controller boots directly into this child's library.</small></label>";

  // Target Player
  h += "<label style='margin-top:14px;'>Target Yoto Player (Audio Output)";
  h += "<select id='dev_sel' name='devid' onchange='onPlayerSelectChange()'>";
  h += "<option value=''>-- Select Player or type below --</option>";
  
  // Aggregate unique players across Core profiles & standalone list
  String seen_ids[32]; int seen_cnt = 0;
  auto add_player_opt = [&](const String& pid, const String& pname, bool online){
    if(pid.length() == 0 || seen_cnt >= 32) return;
    for(int s = 0; s < seen_cnt; s++){ if(seen_ids[s] == pid) return; }
    seen_ids[seen_cnt++] = pid;
    bool sel = (cfg::device_id == pid || cfg::locked_dev_id == pid);
    h += "<option value=\"" + esc(pid) + "\"" + (sel ? " selected" : "") + ">" + esc(pname) + " (" + esc(pid) + ") &bull; " + (online ? "Online" : "Offline") + "</option>";
  };

  for(int i = 0; i < core::s_profile_count; i++){
    for(int p = 0; p < core::s_profiles[i].player_count; p++){
      add_player_opt(core::s_profiles[i].players[p].id, core::s_profiles[i].players[p].name, core::s_profiles[i].players[p].online);
    }
  }
  for(int p = 0; p < core::s_standalone_player_count; p++){
    add_player_opt(core::s_standalone_players[p].id, core::s_standalone_players[p].name, core::s_standalone_players[p].online);
  }

  h += "</select>";
  h += "<input id='dev_in' name='devid_custom' value=\"" + esc(cfg::device_id) + "\" placeholder='or type Yoto Player Device ID' style='margin-top:6px;'>";
  h += "<small>Select from discovered players or enter the Yoto Player ID that will play audio for this controller.</small></label>";

  // Lock Player Checkbox
  h += "<label style='display:flex;align-items:center;gap:8px;margin-top:10px;font-weight:500;cursor:pointer;'>";
  h += "<input type='checkbox' name='lockp' id='lockp' value='1' style='width:auto;margin:0;'" + String(cfg::lock_player ? " checked" : "") + ">";
  h += "<span>Lock this controller to the target player</span></label>";

  h += "<div style='margin-top:10px;'><button type='button' class='test-btn' onclick='fetchDiscovery()'>&#128260; Fetch Profiles &amp; Players from Core</button><span id='disc_badge'></span></div>";
  h += "</div>";
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
    "function onProfileSelectChange(){\n"
    "  var sel = document.getElementById('prof_sel');\n"
    "  var val = sel.value;\n"
    "  if(val){ document.getElementById('prof_in').value = val; }\n"
    "  var opt = sel.options[sel.selectedIndex];\n"
    "  var defPlayer = opt ? opt.getAttribute('data-player') : '';\n"
    "  if(defPlayer){\n"
    "    var dsel = document.getElementById('dev_sel');\n"
    "    dsel.value = defPlayer;\n"
    "    document.getElementById('dev_in').value = defPlayer;\n"
    "    document.getElementById('lockp').checked = true;\n"
    "  }\n"
    "}\n"
    "function onPlayerSelectChange(){\n"
    "  var val = document.getElementById('dev_sel').value;\n"
    "  if(val){\n"
    "    document.getElementById('dev_in').value = val;\n"
    "    document.getElementById('lockp').checked = true;\n"
    "  }\n"
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
    "      if(d.ok){ fetchDiscovery(); }\n"
    "    })\n"
    "    .catch(e => {\n"
    "      b.className = 'status-badge badge-err'; b.innerText = 'Ping request failed: ' + e.message;\n"
    "    });\n"
    "}\n"
    "function fetchDiscovery(){\n"
    "  var u = document.getElementById('curl').value;\n"
    "  var b = document.getElementById('disc_badge');\n"
    "  b.className = 'status-badge badge-wait'; b.innerText = 'Fetching profiles & players...';\n"
    "  fetch('/api/portal/discovery?url=' + encodeURIComponent(u))\n"
    "    .then(r => r.json())\n"
    "    .then(d => {\n"
    "      if(d.ok){\n"
    "        b.className = 'status-badge badge-ok';\n"
    "        b.innerText = 'Loaded ' + d.profiles.length + ' profiles, ' + d.players.length + ' players';\n"
    "        var psel = document.getElementById('prof_sel');\n"
    "        var curProf = document.getElementById('prof_in').value;\n"
    "        psel.innerHTML = '<option value=\"\">-- Select Profile or type below --</option>';\n"
    "        d.profiles.forEach(function(p){\n"
    "          var isSel = (p.id === curProf) ? ' selected' : '';\n"
    "          psel.innerHTML += '<option value=\"' + p.id + '\" data-player=\"' + (p.default_player_id || '') + '\"' + isSel + '>' + p.name + ' (' + p.id + ')</option>';\n"
    "        });\n"
    "        var dsel = document.getElementById('dev_sel');\n"
    "        var curDev = document.getElementById('dev_in').value;\n"
    "        dsel.innerHTML = '<option value=\"\">-- Select Player or type below --</option>';\n"
    "        d.players.forEach(function(pl){\n"
    "          var isSel = (pl.id === curDev) ? ' selected' : '';\n"
    "          dsel.innerHTML += '<option value=\"' + pl.id + '\"' + isSel + '>' + pl.name + ' (' + pl.id + ') • ' + (pl.online ? 'Online' : 'Offline') + '</option>';\n"
    "        });\n"
    "      } else {\n"
    "        b.className = 'status-badge badge-err'; b.innerText = d.error || 'Discovery failed';\n"
    "      }\n"
    "    })\n"
    "    .catch(e => {\n"
    "      b.className = 'status-badge badge-err'; b.innerText = 'Discovery error: ' + e.message;\n"
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
    text_row("StoryDeck Core URL", "curl", cfg::core_url, "Hub address (e.g. http://<server-ip>:5065 or https://storydeck.local). Required for Core Managed mode.") +
    "<div><button type='button' class='test-btn' onclick='testCore()'>Test StoryDeck Core URL</button><span id='core_badge'></span></div>" +
    "</div>" +
    "<div id='yoto_sec'>" +
    "<h2>Yoto Account (Standalone Direct)</h2>" +
    text_row("Client ID", "cid", cfg::client_id, "From the Yoto developer dashboard.") +
    secret_row("Client Secret", "csec", "From the Yoto developer dashboard.") +
    secret_row("Refresh Token", "rt", "Paste a fresh refresh token to re-seed credentials.") +
    "</div>" +
    profile_device_picker() +
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
  v = w.arg("pin");   if(v.length() == 4) cfg::parent_pin = v;

  // Profile assignment
  String prof = w.arg("prof_custom");
  if(prof.length() == 0) prof = w.arg("prof");
  prof.trim();
  cfg::assigned_profile = prof;
  core::s_assigned_profile = prof;

  // Target Player assignment
  String dev = w.arg("devid_custom");
  if(dev.length() == 0) dev = w.arg("devid");
  dev.trim();
  if(dev.length() > 0){
    cfg::device_id = dev;
    cfg::locked_dev_id = dev;
    cfg::lock_player = (w.hasArg("lockp") && (w.arg("lockp") == "1" || w.arg("lockp") == "on"));
  } else {
    cfg::locked_dev_id = "";
    cfg::lock_player = false;
  }

  cfg::save();
  v = w.arg("rt");
  if(v.length()) yoto::set_refresh(v);                  // also clears the cached access token
  w.send(200, "text/html; charset=utf-8",
    "<!doctype html><body style='font-family:system-ui;background:#fbf7f0;padding:24px'>"
    "<h2 style='color:#3a6b50'>Saved.</h2>"
    "<p>Settings saved to device flash. Changes take effect immediately or upon reboot.</p>"
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

// Live Discovery of Profiles & Players from StoryDeck Core
static void handle_portal_discovery(WebServer& w){
  if(!auth_ok(w)) return;
  String u = w.arg("url");
  if(u.length() == 0) u = cfg::core_url;
  if(u.length() == 0){
    w.send(200, "application/json", "{\"ok\":false,\"error\":\"Core URL empty\"}");
    return;
  }

  bool ok = core::fetch_config_and_profiles(u, false);
  if(!ok && core::s_profile_count == 0){
    w.send(200, "application/json", "{\"ok\":false,\"error\":\"Could not load profiles from Core\"}");
    return;
  }

  String out = "{\"ok\":true,\"profiles\":[";
  for(int i = 0; i < core::s_profile_count; i++){
    if(i > 0) out += ",";
    out += "{\"id\":\"" + esc(core::s_profiles[i].id) + "\",";
    out += "\"name\":\"" + esc(core::s_profiles[i].display_name) + "\",";
    out += "\"default_player_id\":\"" + esc(core::s_profiles[i].default_player_id) + "\"}";
  }
  out += "],\"players\":[";
  int pcount = 0;
  String seen[32]; int seen_cnt = 0;
  auto append_json_player = [&](const String& pid, const String& pname, bool online){
    if(pid.length() == 0 || seen_cnt >= 32) return;
    for(int s = 0; s < seen_cnt; s++){ if(seen[s] == pid) return; }
    seen[seen_cnt++] = pid;
    if(pcount > 0) out += ",";
    out += "{\"id\":\"" + esc(pid) + "\",";
    out += "\"name\":\"" + esc(pname) + "\",";
    out += "\"online\":" + String(online ? "true" : "false") + "}";
    pcount++;
  };

  for(int i = 0; i < core::s_profile_count; i++){
    for(int p = 0; p < core::s_profiles[i].player_count; p++){
      append_json_player(core::s_profiles[i].players[p].id, core::s_profiles[i].players[p].name, core::s_profiles[i].players[p].online);
    }
  }
  for(int p = 0; p < core::s_standalone_player_count; p++){
    append_json_player(core::s_standalone_players[p].id, core::s_standalone_players[p].name, core::s_standalone_players[p].online);
  }
  out += "]}";

  w.send(200, "application/json", out);
}

static void begin(WebServer& w){
  w.on("/settings", HTTP_GET,  [&w](){ handle_get(w);    });
  w.on("/settings", HTTP_POST, [&w](){ handle_post(w);   });
  w.on("/reboot",   HTTP_GET,  [&w](){ handle_reboot(w); });
  w.on("/api/ping/wifi", HTTP_GET, [&w](){ handle_ping_wifi(w); });
  w.on("/api/ping/core", HTTP_GET, [&w](){ handle_ping_core(w); });
  w.on("/api/portal/discovery", HTTP_GET, [&w](){ handle_portal_discovery(w); });
}

} // namespace portal
