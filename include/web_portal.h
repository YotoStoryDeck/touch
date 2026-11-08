#pragma once
// Web settings portal — http://storydeck-touch.local/settings (or http://192.168.4.1/settings
// when the board is in StoryDeck-Setup hotspot mode). Rides the SAME WebServer that ElegantOTA
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
#include "standalone_curation.h"

extern bool fetch_library();
extern void build_grid();

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
  String h = "<label>Operational Mode<select name='mode' id='mode_sel' onchange='toggleModeFields()' required>";
  if(cfg::op_mode.length() == 0){
    h += "<option value='' disabled selected>-- Select Initial Operational Mode --</option>";
  }
  h += String("<option value='core'") + (cfg::op_mode == "core" ? " selected" : "") + ">StoryDeck Core Managed (Hub + Profiles)</option>";
  h += String("<option value='standalone'") + (cfg::op_mode == "standalone" ? " selected" : "") + ">Standalone Cloud (Direct Yoto API)</option>";
  h += "</select><small>Choose whether this screen connects to your local StoryDeck Core server or directly to Yoto Cloud.</small></label>";
  return h;
}

inline String parse_tz_input(const String& raw){
  String s = raw;
  s.trim();
  if(s.length() == 0) return "EST5EDT,M3.2.0,M11.1.0";

  String up = s;
  up.toUpperCase();
  if(up == "UTC" || up == "GMT" || up == "Z" || up == "UTC0" || up == "GMT0" || up == "0" || up == "+0" || up == "-0"){
    return "UTC0";
  }

  String rem = s;
  if(rem.startsWith("UTC") || rem.startsWith("utc")) rem = rem.substring(3);
  else if(rem.startsWith("GMT") || rem.startsWith("gmt")) rem = rem.substring(3);
  rem.trim();

  bool has_sign = (rem.startsWith("+") || rem.startsWith("-"));
  bool is_all_digits = true;
  for(size_t i = (has_sign ? 1 : 0); i < rem.length(); i++){
    char c = rem[i];
    if(c != ':' && (c < '0' || c > '9')){
      is_all_digits = false;
      break;
    }
  }

  if(rem.length() > 0 && is_all_digits){
    bool is_positive = !rem.startsWith("-");
    if(rem.startsWith("+") || rem.startsWith("-")) rem = rem.substring(1);
    int colon = rem.indexOf(':');
    int hours = 0;
    int mins = 0;
    if(colon > 0){
      hours = rem.substring(0, colon).toInt();
      mins = rem.substring(colon + 1).toInt();
    } else {
      hours = rem.toInt();
    }
    if(hours == 0 && mins == 0) return "UTC0";
    if(hours < 0) hours = -hours;
    if(mins < 0) mins = -mins;
    if(hours > 14) hours = 14;
    if(mins > 59) mins = 59;

    char label_buf[32];
    char offset_buf[32];
    char sign_char = is_positive ? '+' : '-';
    if(mins > 0){
      snprintf(label_buf, sizeof(label_buf), "<UTC%c%d:%02d>", sign_char, hours, mins);
      // In POSIX: east (+) is negative, west (-) is positive
      snprintf(offset_buf, sizeof(offset_buf), "%s%d:%02d", is_positive ? "-" : "", hours, mins);
    } else {
      snprintf(label_buf, sizeof(label_buf), "<UTC%c%d>", sign_char, hours);
      snprintf(offset_buf, sizeof(offset_buf), "%s%d", is_positive ? "-" : "", hours);
    }
    return String(label_buf) + offset_buf;
  }

  return s;
}

