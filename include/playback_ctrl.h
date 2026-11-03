#pragma once
// Playback Command Dispatch (Core Hub + Standalone Yoto Cloud), Transport Controls,
// MQTT Live-State Synchronization, and 1 Hz System Heartbeat
#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <lvgl.h>
#include <time.h>
#include "app_state.h"
#include "core_api.h"
#include "yoto_api.h"
#include "yoto_mqtt.h"
#include "catalog_loader.h"
#include "ui_screensaver.h"
#include "ui_now_playing.h"

// ---------------- now-playing screen + playback ----------------
static int send_cmd(const char* name, const String& body){
  JsonDocument in;
  deserializeJson(in, body);
  String nm(name);

  // Ensure target device ID is resolved before dispatching any command
  if(cfg::device_id.length() == 0){
    cfg::device_id = core::resolve_active_device_id(core::s_active_profile_idx);
  }

  // If running in Core Managed mode and Core URL is set, dispatch through StoryDeck Core
  if(cfg::op_mode == "core" && cfg::core_url.length() > 0){
    if(nm == "play"){
      const char* cid = in["id"];
      const char* ck  = in["chapterKey"];
      const char* tk  = in["trackKey"];
      int chIdx = 0;
      if(!in["chapterIdx"].isNull()){
        chIdx = (int)in["chapterIdx"];
      } else if(ck && *ck && atoi(ck) > 0){
        chIdx = atoi(ck) - 1;
      }
      String child_id = (core::s_profile_count > 0 && core::s_active_profile_idx < core::s_profile_count)
                        ? core::s_profiles[core::s_active_profile_idx].id : String("default");
      int card_ref = (s_cur_card >= 0 && s_cur_card < s_card_count) ? s_cur_card : s_play_card;
      String ctype = (card_ref >= 0 && card_ref < s_card_count && s_cards[card_ref].source.length() > 0)
                     ? s_cards[card_ref].source : String("yoto");
      if(ctype.startsWith("yoto")) ctype = "yoto";
      return core::play_media(cfg::core_url, child_id, ctype, cid ? cid : "", cfg::device_id, chIdx,
                              ck ? String(ck) : String(""), tk ? String(tk) : String(""));
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
    auto pad_key = [](const char* k) -> String {
      if(!k || !*k) return String("01");
      String s(k);
      if(s.length() == 1 && s[0] >= '1' && s[0] <= '9') return String("0") + s;
      return s;
    };
    const char* ck = in["chapterKey"];
    const char* tk = in["trackKey"];
    String pck = pad_key(ck);
    String ptk = (tk && *tk) ? pad_key(tk) : pck;
    p["chapterKey"] = pck;
    p["trackKey"]   = ptk;
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
    // Kiosk Sleep Timer Safety Gates:
    // 1. Never stop an away/traveling player
    const core::PlayerInfo* pi = core::find_player_info(cfg::device_id);
    if(pi && pi->online && !pi->is_home){
      s_sleep_sec = 0;
    } else if(!cfg::sleep_bedtime || is_bedtime_now()){
      s_sleep_sec--;
      if(s_sleep_sec == 0){
        if(s_playing && s_local_session_active){
          s_playing = false;
          s_local_session_active = false;
          np_set_playicon();
          update_home_bar();
          send_cmd("stop", "{}");
        }
      }
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
static void arm_sleep_timer_if_needed(){
  s_local_session_active = true;
  if(cfg::sleep_min > 0 && s_sleep_sec == 0 && (!cfg::sleep_bedtime || is_bedtime_now())){
    s_sleep_sec = cfg::sleep_min * 60;
  }
}

static void np_goto_chapter(int k){
  if(k<0 || k>=s_ch_count) return;
  arm_sleep_timer_if_needed();
  s_cur_ch=k; s_pos=0; s_playing=true;
  s_play_card=s_cur_card; s_play_ch=k;                 // keep the mini-player in sync
  s_play_author=s_cards[s_cur_card].author;
  s_play_chapter=s_ch[k].title; s_play_dur=s_ch[k].dur;
  np_set_chapter_label(); np_set_playicon(); np_update_progress(); update_home_bar();
  lv_refr_now(lv_disp_get_default());
  send_cmd("play", String("{\"id\":\"")+s_cards[s_cur_card].id+"\",\"chapterIdx\":"+k+",\"chapterKey\":\""+s_ch[k].key+"\"}");
}
static void np_toggle(){
  s_playing = !s_playing;
  np_set_playicon(); update_home_bar();
  lv_refr_now(lv_disp_get_default());
  send_cmd(s_playing ? "resume" : "pause", "{}");
}
void np_next(){ if(s_cur_ch < s_ch_count-1) np_goto_chapter(s_cur_ch+1); }
void np_prev(){ if(s_cur_ch > 0)           np_goto_chapter(s_cur_ch-1); }
void np_volume(int v){ s_vol=v; send_cmd("volume", String("{\"volume\":")+v+"}"); }

static void play_from(int chapterIdx){
  if(s_cur_card < 0 || chapterIdx < 0) return;
  if(s_ch_count == 0){
    s_ch_count = 1;
    s_ch[0].key = "01";
    s_ch[0].title = s_cards[s_cur_card].title.length() ? s_cards[s_cur_card].title : "Full Audiobook";
    s_ch[0].dur = 1800;
  }
  if(chapterIdx >= s_ch_count) chapterIdx = 0;

  // Away / Traveling Player Guard (cfg::block_remote parity with Kiosk initiatePlayback)
  const core::PlayerInfo* target_pi = core::find_player_info(cfg::device_id);
  if(target_pi && target_pi->online && !target_pi->is_home && cfg::block_remote){
    Serial.printf("[PLAY] Blocked playback to away player %s (block_remote=1)\n", cfg::device_id.c_str());
    if(s_detail_play_lbl){
      lv_label_set_text(s_detail_play_lbl, "Blocked: Player is Away");
      lv_refr_now(lv_disp_get_default());
    }
    return;
  }

  // Kiosk initiatePlayback() parity: show "Starting..." feedback and confirm dispatch before switching to Now Playing
  if(s_detail_play_lbl){
    lv_label_set_text(s_detail_play_lbl, "Starting...");
    lv_refr_now(lv_disp_get_default());
  }

  String payload = String("{\"id\":\"") + s_cards[s_cur_card].id
                 + "\",\"chapterIdx\":" + chapterIdx
                 + ",\"chapterKey\":\"" + s_ch[chapterIdx].key + "\"}";
  int code = send_cmd("play", payload);

  if(code >= 200 && code < 300){
    arm_sleep_timer_if_needed();
    s_cur_ch = chapterIdx; s_pos = 0; s_playing = true;
    s_play_card = s_cur_card; s_play_ch = chapterIdx;
    s_play_title = s_detail_title.length() ? s_detail_title : s_cards[s_cur_card].title;
    s_play_author = s_cards[s_cur_card].author;
    s_play_chapter = s_ch[chapterIdx].title;
    s_play_dur = s_ch[chapterIdx].dur > 0 ? s_ch[chapterIdx].dur : 1800;
    update_home_bar();
    open_now();
    lv_refr_now(lv_disp_get_default());
    if(s_scr_now) confetti(s_scr_now);
  } else {
    Serial.printf("[PLAY] Failed to start playback on player %s (HTTP %d)\n", cfg::device_id.c_str(), code);
    if(s_detail_play_lbl){
      lv_label_set_text_fmt(s_detail_play_lbl, "Play Error (%d)", code);
      lv_refr_now(lv_disp_get_default());
    }
  }
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
