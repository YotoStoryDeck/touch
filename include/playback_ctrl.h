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

// ---------------- asynchronous playback command queue ----------------
struct TransportCmd {
  char name[16];
  int  volume;
  bool is_volume;
};

static QueueHandle_t s_cmd_queue = nullptr;
static TaskHandle_t  s_cmd_task  = nullptr;

static void transport_worker_task(void* param){
  TransportCmd cmd;
  while(true){
    if(xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) == pdTRUE){
      // Drain consecutive pending volume commands to coalesce rapid slider scrubs
      while(cmd.is_volume && uxQueueMessagesWaiting(s_cmd_queue) > 0){
        TransportCmd next_cmd;
        if(xQueuePeek(s_cmd_queue, &next_cmd, 0) == pdTRUE && next_cmd.is_volume){
          xQueueReceive(s_cmd_queue, &cmd, 0);
        } else {
          break;
        }
      }

      if(cfg::device_id.length() == 0){
        cfg::device_id = core::resolve_active_device_id(core::s_active_profile_idx);
      }

      if(cfg::op_mode == "core" && cfg::core_url.length() > 0){
        if(cmd.is_volume){
          core::player_control(cfg::core_url, cfg::device_id, "volume", cmd.volume);
        } else {
          core::player_control(cfg::core_url, cfg::device_id, cmd.name);
        }
      } else {
        // Standalone Yoto Cloud mode
        String ycmd, ypayload;
        if(cmd.is_volume){
          ycmd = "volume/set";
          JsonDocument p; p["volume"] = cmd.volume; serializeJson(p, ypayload);
        } else if(strcmp(cmd.name, "pause") == 0 || strcmp(cmd.name, "resume") == 0 || strcmp(cmd.name, "stop") == 0){
          ycmd = String("card/") + cmd.name;
        }
        if(ycmd.length() > 0){
          String path = String("/device-v2/") + cfg::device_id + "/command/" + ycmd;
          String out;
          yoto::post(path, ypayload.length() ? ypayload : String("{}"), out);
        }
      }
    }
  }
}

inline void init_transport_queue(){
  if(!s_cmd_queue){
    s_cmd_queue = xQueueCreate(8, sizeof(TransportCmd));
    xTaskCreatePinnedToCore(transport_worker_task, "transport_cmd", 4096, nullptr, 3, &s_cmd_task, 0);
  }
}

static void queue_cmd(const char* name, int volume = -1){
  init_transport_queue();
  TransportCmd cmd;
  memset(&cmd, 0, sizeof(cmd));
  strncpy(cmd.name, name, sizeof(cmd.name) - 1);
  if(volume >= 0){
    cmd.is_volume = true;
    cmd.volume = volume;
  } else {
    cmd.is_volume = false;
    cmd.volume = -1;
  }
  xQueueSend(s_cmd_queue, &cmd, 0);
}

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
      int chIdx = -1;
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
      int vmax = get_active_player_vol_max();
      int v = (int)(in["volume"] | 30); if(v < 0) v = 0; if(v > vmax) v = vmax;
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
      if(cfg::op_mode != "core" && s_cur_card == s_play_card && s_play_ch >= 0 && s_play_ch < s_ch_count-1){
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
  String tk = s_ch[k].track_key.length() ? s_ch[k].track_key : s_ch[k].key;
  send_cmd("play", String("{\"id\":\"")+s_cards[s_cur_card].id+"\",\"chapterIdx\":"+k+",\"chapterKey\":\""+s_ch[k].key+"\",\"trackKey\":\""+tk+"\"}");
}
static void np_toggle(){
  s_playing = !s_playing;
  np_set_playicon(); update_home_bar();
  lv_refr_now(lv_disp_get_default());
  queue_cmd(s_playing ? "resume" : "pause");
}
void np_next(){ if(s_cur_ch < s_ch_count-1) np_goto_chapter(s_cur_ch+1); }
void np_prev(){ if(s_cur_ch > 0)           np_goto_chapter(s_cur_ch-1); }
void np_volume(int v){
  int vmax = get_active_player_vol_max();
  if(v < 0) v = 0; if(v > vmax) v = vmax;
  s_vol = v;
  queue_cmd("volume", v);
}

static lv_obj_t* s_away_modal = nullptr;
static int s_pending_away_ch = 0;
static void play_from_confirmed(int chapterIdx);