static String tz_select(){
  struct TzEntry { const char* grp; const char* label; const char* val; };
  static const TzEntry zones[] = {
    // North America
    {"North America", "US Eastern (New York, Miami, Atlanta)", "EST5EDT,M3.2.0,M11.1.0"},
    {"North America", "US Central (Chicago, Dallas)", "CST6CDT,M3.2.0,M11.1.0"},
    {"North America", "US Mountain (Denver, Salt Lake City)", "MST7MDT,M3.2.0,M11.1.0"},
    {"North America", "US Arizona (Phoenix - no DST)", "MST7"},
    {"North America", "US Pacific (Los Angeles, Seattle)", "PST8PDT,M3.2.0,M11.1.0"},
    {"North America", "US Alaska (Anchorage)", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"North America", "US Hawaii (Honolulu - no DST)", "HST10"},
    {"North America", "Canada Atlantic (Halifax)", "AST4ADT,M3.2.0,M11.1.0"},
    {"North America", "Canada Newfoundland (St. John's)", "NST3:30NDT,M3.2.0,M11.1.0"},
    {"North America", "Mexico (Mexico City)", "CST6"},

    // Central & South America
    {"Central & South America", "Colombia, Peru, Ecuador (Bogota, Lima)", "COT5"},
    {"Central & South America", "Venezuela (Caracas)", "VET4"},
    {"Central & South America", "Brazil (Sao Paulo, Rio de Janeiro)", "BRT3"},
    {"Central & South America", "Argentina (Buenos Aires)", "ART3"},
    {"Central & South America", "Chile (Santiago)", "CLT4CLST,M9.1.0/24,M4.1.0/24"},

    // Europe & UK
    {"Europe & UK", "UK & Ireland (London, Dublin)", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe & UK", "Western Europe (Lisbon)", "WET0WEST,M3.5.0/1,M10.5.0"},
    {"Europe & UK", "Central Europe (Berlin, Paris, Rome, Madrid, Amsterdam)", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe & UK", "Eastern Europe (Athens, Helsinki, Kyiv, Bucharest)", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe & UK", "Turkey (Istanbul)", "TRT-3"},

    // Africa & Middle East
    {"Africa & Middle East", "Egypt (Cairo)", "EET-2EEST,M4.5.5/0,M10.5.4/24"},
    {"Africa & Middle East", "South Africa (Johannesburg, Cape Town)", "SAST-2"},
    {"Africa & Middle East", "East Africa (Nairobi, Addis Ababa)", "EAT-3"},
    {"Africa & Middle East", "West Africa (Lagos, Accra)", "WAT-1"},
    {"Africa & Middle East", "Gulf / UAE (Dubai, Abu Dhabi)", "GST-4"},
    {"Africa & Middle East", "Israel (Jerusalem)", "IST-2IDT,M3.4.4/26,M10.5.0"},

    // Asia
    {"Asia", "Pakistan (Karachi, Islamabad)", "PKT-5"},
    {"Asia", "India & Sri Lanka (New Delhi, Mumbai, Colombo)", "IST-5:30"},
    {"Asia", "Nepal (Kathmandu)", "NPT-5:45"},
    {"Asia", "Bangladesh (Dhaka)", "BST-6"},
    {"Asia", "Thailand, Vietnam, Indonesia (Bangkok, Hanoi, Jakarta)", "ICT-7"},
    {"Asia", "China, Hong Kong, Singapore, Taiwan (Beijing, Singapore)", "CST-8"},
    {"Asia", "Japan & Korea (Tokyo, Seoul)", "JST-9"},

    // Australia & Pacific
    {"Australia & Pacific", "Australia Western (Perth)", "AWST-8"},
    {"Australia & Pacific", "Australia Northern (Darwin)", "ACST-9:30"},
    {"Australia & Pacific", "Australia South (Adelaide)", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"Australia & Pacific", "Australia Queensland (Brisbane)", "AEST-10"},
    {"Australia & Pacific", "Australia Eastern (Sydney, Melbourne, Hobart)", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Australia & Pacific", "New Zealand (Auckland, Wellington)", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
    {"Australia & Pacific", "Fiji", "FJT-12"},

    // Standard Fixed UTC Offsets
    {"Standard UTC Offsets", "UTC-12:00 (Baker Island)", "<UTC-12>12"},
    {"Standard UTC Offsets", "UTC-11:00 (Samoa, Niue)", "<UTC-11>11"},
    {"Standard UTC Offsets", "UTC-10:00 (Hawaii Std)", "<UTC-10>10"},
    {"Standard UTC Offsets", "UTC-09:00 (Alaska Std)", "<UTC-9>9"},
    {"Standard UTC Offsets", "UTC-08:00 (Pacific Std)", "<UTC-8>8"},
    {"Standard UTC Offsets", "UTC-07:00 (Mountain Std)", "<UTC-7>7"},
    {"Standard UTC Offsets", "UTC-06:00 (Central Std)", "<UTC-6>6"},
    {"Standard UTC Offsets", "UTC-05:00 (Eastern Std)", "<UTC-5>5"},
    {"Standard UTC Offsets", "UTC-04:00 (Atlantic Std)", "<UTC-4>4"},
    {"Standard UTC Offsets", "UTC-03:30 (Newfoundland Std)", "<UTC-3:30>3:30"},
    {"Standard UTC Offsets", "UTC-03:00 (Buenos Aires / Brasilia)", "<UTC-3>3"},
    {"Standard UTC Offsets", "UTC-02:00 (South Georgia)", "<UTC-2>2"},
    {"Standard UTC Offsets", "UTC-01:00 (Azores Std)", "<UTC-1>1"},
    {"Standard UTC Offsets", "UTC±00:00 (Greenwich / UTC)", "UTC0"},
    {"Standard UTC Offsets", "UTC+01:00 (Central European Std)", "<UTC+1>-1"},
    {"Standard UTC Offsets", "UTC+02:00 (Eastern European Std)", "<UTC+2>-2"},
    {"Standard UTC Offsets", "UTC+03:00 (Moscow / Arabia Std)", "<UTC+3>-3"},
    {"Standard UTC Offsets", "UTC+03:30 (Iran Std)", "<UTC+3:30>-3:30"},
    {"Standard UTC Offsets", "UTC+04:00 (Gulf Std)", "<UTC+4>-4"},
    {"Standard UTC Offsets", "UTC+04:30 (Afghanistan)", "<UTC+4:30>-4:30"},
    {"Standard UTC Offsets", "UTC+05:00 (Pakistan Std)", "<UTC+5>-5"},
    {"Standard UTC Offsets", "UTC+05:30 (India Standard Time)", "<UTC+5:30>-5:30"},
    {"Standard UTC Offsets", "UTC+05:45 (Nepal)", "<UTC+5:45>-5:45"},
    {"Standard UTC Offsets", "UTC+06:00 (Bangladesh Std)", "<UTC+6>-6"},
    {"Standard UTC Offsets", "UTC+06:30 (Myanmar)", "<UTC+6:30>-6:30"},
    {"Standard UTC Offsets", "UTC+07:00 (Indochina Time)", "<UTC+7>-7"},
    {"Standard UTC Offsets", "UTC+08:00 (China / Singapore / WA)", "<UTC+8>-8"},
    {"Standard UTC Offsets", "UTC+09:00 (Japan / Korea Std)", "<UTC+9>-9"},
    {"Standard UTC Offsets", "UTC+09:30 (Darwin Std)", "<UTC+9:30>-9:30"},
    {"Standard UTC Offsets", "UTC+10:00 (Australia Eastern Std)", "<UTC+10>-10"},
    {"Standard UTC Offsets", "UTC+11:00 (Solomon Islands)", "<UTC+11>-11"},
    {"Standard UTC Offsets", "UTC+12:00 (New Zealand Std)", "<UTC+12>-12"},
    {"Standard UTC Offsets", "UTC+13:00 (Tonga / Tokelau)", "<UTC+13>-13"},
    {"Standard UTC Offsets", "UTC+14:00 (Line Islands / Kiritimati)", "<UTC+14>-14"},
  };

  String h = "<label>Timezone<select name='tz' id='tz_sel'>";
  bool found = false;
  const char* cur_grp = nullptr;

  for(size_t i = 0; i < sizeof(zones)/sizeof(zones[0]); i++){
    auto& z = zones[i];
    if(!cur_grp || strcmp(cur_grp, z.grp) != 0){
      if(cur_grp) h += "</optgroup>";
      cur_grp = z.grp;
      h += String("<optgroup label=\"") + cur_grp + "\">";
    }
    bool sel = (cfg::tz == z.val);
    found |= sel;
    h += String("<option value=\"") + z.val + "\"" + (sel ? " selected" : "") + ">" + z.label + "</option>";
  }
  if(cur_grp) h += "</optgroup>";

  if(!found){
    h += "<optgroup label=\"Current Custom Setting\">";
    h += "<option value=\"" + esc(cfg::tz) + "\" selected>Custom (" + esc(cfg::tz) + ")</option>";
    h += "</optgroup>";
  }
  h += "</select></label>";

  h += "<label style='margin-top:8px'>Manual UTC Offset or Custom POSIX String"
       "<input type='text' id='tz_custom' name='tz_custom' value='' "
       "placeholder='e.g. UTC-5, UTC+2, +05:30, or custom POSIX' style='margin-top:4px'>"
       "<small>Entering a value here overrides the dropdown above. Supports friendly UTC offsets (e.g. UTC+1, -5, +5:30) or full POSIX strings.</small></label>";

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

  // Child Profile (Core Managed Mode only — hidden in Standalone)
  h += "<div id='prof_group'>";
  h += "<label>Assigned Child Profile (Core Managed Mode)";
  h += "<select id='prof_sel' name='prof' onchange='onProfileSelectChange()'>";
  h += "<option value=''>-- Select Profile or type below --</option>";
  for(int i = 0; i < core::s_profile_count; i++){
    bool sel = (cfg::assigned_profile == core::s_profiles[i].id);
    h += "<option value=\"" + esc(core::s_profiles[i].id) + "\" data-player=\"" + esc(core::s_profiles[i].default_player_id) + "\"" + (sel ? " selected" : "") + ">" + esc(core::s_profiles[i].display_name) + " (" + esc(core::s_profiles[i].id) + ")</option>";
  }
  h += "</select>";
  h += "<input id='prof_in' name='prof_custom' value=\"" + esc(cfg::assigned_profile) + "\" placeholder='or type Profile ID (e.g. child-id)' style='margin-top:6px;'>";
  h += "<small>Select from discovered Core profiles or type profile ID. The controller boots directly into this child's library.</small></label>";
  h += "</div>";

  // Target Player (Audio Output) — Required for all controllers
  h += "<div id='player_group'>";
  h += "<label style='margin-top:14px;'>Target Yoto Player (Required Audio Destination)";
  h += "<select id='dev_sel' name='devid' onchange='onPlayerSelectChange()'>";
  
  bool is_standalone = (cfg::op_mode == "standalone");
  bool yoto_authed = yoto::is_authenticated();

  if(is_standalone && !yoto_authed){
    h += "<option value=''>-- Sign in to Yoto above to discover players --</option>";
  } else {
    h += "<option value=''>-- Select Target Player --</option>";
  }
  
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
  h += "<input id='dev_in' name='devid_custom' value=\"" + esc(cfg::device_id) + "\" placeholder='or enter Yoto Player Device ID' style='margin-top:6px;'>";
  h += "<small style='display:block;margin-top:6px;color:#7a7060;'>🔒 All StoryDeck Touch controllers are dedicated single-user appliances and are permanently locked to their target player.</small>";
  h += "<div id='dev_standalone_notice' style='display:none;color:#e5352b;font-weight:600;font-size:0.9em;margin-top:6px;'>⚠️ Authenticate with Yoto above to discover and select your players.</div>";
  h += "</label>";
  h += "</div>";

  // Discovery Action Buttons
  h += "<div style='margin-top:12px;'>";
  h += "<button type='button' id='disc_btn_core' class='test-btn' onclick='fetchDiscovery()'>&#128260; Fetch Profiles &amp; Players from Core</button>";
  h += "<button type='button' id='disc_btn_yoto' class='test-btn' style='display:none;' onclick='fetchYotoPlayers()'>&#128260; Refresh Yoto Players</button>";
  h += "<span id='disc_badge'></span></div>";
  h += "</div>";
  return h;
}

static void handle_get(WebServer& w){
  if(!auth_ok(w)) return;
  String tab = w.arg("tab");
  if(tab == "curation" && cfg::op_mode != "core"){
    curation::load();
    String ch = "<!doctype html><html><head><meta charset='utf-8'>"
      "<meta name=viewport content='width=device-width,initial-scale=1'>"
      "<title>StoryDeck Standalone Curation</title><style>"
      "body{font-family:-apple-system,system-ui,sans-serif;background:#fbf7f0;color:#23303a;max-width:680px;margin:0 auto;padding:18px}"
      "h1{color:#e5352b;font-size:1.5em}h2{font-size:1.05em;margin:1.4em 0 .3em;color:#3a6b50}"
      "table{width:100%;border-collapse:collapse;margin-top:12px;background:#fff;border-radius:10px;overflow:hidden;box-shadow:0 1px 3px rgba(0,0,0,0.1)}"
      "th,td{padding:10px 12px;text-align:left;border-bottom:1px solid #f0eae0}"
      "th{background:#f4ede0;color:#23303a;font-weight:700;font-size:0.9em}"
      "button.save-btn{background:#e5352b;color:#fff;border:0;border-radius:11px;padding:12px 22px;font-size:1.05em;font-weight:700;margin-top:14px;cursor:pointer;width:100%}"
      "a.nav-btn{display:inline-block;padding:8px 16px;border-radius:8px;text-decoration:none;font-weight:700;font-size:0.95em}"
      "</style></head><body>"
      "<div style='display:flex;gap:8px;margin-bottom:18px;'>"
      "<a href='/settings' class='nav-btn' style='background:#f4ede0;color:#23303a;'>Settings</a>"
      "<a href='/settings?tab=curation' class='nav-btn' style='background:#e5352b;color:#fff;'>Card Curation</a>"
      "</div>"
      "<h1>Card Curation &amp; Ownership</h1>"
      "<p style='color:#7a7060;font-size:0.92em;'>Manage sticky favorites and mark physical plastic card copies owned in bedroom stands. Physical cards receive lower rotation probability unless hidden.</p>"
      "<form method='POST' action='/settings/curation'>"
      "<label style='display:flex;align-items:center;gap:10px;background:#fff;padding:12px;border-radius:10px;border:1px solid #e7e0d2;cursor:pointer;font-weight:600;'>"
      "<input type='checkbox' name='hide_phys' value='1'" + String(curation::get_hide_physical() ? " checked" : "") + " style='width:auto;margin:0;'>"
      "<span>Hide Physical Cards from Touchscreen Catalog</span></label>"
      "<table><thead><tr><th style='width:50px;'>Cover</th><th>Title</th><th style='width:70px;text-align:center;'>Sticky &#11088;</th><th style='width:70px;text-align:center;'>Physical &#127188;</th></tr></thead><tbody>";

    for(int i = 0; i < s_card_count; i++){
      if(s_cards[i].daily) continue;
      bool is_p = curation::is_card_pinned(s_cards[i].id.c_str());
      bool is_ph = curation::is_card_physical(s_cards[i].id.c_str());
      ch += "<tr><td><img src='" + esc(s_cards[i].cover.c_str()) + "' style='width:36px;height:54px;object-fit:cover;border-radius:4px;'></td>";
      ch += "<td><strong>" + esc(s_cards[i].title.c_str()) + "</strong><br><small style='color:#9a9384;'>" + esc(s_cards[i].id.c_str()) + "</small></td>";
      ch += "<td style='text-align:center;'><input type='checkbox' name='pin_" + esc(s_cards[i].id.c_str()) + "' value='1'" + (is_p ? " checked" : "") + "></td>";
      ch += "<td style='text-align:center;'><input type='checkbox' name='phys_" + esc(s_cards[i].id.c_str()) + "' value='1'" + (is_ph ? " checked" : "") + "></td></tr>";
    }

    ch += "</tbody></table>"
      "<button type='submit' class='save-btn'>Save Curation to MicroSD</button></form>"
      "</body></html>";
    w.send(200, "text/html; charset=utf-8", ch);
    return;
  }

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
    "var s_yoto_pkce_verifier = '';\n"
    "var s_yoto_redirect_uri = 'http://127.0.0.1:8787/callback';\n"
    "\n"
    "function generateRandomString(len){\n"
    "  var chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~';\n"
    "  var res = '';\n"
    "  var bytes = new Uint8Array(len);\n"
    "  crypto.getRandomValues(bytes);\n"
    "  for(var i=0; i<len; i++) res += chars[bytes[i] % chars.length];\n"
    "  return res;\n"
    "}\n"
    "\n"
    "async function sha256Base64Url(str){\n"
    "  var enc = new TextEncoder().encode(str);\n"
    "  var hash = await crypto.subtle.digest('SHA-256', enc);\n"
    "  var bytes = new Uint8Array(hash);\n"
    "  var binary = '';\n"
    "  for(var i=0; i<bytes.byteLength; i++) binary += String.fromCharCode(bytes[i]);\n"
    "  return btoa(binary).replace(/\\+/g, '-').replace(/\\//g, '_').replace(/=+$/, '');\n"
    "}\n"
    "\n"
    "async function startYotoOAuth(){\n"
    "  try {\n"
    "    var cidIn = document.getElementById('cid');\n"
    "    var cid = cidIn ? cidIn.value.trim() : '';\n"
    "    if(!cid){\n"
    "      alert('Please enter your Yoto Client ID first.');\n"
    "      if(cidIn) cidIn.focus();\n"
    "      return;\n"
    "    }\n"
    "    s_yoto_pkce_verifier = generateRandomString(64);\n"
    "    sessionStorage.setItem('yoto_verifier', s_yoto_pkce_verifier);\n"
    "    sessionStorage.setItem('yoto_cid', cid);\n"
    "    \n"
    "    var challenge = await sha256Base64Url(s_yoto_pkce_verifier);\n"
    "    var state = generateRandomString(16);\n"
    "    var scope = 'family:library:view user:content:view family:devices:view family:devices:control offline_access openid profile';\n"
    "    \n"
    "    var authUrl = 'https://login.yotoplay.com/authorize?' +\n"
    "      'audience=' + encodeURIComponent('https://api.yotoplay.com') +\n"
    "      '&scope=' + encodeURIComponent(scope) +\n"
    "      '&response_type=code' +\n"
    "      '&client_id=' + encodeURIComponent(cid) +\n"
    "      '&code_challenge=' + encodeURIComponent(challenge) +\n"
    "      '&code_challenge_method=S256' +\n"
    "      '&redirect_uri=' + encodeURIComponent(s_yoto_redirect_uri) +\n"
    "      '&state=' + encodeURIComponent(state);\n"
    "      \n"
    "    var pasteBox = document.getElementById('yoto_oauth_paste_box');\n"
    "    if(pasteBox) pasteBox.style.display = 'block';\n"
    "    \n"
    "    var pop = window.open(authUrl, 'yotoAuthPopup', 'width=600,height=750,menubar=no,toolbar=no');\n"
    "    if(!pop || pop.closed || typeof pop.closed === 'undefined'){\n"
    "      window.open(authUrl, '_blank');\n"
    "    }\n"
    "  } catch(err) {\n"
    "    alert('Error starting Yoto sign-in: ' + err.message);\n"
    "  }\n"
    "}\n"
    "\n"
    "function finishYotoOAuth(){\n"
    "  var rawIn = document.getElementById('yoto_callback_url');\n"
    "  var raw = rawIn ? rawIn.value.trim() : '';\n"
    "  if(!raw){\n"
    "    alert('Please paste the callback URL or authorization code.');\n"
    "    return;\n"
    "  }\n"
    "  var code = raw;\n"
    "  if(raw.indexOf('code=') >= 0){\n"
    "    try {\n"
    "      var u = new URL(raw.startsWith('http') ? raw : ('http://localhost/' + raw));\n"
    "      code = u.searchParams.get('code') || raw;\n"
    "    } catch(e){\n"
    "      var m = raw.match(/code=([^&]+)/);\n"
    "      if(m) code = decodeURIComponent(m[1]);\n"
    "    }\n"
    "  }\n"
    "  var verifier = s_yoto_pkce_verifier || sessionStorage.getItem('yoto_verifier') || '';\n"
    "  var cidEl = document.getElementById('cid');\n"
    "  var cid = (cidEl ? cidEl.value.trim() : '') || sessionStorage.getItem('yoto_cid') || '';\n"
    "  var csecEl = document.getElementById('csec');\n"
    "  var csec = csecEl ? csecEl.value.trim() : '';\n"
    "  var b = document.getElementById('yoto_oauth_badge');\n"
    "  if(b){\n"
    "    b.className = 'status-badge badge-wait';\n"
    "    b.innerText = 'Exchanging code for Yoto token on ESP32...';\n"
    "  }\n"
    "  \n"
    "  fetch('/api/yoto/oauth/exchange', {\n"
    "    method: 'POST',\n"
    "    headers: { 'Content-Type': 'application/json' },\n"
    "    body: JSON.stringify({\n"
    "      code: code,\n"
    "      verifier: verifier,\n"
    "      redirect_uri: s_yoto_redirect_uri,\n"
    "      client_id: cid,\n"
    "      client_secret: csec\n"
    "    })\n"
    "  })\n"
    "  .then(r => r.json())\n"
    "  .then(d => {\n"
    "    if(d.ok){\n"
    "      if(b){\n"
    "        b.className = 'status-badge badge-ok';\n"
    "        b.innerText = 'Authenticated! Discovered ' + (d.players ? d.players.length : 0) + ' player(s)';\n"
    "      }\n"
    "      var st = document.getElementById('yoto_status_text');\n"
    "      if(st) st.innerHTML = '<span style=\"color:#137333;font-weight:700;\">&#10004; Authenticated (Ready)</span>';\n"
    "      var yAuth = document.getElementById('yoto_is_authed');\n"
    "      if(yAuth) yAuth.value = '1';\n"
    "      var pasteBox = document.getElementById('yoto_oauth_paste_box');\n"
    "      if(pasteBox) pasteBox.style.display = 'none';\n"
    "      if(d.players && d.players.length > 0){\n"
    "        populatePlayerDropdown(d.players);\n"
    "      }\n"
    "      toggleModeFields();\n"
    "    } else {\n"
    "      if(b){\n"
    "        b.className = 'status-badge badge-err';\n"
    "        b.innerText = d.error || 'Authentication failed';\n"
    "      }\n"
    "    }\n"
    "  })\n"
    "  .catch(e => {\n"
    "    if(b){\n"
    "      b.className = 'status-badge badge-err';\n"
    "      b.innerText = 'Exchange failed: ' + e.message;\n"
    "    }\n"
    "  });\n"
    "}\n"
    "\n"
    "function fetchYotoPlayers(){\n"
    "  var b = document.getElementById('disc_badge');\n"
    "  if(b){ b.className = 'status-badge badge-wait'; b.innerText = 'Fetching Yoto players...'; }\n"
    "  fetch('/api/yoto/players')\n"
    "    .then(r => r.json())\n"
    "    .then(d => {\n"
    "      if(d.ok && d.players){\n"
    "        if(b){\n"
    "          b.className = 'status-badge badge-ok';\n"
    "          b.innerText = 'Discovered ' + d.players.length + ' Yoto player(s)';\n"
    "        }\n"
    "        populatePlayerDropdown(d.players);\n"
    "      } else {\n"
    "        if(b){\n"
    "          b.className = 'status-badge badge-err';\n"
    "          b.innerText = d.error || 'Failed to fetch players';\n"
    "        }\n"
    "      }\n"
    "    })\n"
    "    .catch(e => {\n"
    "      if(b){\n"
    "        b.className = 'status-badge badge-err';\n"
    "        b.innerText = 'Player fetch error: ' + e.message;\n"
    "      }\n"
    "    });\n"
    "}\n"
    "\n"
    "function populatePlayerDropdown(players){\n"
    "  var dsel = document.getElementById('dev_sel');\n"
    "  var devIn = document.getElementById('dev_in');\n"
    "  var curDev = devIn ? devIn.value : '';\n"
    "  if(!dsel) return;\n"
    "  dsel.innerHTML = '<option value=\"\">-- Select Target Player --</option>';\n"
    "  var matched = false;\n"
    "  players.forEach(function(pl){\n"
    "    var isSel = (pl.id === curDev);\n"
    "    if(isSel) matched = true;\n"
    "    dsel.innerHTML += '<option value=\"' + pl.id + '\"' + (isSel ? ' selected' : '') + '>' + pl.name + ' (' + pl.id + ') &bull; ' + (pl.online ? 'Online' : 'Offline') + '</option>';\n"
    "  });\n"
    "  if(!matched && players.length > 0){\n"
    "    dsel.selectedIndex = 1;\n"
    "    if(devIn) devIn.value = players[0].id;\n"
    "  }\n"
    "}\n"
    "\n"
    "function toggleModeFields(){\n"
    "  var mEl = document.getElementById('mode_sel');\n"
    "  var m = mEl ? mEl.value : '';\n"
    "  var isCore = (m === 'core');\n"
    "  var coreSec = document.getElementById('core_sec');\n"
    "  var ySec = document.getElementById('yoto_sec');\n"
    "  var rotSec = document.getElementById('rot_sec');\n"
    "  var profGrp = document.getElementById('prof_group');\n"
    "  var discBtnCore = document.getElementById('disc_btn_core');\n"
    "  var discBtnYoto = document.getElementById('disc_btn_yoto');\n"
    "  var devNotice = document.getElementById('dev_standalone_notice');\n"
    "  var dsel = document.getElementById('dev_sel');\n"
    "\n"
    "  if(coreSec) coreSec.style.display = isCore ? 'block' : 'none';\n"
    "  if(ySec) ySec.style.display = isCore ? 'none' : 'block';\n"
    "  if(rotSec) rotSec.style.display = isCore ? 'none' : 'block';\n"
    "  if(profGrp) profGrp.style.display = isCore ? 'block' : 'none';\n"
    "  if(discBtnCore) discBtnCore.style.display = isCore ? 'inline-block' : 'none';\n"
    "  if(discBtnYoto) discBtnYoto.style.display = isCore ? 'none' : 'inline-block';\n"
    "\n"
    "  if(!isCore){\n"
    "    var yAuthEl = document.getElementById('yoto_is_authed');\n"
    "    var yotoAuthOk = (yAuthEl && yAuthEl.value === '1');\n"
    "    if(devNotice) devNotice.style.display = yotoAuthOk ? 'none' : 'block';\n"
    "    if(!yotoAuthOk && dsel && dsel.options.length <= 1){\n"
    "      dsel.innerHTML = '<option value=\"\">-- Sign in to Yoto above to discover players --</option>';\n"
    "    }\n"
    "  } else {\n"
    "    if(devNotice) devNotice.style.display = 'none';\n"
    "  }\n"
    "}\n"
    "\n"
    "function onProfileSelectChange(){\n"
    "  var sel = document.getElementById('prof_sel');\n"
    "  if(!sel) return;\n"
    "  var val = sel.value;\n"
    "  var profIn = document.getElementById('prof_in');\n"
    "  if(val && profIn){ profIn.value = val; }\n"
    "  var opt = sel.options[sel.selectedIndex];\n"
    "  var defPlayer = opt ? opt.getAttribute('data-player') : '';\n"
    "  if(defPlayer){\n"
    "    var dsel = document.getElementById('dev_sel');\n"
    "    if(dsel) dsel.value = defPlayer;\n"
    "    var devIn = document.getElementById('dev_in');\n"
    "    if(devIn) devIn.value = defPlayer;\n"
    "  }\n"
    "}\n"
    "\n"
    "function onPlayerSelectChange(){\n"
    "  var sel = document.getElementById('dev_sel');\n"
    "  if(!sel) return;\n"
    "  var val = sel.value;\n"
    "  var devIn = document.getElementById('dev_in');\n"
    "  if(val && devIn){\n"
    "    devIn.value = val;\n"
    "  }\n"
    "}\n"
    "\n"
    "function testWifi(){\n"
    "  var sEl = document.getElementById('wf_in');\n"
    "  var pEl = document.getElementById('pass');\n"
    "  var s = sEl ? sEl.value : '';\n"
    "  var p = pEl ? pEl.value : '';\n"
    "  var b = document.getElementById('wifi_badge');\n"
    "  if(b){ b.className = 'status-badge badge-wait'; b.innerText = 'Testing connection...'; }\n"
    "  fetch('/api/ping/wifi?ssid=' + encodeURIComponent(s) + '&pass=' + encodeURIComponent(p))\n"
    "    .then(r => r.json())\n"
    "    .then(d => {\n"
    "      if(b){\n"
    "        b.className = 'status-badge ' + (d.ok ? 'badge-ok' : 'badge-err');\n"
    "        b.innerText = d.ok ? ('Connected (IP: ' + d.ip + ')') : ('Failed: ' + (d.error || 'Check credentials'));\n"
    "      }\n"
    "    })\n"
    "    .catch(e => {\n"
    "      if(b){ b.className = 'status-badge badge-err'; b.innerText = 'Ping failed: ' + e.message; }\n"
    "    });\n"
    "}\n"
    "\n"
    "function testCore(){\n"
    "  var uEl = document.getElementById('curl');\n"
    "  var u = uEl ? uEl.value : '';\n"
    "  var b = document.getElementById('core_badge');\n"
    "  if(b){ b.className = 'status-badge badge-wait'; b.innerText = 'Pinging Core...'; }\n"
    "  fetch('/api/ping/core?url=' + encodeURIComponent(u))\n"
    "    .then(r => r.json())\n"
    "    .then(d => {\n"
    "      if(b){\n"
    "        b.className = 'status-badge ' + (d.ok ? 'badge-ok' : 'badge-err');\n"
    "        b.innerText = d.ok ? 'StoryDeck Core reachable (HTTP 200)' : ('Unreachable: ' + (d.error || 'Check URL/DNS'));\n"
    "      }\n"
    "      if(d.ok){ fetchDiscovery(); }\n"
    "    })\n"
    "    .catch(e => {\n"
    "      if(b){ b.className = 'status-badge badge-err'; b.innerText = 'Ping request failed: ' + e.message; }\n"
    "    });\n"
    "}\n"
    "\n"
    "function fetchDiscovery(){\n"
    "  var uEl = document.getElementById('curl');\n"
    "  var u = uEl ? uEl.value : '';\n"
    "  var b = document.getElementById('disc_badge');\n"
    "  if(b){ b.className = 'status-badge badge-wait'; b.innerText = 'Fetching profiles & players...'; }\n"
    "  fetch('/api/portal/discovery?url=' + encodeURIComponent(u))\n"
    "    .then(r => r.json())\n"
    "    .then(d => {\n"
    "      if(d.ok){\n"
    "        if(b){\n"
    "          b.className = 'status-badge badge-ok';\n"
    "          b.innerText = 'Loaded ' + d.profiles.length + ' profiles, ' + d.players.length + ' players';\n"
    "        }\n"
    "        var psel = document.getElementById('prof_sel');\n"
    "        var curProf = document.getElementById('prof_in') ? document.getElementById('prof_in').value : '';\n"
    "        if(psel){\n"
    "          psel.innerHTML = '<option value=\"\">-- Select Profile or type below --</option>';\n"
    "          d.profiles.forEach(function(p){\n"
    "            var isSel = (p.id === curProf) ? ' selected' : '';\n"
    "            psel.innerHTML += '<option value=\"' + p.id + '\" data-player=\"' + (p.default_player_id || '') + '\"' + isSel + '>' + p.name + ' (' + p.id + ')</option>';\n"
    "          });\n"
    "        }\n"
    "        populatePlayerDropdown(d.players);\n"
    "      } else {\n"
    "        if(b){ b.className = 'status-badge badge-err'; b.innerText = d.error || 'Discovery failed'; }\n"
    "      }\n"
    "    })\n"
    "    .catch(e => {\n"
    "      if(b){ b.className = 'status-badge badge-err'; b.innerText = 'Discovery error: ' + e.message; }\n"
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
    text_row("StoryDeck Core URL", "curl", cfg::core_url, "Hub address (e.g. http://<server-ip>:5055 or http://localhost:5055). Required for Core Managed mode.") +
    "<div><button type='button' class='test-btn' onclick='testCore()'>Test StoryDeck Core URL</button><span id='core_badge'></span></div>" +
    "</div>" +
    "<div id='yoto_sec'>" +
    "<h2>Yoto Cloud Account (Standalone Direct)</h2>" +
    "<p style='color:#7a7060;font-size:0.92em;margin-bottom:12px;'>"
      "StoryDeck Touch connects directly to Yoto Cloud without a Core hub. Register a free developer app at <a href='https://dashboard.yoto.dev' target='_blank' rel='noopener' style='color:#e5352b;font-weight:600;'>dashboard.yoto.dev</a> (Public Client, Callback: <code>http://127.0.0.1:8787/callback</code>) and authenticate below."
    "</p>"
    "<input type='hidden' id='yoto_is_authed' value='" + String(yoto::is_authenticated() ? "1" : "0") + "'>" +
    text_row("Yoto Client ID", "cid", cfg::client_id, "From your Yoto Developer Dashboard application.") +
    secret_row("Client Secret (Optional)", "csec", "Optional: Leave blank for standard Public Client PKCE apps.") +
    "<div style='background:#f4ede0;border-radius:12px;padding:14px;margin-top:12px;border:1px solid #e7e0d2;'>"
    "<div style='display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:10px;'>"
    "<div><div style='font-weight:700;color:#23303a;'>Yoto Cloud Connection Status</div>"
    "<div id='yoto_status_text' style='font-size:0.9em;'>" + (yoto::is_authenticated() ? "<span style='color:#137333;font-weight:700;'>&#10004; Authenticated (Ready)</span>" : "<span style='color:#e5352b;font-weight:600;'>&#9888; Not Authenticated</span>") + "</div></div>"
    "<button type='button' class='test-btn' id='btn_yoto_oauth' style='background:#e5352b;color:#fff;border:0;padding:10px 18px;font-weight:700;border-radius:8px;cursor:pointer;' onclick='startYotoOAuth()'>🔑 Authenticate with Yoto</button>"
    "</div>"
    "<div id='yoto_oauth_paste_box' style='display:none;margin-top:14px;padding-top:14px;border-top:1px dashed #d7cebe;'>"
    "<div style='font-weight:600;font-size:0.92em;color:#23303a;margin-bottom:6px;'>Step 2: Complete Authorization</div>"
    "<div style='font-size:0.88em;color:#5f6368;margin-bottom:8px;'>Sign into Yoto in the opened window. When redirected to <code>http://127.0.0.1:8787/callback?code=...</code>, copy the full URL from your browser address bar and paste it below:</div>"
    "<div style='display:flex;gap:8px;'>"
    "<input type='text' id='yoto_callback_url' placeholder='http://127.0.0.1:8787/callback?code=...' style='margin:0;flex:1;'>"
    "<button type='button' class='test-btn' style='background:#137333;color:#fff;border:0;padding:10px 16px;font-weight:700;border-radius:8px;cursor:pointer;' onclick='finishYotoOAuth()'>Complete Sign-In</button>"
    "</div><div id='yoto_oauth_badge' style='margin-top:8px;'></div></div></div>"
    "</div>" +
    profile_device_picker() +
        "<div id='rot_sec'>" +
    "<h2>Library Rotation &amp; Discovery (Standalone)</h2>" +
    "<label>Catalog Card Limit"
    "<input type='number' name='catlim' id='catlim' min='10' max='300' value='" + String(cfg::catalog_card_limit) + "'>"
    "<small>Maximum cards loaded into grid memory (default: 60). Lower values optimize RAM and scrolling speed.</small></label>" +
    "<label>Daily Rotation Refresh Hour (0-23)"
    "<input type='number' name='rothr' id='rothr' min='0' max='23' value='" + String(cfg::rotation_hour) + "'>"
    "<small>Local hour when discovery rotation re-rolls (default: 3 for 3:00 AM).</small></label>" +
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
  String custom_tz = w.arg("tz_custom");
  custom_tz.trim();
  String chosen_tz = (custom_tz.length() > 0) ? custom_tz : w.arg("tz");
  chosen_tz.trim();
  if(chosen_tz.length() > 0){
    String parsed = parse_tz_input(chosen_tz);
    if(cfg::tz != parsed){
      cfg::tz = parsed;
      configTzTime(cfg::tz.c_str(), "pool.ntp.org", "time.nist.gov");
      log_msg("[NTP] Clock reconfigured with TZ '%s'", cfg::tz.c_str());
    }
  }
  v = w.arg("pin");   if(v.length() == 4) cfg::parent_pin = v;

  // Profile assignment (Core mode only)
  if(cfg::op_mode == "standalone"){
    cfg::assigned_profile = "";
    core::s_assigned_profile = "";
  } else {
    String prof = w.arg("prof_custom");
    if(prof.length() == 0) prof = w.arg("prof");
    prof.trim();
    cfg::assigned_profile = prof;
    core::s_assigned_profile = prof;
  }

  // Target Player assignment: Permanently locked
  String dev = w.arg("devid_custom");
  if(dev.length() == 0) dev = w.arg("devid");
  dev.trim();
  if(dev.length() > 0){
    cfg::device_id = dev;
    cfg::locked_dev_id = dev;
    cfg::lock_player = true;
  }

    v = w.arg("catlim"); if(v.length()){ int n = v.toInt(); if(n >= 10 && n <= 300) cfg::catalog_card_limit = n; }
  v = w.arg("rothr");  if(v.length()){ int h = v.toInt(); if(h >= 0 && h <= 23) cfg::rotation_hour = h; }
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

static void handle_curation_post(WebServer& w){
  if(!auth_ok(w)) return;
  curation::load();
  curation::set_hide_physical(w.hasArg("hide_phys"));

  for(int i = 0; i < s_card_count; i++){
    if(s_cards[i].daily) continue;
    String id = s_cards[i].id.c_str();
    bool pin = w.hasArg("pin_" + id);
    bool phys = w.hasArg("phys_" + id);
    curation::toggle_card_pinned(id, pin);
    curation::toggle_card_physical(id, phys);
  }
  curation::save();
  fetch_library();
  if(s_scr_home) build_grid();

  w.send(200, "text/html; charset=utf-8",
    "<!doctype html><body style='font-family:system-ui;background:#fbf7f0;padding:24px'>"
    "<h2 style='color:#3a6b50'>Curation Saved.</h2>"
    "<p>Card highlights and physical tags saved to MicroSD.</p>"
    "<p><a href='/settings?tab=curation'>Back to Card Curation</a> &middot; <a href='/settings'>Settings</a></p></body>");
}

// Standalone: Exchange OAuth code for Yoto token and discover players
static void handle_yoto_oauth_exchange(WebServer& w){
  if(!auth_ok(w)) return;
  if(w.hasArg("plain")){
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, w.arg("plain"));
    if(err){
      w.send(200, "application/json", "{\"ok\":false,\"error\":\"JSON parse error\"}");
      return;
    }
    String code = (const char*)(doc["code"] | "");
    String verifier = (const char*)(doc["verifier"] | "");
    String redirect_uri = (const char*)(doc["redirect_uri"] | "http://127.0.0.1:8787/callback");
    String client_id = (const char*)(doc["client_id"] | "");
    String client_secret = (const char*)(doc["client_secret"] | "");

    if(code.length() == 0){
      w.send(200, "application/json", "{\"ok\":false,\"error\":\"Authorization code empty\"}");
      return;
    }
    if(client_id.length() == 0) client_id = cfg::client_id;
    if(client_id.length() == 0){
      w.send(200, "application/json", "{\"ok\":false,\"error\":\"Client ID empty\"}");
      return;
    }

    String err_msg;
    bool ok = yoto::exchange_auth_code(code, verifier, redirect_uri, client_id, client_secret, err_msg);
    if(!ok){
      w.send(200, "application/json", "{\"ok\":false,\"error\":\"" + esc(err_msg) + "\"}");
      return;
    }

    // Save client_id to config
    cfg::client_id = client_id;
    if(client_secret.length() > 0) cfg::client_secret = client_secret;
    cfg::save();

    // Discover players from Yoto
    core::discover_standalone_players();

    String out = "{\"ok\":true,\"message\":\"Yoto authenticated successfully\",\"players\":[";
    for(int i = 0; i < core::s_standalone_player_count; i++){
      if(i > 0) out += ",";
      out += "{\"id\":\"" + esc(core::s_standalone_players[i].id) + "\",";
      out += "\"name\":\"" + esc(core::s_standalone_players[i].name) + "\",";
      out += "\"online\":" + String(core::s_standalone_players[i].online ? "true" : "false") + "}";
    }
    out += "]}";
    w.send(200, "application/json", out);
    return;
  }
  w.send(400, "application/json", "{\"ok\":false,\"error\":\"Bad request\"}");
}

// Standalone: Discover Yoto players
static void handle_yoto_players(WebServer& w){
  if(!auth_ok(w)) return;
  if(!yoto::is_authenticated()){
    w.send(200, "application/json", "{\"ok\":false,\"authenticated\":false,\"error\":\"Yoto not authenticated\"}");
    return;
  }
  bool ok = core::discover_standalone_players();
  String out = "{\"ok\":true,\"authenticated\":true,\"players\":[";
  for(int i = 0; i < core::s_standalone_player_count; i++){
    if(i > 0) out += ",";
    out += "{\"id\":\"" + esc(core::s_standalone_players[i].id) + "\",";
    out += "\"name\":\"" + esc(core::s_standalone_players[i].name) + "\",";
    out += "\"online\":" + String(core::s_standalone_players[i].online ? "true" : "false") + "}";
  }
  out += "]}";
  w.send(200, "application/json", out);
}

// Direct callback capture
static void handle_yoto_callback(WebServer& w){
  String code = w.arg("code");
  w.send(200, "text/html; charset=utf-8",
    "<!doctype html><body style='font-family:system-ui;background:#fbf7f0;padding:24px;text-align:center'>"
    "<h2 style='color:#137333'>&#10004; Yoto Authorization Code Received!</h2>"
    "<p>You can close this window and paste this authorization code into your settings page:</p>"
    "<pre style='background:#fff;padding:12px;border-radius:8px;border:1px solid #ccc;font-size:1.1em;'>" + esc(code) + "</pre>"
    "<p><a href='/settings' style='color:#e5352b;font-weight:700;'>Return to Settings</a></p></body>");
}

static void begin(WebServer& w){

  w.on("/settings", HTTP_GET,  [&w](){ handle_get(w);    });
  w.on("/settings", HTTP_POST, [&w](){ handle_post(w);   });
  w.on("/settings/curation", HTTP_POST, [&w](){ handle_curation_post(w); });
  w.on("/reboot",   HTTP_GET,  [&w](){ handle_reboot(w); });
  w.on("/api/ping/wifi", HTTP_GET, [&w](){ handle_ping_wifi(w); });
  w.on("/api/ping/core", HTTP_GET, [&w](){ handle_ping_core(w); });
  w.on("/api/portal/discovery", HTTP_GET, [&w](){ handle_portal_discovery(w); });
  w.on("/api/yoto/oauth/exchange", HTTP_POST, [&w](){ handle_yoto_oauth_exchange(w); });
  w.on("/api/yoto/players", HTTP_GET, [&w](){ handle_yoto_players(w); });
  w.on("/callback", HTTP_GET, [&w](){ handle_yoto_callback(w); });
}

} // namespace portal