static void show_away_confirm_modal(const String& player_name, int chapterIdx){
  if(s_away_modal){ lv_obj_del(s_away_modal); s_away_modal = nullptr; }
  s_pending_away_ch = chapterIdx;
  lv_obj_t* act = lv_scr_act();
  s_away_modal = lv_obj_create(act);
  lv_obj_set_size(s_away_modal, LCD_H_RES, LCD_V_RES);
  lv_obj_set_pos(s_away_modal, 0, 0);
  lv_obj_set_style_bg_color(s_away_modal, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(s_away_modal, LV_OPA_50, 0);
  lv_obj_set_style_border_width(s_away_modal, 0, 0);
  lv_obj_clear_flag(s_away_modal, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* box = lv_obj_create(s_away_modal);
  lv_obj_set_size(box, 480, 230);
  lv_obj_center(box);
  lv_obj_set_style_bg_color(box, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_radius(box, 18, 0);
  lv_obj_set_style_border_width(box, 2, 0);
  lv_obj_set_style_border_color(box, lv_color_hex(0xEA580C), 0);
  lv_obj_set_style_pad_all(box, 20, 0);
  lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* t = lv_label_create(box);
  lv_label_set_text(t, "Player is Away (Traveling)");
  lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(t, lv_color_hex(0xEA580C), 0);

  lv_obj_t* m = lv_label_create(box);
  lv_label_set_text_fmt(m, "%s is currently away from home.\nAre you sure you want to start audio on this remote player?", player_name.c_str());
  lv_obj_set_width(m, 430);
  lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(m, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(m, lv_color_hex(0x23303A), 0);

  lv_obj_t* r = lv_obj_create(box);
  lv_obj_set_size(r, 420, 56);
  lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(r, 0, 0);
  lv_obj_set_style_pad_all(r, 0, 0);
  lv_obj_set_style_pad_column(r, 18, 0);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* bc = lv_btn_create(r);
  lv_obj_set_size(bc, 170, 48);
  lv_obj_set_style_radius(bc, 14, 0);
  lv_obj_set_style_bg_color(bc, lv_color_hex(0xF3ECE0), 0);
  lv_obj_add_event_cb(bc, [](lv_event_t*){
    if(s_away_modal){ lv_obj_del_async(s_away_modal); s_away_modal = nullptr; }
  }, LV_EVENT_CLICKED, NULL);
  lv_obj_t* bcl = lv_label_create(bc);
  lv_label_set_text(bcl, "Cancel");
  lv_obj_set_style_text_font(bcl, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(bcl, lv_color_hex(0x23303A), 0);
  lv_obj_center(bcl);

  lv_obj_t* bp = lv_btn_create(r);
  lv_obj_set_size(bp, 190, 48);
  lv_obj_set_style_radius(bp, 14, 0);
  lv_obj_set_style_bg_color(bp, lv_color_hex(0xEA580C), 0);
  lv_obj_add_event_cb(bp, [](lv_event_t*){
    int ch = s_pending_away_ch;
    if(s_away_modal){ lv_obj_del(s_away_modal); s_away_modal = nullptr; }
    play_from_confirmed(ch);
  }, LV_EVENT_CLICKED, NULL);
  lv_obj_t* bpl = lv_label_create(bp);
  lv_label_set_text(bpl, "Play Anyway");
  lv_obj_set_style_text_font(bpl, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(bpl, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(bpl);
}

static void play_from(int chapterIdx){
  if(s_cur_card < 0) return;
  // Away / Traveling Player Guard (Kiosk initiatePlayback parity: Block vs Confirm Prompt)
  const core::PlayerInfo* target_pi = core::find_player_info(cfg::device_id);
  if(target_pi && target_pi->online && !target_pi->is_home){
    if(cfg::block_remote){
      Serial.printf("[PLAY] Blocked playback to away player %s (block_remote=1)\n", cfg::device_id.c_str());
      if(s_detail_play_lbl){
        lv_label_set_text(s_detail_play_lbl, "Blocked: Player Away");
        lv_refr_now(lv_disp_get_default());
      }
      return;
    } else {
      show_away_confirm_modal(target_pi->name, chapterIdx);
      return;
    }
  }
  play_from_confirmed(chapterIdx);
}

static void play_from_confirmed(int chapterIdx){
  if(s_cur_card < 0) return;
  if(s_ch_count == 0){
    s_ch_count = 1;
    s_ch[0].key = "01";
    s_ch[0].track_key = "01";
    s_ch[0].title = s_cards[s_cur_card].title.length() ? s_cards[s_cur_card].title : "Full Audiobook";
    s_ch[0].dur = 1800;
  }

  // Kiosk initiatePlayback() parity: show "Starting..." feedback and confirm dispatch before switching to Now Playing
  if(s_detail_play_lbl){
    lv_label_set_text(s_detail_play_lbl, "Starting...");
    lv_refr_now(lv_disp_get_default());
  }

  String payload;
  if(chapterIdx < 0){
    // Resume mode ('Play Book' button): omit chapterKey/trackKey and pass chapterIdx = -1 so Core/Bridge resumes saved progress
    payload = String("{\"id\":\"") + s_cards[s_cur_card].id + "\",\"chapterIdx\":-1}";
  } else {
    if(chapterIdx >= s_ch_count) chapterIdx = 0;
    String tk = s_ch[chapterIdx].track_key.length() ? s_ch[chapterIdx].track_key : s_ch[chapterIdx].key;
    payload = String("{\"id\":\"") + s_cards[s_cur_card].id
            + "\",\"chapterIdx\":" + chapterIdx
            + ",\"chapterKey\":\"" + s_ch[chapterIdx].key
            + "\",\"trackKey\":\"" + tk + "\"}";
  }
  int code = send_cmd("play", payload);

  if(code >= 200 && code < 300){
    arm_sleep_timer_if_needed();
    // Do NOT autoresolve the book/chapter. Wait for the player to send an authoritative update.
    s_playing = true;
    s_pos = 0;
    s_play_card = s_cur_card;
    s_play_title = s_detail_title.length() ? s_detail_title : s_cards[s_cur_card].title;
    s_play_author = s_cards[s_cur_card].author;
    s_play_chapter = "Connecting to Player...";
    update_home_bar();
    open_now();
    lv_refr_now(lv_disp_get_default());
    if(s_scr_now) confetti(s_scr_now);

    // Immediately trigger a status sync so the player's true track/chapter arrives ASAP
    if(cfg::op_mode == "core" && cfg::core_url.length() > 0){
      core::fetch_config_and_profiles(cfg::core_url, false);
      sync_active_player_playback();
      np_set_chapter_label(); np_set_playicon(); np_update_progress();
      update_home_bar();
    }
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
