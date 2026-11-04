// StoryDeck Touch & Standalone Yoto Wall Controller
// Dual-mode firmware:
//   - Core Mode: pairs with StoryDeck Core Hub (/api/devices/register, child profiles, ABS + Yoto catalog, remote play/control)
//   - Standalone Mode: talks TLS straight to api.yotoplay.com + MQTT live-state feed
// Cover art is decoded (PNG/JPEG) to RGB565 in PSRAM and cached to MicroSD.
// Waveshare ESP32-S3-Touch-LCD-4.3B · arduino-esp32 3.3.9 · LVGL 8.4 · ArduinoJson 7.

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <ElegantOTA.h>
#include <ArduinoJson.h>
#include <lvgl.h>
#include <time.h>
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include <pngle.h>
#include <JPEGDEC.h>
#include <mbedtls/platform.h>

// Give Arduino loopTask 16KB stack (default 8KB is tight for LVGL + HTTPClient + JSON)
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// mbedTLS allocator: route blocks >= 512B to 8MB PSRAM so internal SRAM stays free for WiFi/DMA
static void* tls_psram_calloc(size_t n, size_t size){
  size_t total = n * size;
  uint32_t pref = (total >= 512) ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL;
  void* p = heap_caps_calloc(n, size, pref);
  if(!p) p = heap_caps_calloc(n, size, pref == MALLOC_CAP_SPIRAM ? MALLOC_CAP_INTERNAL : MALLOC_CAP_SPIRAM);
  return p;
}

// Core & Hardware Modules
#include "secrets.h"
#include "app_config.h"
#include "yoto_api.h"
#include "core_api.h"
#include "yoto_mqtt.h"
#include "web_portal.h"
#include "hal_display.h"

// Application State, Storage & Decoders
#include "app_state.h"
#include "storage_sd.h"
#include "cover_decoder.h"
#include "catalog_loader.h"

// UI Screens & Playback Controller
#include "ui_screensaver.h"
#include "ui_catalog.h"
#include "ui_detail.h"
#include "ui_now_playing.h"
#include "playback_ctrl.h"
#include "ui_grownups.h"
#include "ui_profile_picker.h"
#include "ui_setup_pairing.h"
#include "ota_manager.h"

void net_init_and_fetch(){
  if(!s_net_ready){
    configTzTime(cfg::tz.c_str(), "pool.ntp.org", "time.nist.gov");   // screensaver clock
    if(cfg::op_mode != "core"){
      yoto::refresh();                        // mint a fresh access token before the first call
    }
    s_net_ready = true;
  }

  // Core mode registration & approval gate
  if(cfg::op_mode == "core" && cfg::core_url.length() > 0){
    if(!core::s_is_approved){
      status("Checking StoryDeck Core registration...");
      String err;
      bool reg_ok = core::register_device(cfg::core_url, err);
      if(!reg_ok){
        Serial.printf("[CORE] Registration status: %s (registered=%d approved=%d)\n", err.c_str(), (int)core::s_is_registered, (int)core::s_is_approved);
        // Show dedicated Pairing Screen and start 10s poll timer
        show_pairing_screen(cfg::core_url, err);
        if(!s_pairing_timer){
          s_pairing_timer = lv_timer_create(pairing_poll_cb, 10000, NULL);
        }
        return; // STOP loop right here! Do not proceed to catalog fetch or error screen
      }
    }
  }

  // If approved and pairing screen is visible, dismiss it
  if(s_scr_pairing){
    lv_obj_del_async(s_scr_pairing);
    s_scr_pairing = nullptr;
  }
  if(s_pairing_timer){
    lv_timer_del(s_pairing_timer);
    s_pairing_timer = nullptr;
  }

  if(!s_lib_ok){
    if(cfg::op_mode == "core" && cfg::core_url.length() > 0){
      status("Loading StoryDeck Profiles...");
      if(core::fetch_config_and_profiles(cfg::core_url)){
        cfg::device_id = core::resolve_active_device_id(core::s_active_profile_idx);
      }
    } else if(cfg::op_mode != "core"){
      // Standalone mode: discover all Yoto players on the account via GET /device-v2/devices/mine
      status("Discovering Yoto Players...");
      size_t blen = 0; char* body = nullptr;
      {
        tlsarena::Guard tg;
        WiFiClientSecure cli; HTTPClient http;
        int code = yoto::get(cli, http, "/device-v2/devices/mine");
        if(code == 200) body = read_body_psram(http, blen);
        if(code > 0) http.end();
      }
      if(body){
        PsramAllocator psram; JsonDocument doc(&psram);
        if(!deserializeJson(doc, (const char*)body, blen)){
          core::s_standalone_player_count = 0;
          for(JsonObject d : doc["devices"].as<JsonArray>()){
            if(core::s_standalone_player_count >= MAX_PROF_PLAYERS) break;
            String did = (const char*)(d["deviceId"] | d["id"] | "");
            if(did.length() == 0) continue;
            core::PlayerInfo& pi = core::s_standalone_players[core::s_standalone_player_count++];
            pi.id      = did;
            pi.name    = (const char*)(d["name"] | "Yoto Player");
            pi.online  = (bool)(d["online"] | false);
            pi.is_home = pi.online;
          }
        }
        free(body);
      }
      cfg::device_id = core::resolve_active_device_id(-1);
    }

    Serial.printf("[HEAP] after-config: int=%u largest=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    // Kiosk Profile Permissions:
    // - If Core mode is active, profile switching is allowed, no specific profile was force-assigned to this hardware,
    //   and multiple profiles exist -> open the Profile Picker immediately!
    // - If a profile was force-assigned to this kiosk in Core Admin (or only 1 profile exists, or Standalone mode) ->
    //   boot directly into the catalog!
    if(cfg::op_mode == "core" && core::s_profile_count > 1 && core::s_allow_profile_switch && core::s_assigned_profile.length() == 0){
      build_grid();       // initialize empty s_scr_home backing screen
      s_lib_ok = true;
      open_profile_picker();
    } else {
      status("Fetching library...");
      bool lib_fetched = false;
      if(cfg::op_mode == "core"){
        if(cfg::core_url.length() > 0){
          lib_fetched = fetch_library_core();
          if(!lib_fetched){
            show_error("Core Catalog Failed",
                       "Could not load media catalog from StoryDeck Core.",
                       "Check Core server health and network link in Settings.");
          }
        } else {
          show_error("Core URL Needed",
                     "StoryDeck Core Managed mode is active, but Core URL is empty.",
                     "Open Settings below or visit http://<ip>/settings to set Core URL.");
        }
      } else {
        lib_fetched = fetch_library();
      }

      if(lib_fetched){
        Serial.printf("[HEAP] after-catalog: int=%u largest=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        build_grid();
        lv_scr_load(s_scr_home);
        lv_refr_now(lv_disp_get_default());
        load_thumbs();
        s_lib_ok = true;
      } else if(cfg::op_mode == "core" && core::s_profile_count > 0){
        build_grid();
        lv_scr_load(s_scr_home);
      }
    }
  }
  // Live device state over MQTT — start once the library is in (so event cardIds resolve to
  // grid tiles) and a token exists. begin() self-guards against a second start.
  if(s_lib_ok && cfg::op_mode != "core") ymqtt::begin(cfg::device_id);
  if(s_lib_ok && (WiFi.getMode() & WIFI_MODE_AP)){
    s_hotspot_wanted = false;
    WiFi.softAPdisconnect(true);            // drop the setup hotspot once we're properly online
    WiFi.mode(WIFI_STA);
  }
}
static void net_retry_cb(lv_timer_t* t){
  if(s_lib_ok){ lv_timer_del(t); return; }
  if(s_safe_mode) return;                   // wait for a manual Retry
  if(WiFi.status() == WL_CONNECTED) net_init_and_fetch();
}

void setup(){
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);
#endif
  crash_guard_init();
  // Route mbedTLS's big allocations (the two ~17KB SSL buffers per connection) to PSRAM.
  // Internal RAM fragments once the UI is up and can no longer host them — every runtime
  // HTTPS call died with "SSL - Memory allocation failed" (chapters, covers, commands).
  // Small hot allocations (bignum math during the handshake) stay in internal RAM for speed.
  mbedtls_platform_set_calloc_free(tls_psram_calloc, free);
  cfg::load();                              // NVS-backed settings (secrets.h = defaults)

  // Start WiFi early while internal SRAM is completely unfragmented
  WiFi.mode(WIFI_STA);
  {
    String hname = cfg::kiosk_name;
    hname.replace(" ", "-");
    if(hname.length() > 0) WiFi.setHostname(hname.c_str());
    else WiFi.setHostname("yoto-controller");
  }
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);                     // no modem-sleep: pm_dream wake re-enables the PHY and aborts if internal RAM is empty
  WiFi.begin(cfg::wifi_ssid.c_str(), cfg::wifi_pass.c_str());

  // hardware: backlight + panel + touch
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000); delay(10);
  ch422g_write(CH422G_WR_SET, 0x01);
  s_io_shadow=(1u<<EXIO_BL_DISP)|(1u<<EXIO_TP_RST); ch422g_flush(); delay(20);
  ch422g_set(EXIO_LCD_RST,true); delay(120);
  rgb_panel_init();
  gt_init();

  // LVGL
  lv_init();
  size_t bufpx = LCD_H_RES*DRAW_LINES;
  s_buf1=(lv_color_t*)heap_caps_malloc(bufpx*sizeof(lv_color_t), MALLOC_CAP_INTERNAL);
  if(!s_buf1) s_buf1=(lv_color_t*)heap_caps_malloc(bufpx*sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  lv_disp_draw_buf_init(&s_draw_buf, s_buf1, NULL, bufpx);
  static lv_disp_drv_t dd; lv_disp_drv_init(&dd);
  dd.hor_res=LCD_H_RES; dd.ver_res=LCD_V_RES; dd.flush_cb=flush_cb; dd.draw_buf=&s_draw_buf;
  lv_disp_drv_register(&dd);
  static lv_indev_drv_t id; lv_indev_drv_init(&id);
  id.type=LV_INDEV_TYPE_POINTER; id.read_cb=touch_cb; lv_indev_drv_register(&id);
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0xFBF7F0), 0);

  // MicroSD (cover cache) — best-effort. A write/read self-test proves the wiring; on failure
  // the app still runs, covers just fetch live each boot as before.
  if(sd_begin()){
    uint32_t mb = (uint32_t)(SD.cardSize() / (1024ULL * 1024ULL));
    const char* type = SD.cardType()==CARD_SDHC ? "SDHC" : SD.cardType()==CARD_SD ? "SD" : SD.cardType()==CARD_MMC ? "MMC" : "?";
    bool rw = false;                                   // round-trip a marker file
    File f = SD.open(SD_DIR "/.selftest", FILE_WRITE);
    if(f){ rw = (f.print("ok") == 2); f.close(); SD.remove(SD_DIR "/.selftest"); }
    Serial.printf("SD: mounted %s %u MB, used %llu MB, RW=%s\n",
                  type, mb, SD.usedBytes()/(1024ULL*1024ULL), rw ? "yes" : "NO");
    status((String("SD ") + type + "  " + mb + " MB  " + (rw ? "(cache ready)" : "(read-only?)")).c_str());
  } else {
    Serial.println("SD: not mounted — covers will fetch live each boot");
    status("SD not found - covers load over WiFi");
  }
  delay(700);                                          // let the self-test line be visible

  // WiFi — never dead-end here: if we can't join, raise a setup hotspot carrying the web
  // portal and keep retrying STA in the background. OTA + portal start either way, so the
  // board is ALWAYS wirelessly reachable (the old code returned on failure, which made a
  // wrong WiFi password recoverable only over USB at the wall).
  status("Connecting to WiFi...");
  uint32_t t0=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-t0 < 20000){ delay(200); }

  if(cfg::op_mode != "core"){
    yoto::begin();                          // load tokens from NVS (standalone direct Yoto cloud)
  }
  lv_timer_create(tick_cb, 1000, NULL);     // 1 Hz heartbeat: progress/sleep timer/screensaver

#ifdef WOKWI_SIMULATION
  // In Wokwi simulation, start the web server BEFORE net_init_and_fetch() so http://localhost:8180/screen
  // and /settings are immediately live while connecting to StoryDeck Core.
  ota_begin();
  if(WiFi.status() == WL_CONNECTED && cfg::op_mode == "core"){
    if(!core::ping(cfg::core_url)){
      if(core::ping("http://192.168.1.11:5065")) cfg::core_url = "http://192.168.1.11:5065";
      else if(core::ping("http://host.wokwi.internal:5055")) cfg::core_url = "http://host.wokwi.internal:5055";
      else if(core::ping("http://host.wokwi.internal:5065")) cfg::core_url = "http://host.wokwi.internal:5065";
    }
  }
#endif

  if(s_safe_mode){
    start_hotspot();
    show_error("Safe Mode",
               (String("The controller crashed ") + (unsigned)s_crash_count + " times in a row while starting, so automatic loading was skipped.").c_str(),
               "Open Settings to check WiFi / Core URL, then tap Retry. OTA updates still work.");
  } else if(WiFi.status()==WL_CONNECTED){
    status((String("WiFi OK: ") + WiFi.localIP().toString() + "\nFetching library...").c_str());
    net_init_and_fetch();
    // (on fetch failure, status() already shows the reason; the retry timer keeps trying)
  } else {
    start_hotspot();
    WiFi.begin(cfg::wifi_ssid.c_str(), cfg::wifi_pass.c_str());
    show_error("WiFi Connection Needed",
               "Unable to join the configured WiFi network (2.4GHz only).\nSetup hotspot 'Yoto-Setup' is active.",
               "Join WiFi 'Yoto-Setup' on phone/PC or tap 'Open Settings' below.");
  }
  lv_timer_create(net_retry_cb, 30000, NULL);   // self-deletes once the library is in

#ifndef WOKWI_SIMULATION
  // Start OTA LAST on physical hardware: the TLS handshakes to Yoto (refresh/library/covers above) are
  // internal-RAM hungry, and mDNS + the web server compete for that same RAM. Bringing
  // them up after the heavy boot fetch keeps the handshake headroom intact. OTA still
  // starts here even if the library failed, so a bad build is always recoverable wirelessly.
  ota_begin();
#endif
}

void loop(){
  if(!s_crash_cleared && millis() > 60000){ s_crash_cleared = true; s_crash_count = 0; }
  ArduinoOTA.handle();        // cheap polls when idle; block only during an actual update
  s_ota_web.handleClient();
  ElegantOTA.loop();
  lv_timer_handler();
  if(s_thumbs_loading && heap_caps_get_free_size(MALLOC_CAP_INTERNAL) > 14000){
    thumb_load_step();
  }

  // Periodic live player status sync in Core mode (every 8s when covers are done & user isn't mid-touch)
  static uint32_t s_core_sync_t = 0;
  if(s_lib_ok && !s_thumbs_loading && cfg::op_mode == "core" && cfg::core_url.length() > 0 &&
     millis() - s_core_sync_t > 8000 && lv_disp_get_inactive_time(NULL) > 1500 &&
     heap_caps_get_free_size(MALLOC_CAP_INTERNAL) > 16000){
    s_core_sync_t = millis();
    int saved_prof = core::s_active_profile_idx;
    String saved_dev = cfg::device_id;
    bool was_playing = s_playing;
    int prev_card = s_play_card;
    String prev_title = s_play_title;
    String prev_ch = s_play_chapter;
    if(core::fetch_config_and_profiles(cfg::core_url)){
      core::s_active_profile_idx = saved_prof;
      if(cfg::lock_player && cfg::locked_dev_id.length() > 0) cfg::device_id = cfg::locked_dev_id;
      else if(saved_dev.length() > 0) cfg::device_id = saved_dev;
      sync_active_player_playback();
      if(s_playing != was_playing || s_play_card != prev_card || s_play_title != prev_title || s_play_chapter != prev_ch){
        update_home_bar();
      }
      if(s_scr_now && lv_scr_act() == s_scr_now){
        np_set_chapter_label(); np_set_playicon(); np_update_progress();
      }
    }
  }

  // Warm the library cache during idle: only on the home screen or screensaver, and only after
  // a short lull, so the ~1s blocking fetch lands when nobody's mid-tap. Runs after
  // lv_timer_handler so any pending async screen deletes are already settled. Throttled to one
  // fetch every ~2.5s so internal RAM (shared with the concurrent MQTT TLS session) recovers
  // between handshakes instead of being held at the knife's edge by back-to-back fetches.
  static uint32_t s_pc_t = 0;
  if(s_lib_ok && s_sd_ok && !s_pc_done && millis() - s_pc_t > 2500){
    lv_obj_t* act = lv_scr_act();
    // Safety valve: only start a warming fetch with comfortable internal-RAM headroom. A TLS
    // handshake transiently consumes most of free internal RAM; if MQTT (or anything) already has
    // it tight, skip this round and let it recover rather than stacking a fetch on top and dipping
    // to a near-zero low-water. At-rest free is ~24-28KB, so a 16KB floor passes normally but
    // backs off during contention. (Rechecked next loop; this defers warming, never aborts it.)
    bool ram_ok = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) > 16000;
    if(ram_ok && (act == s_scr_home || act == s_scr_saver) && lv_disp_get_inactive_time(NULL) > 1500){
      precache_step();
      s_pc_t = millis();
    }
  }
  // Backlight/DISP self-heal. A dip from the 12V supply can disturb the CH422G I2C expander —
  // EXIO2 (EXIO_BL_DISP) drives the backlight + panel DISP enable, EXIO1 (EXIO_TP_RST) the GT911
  // reset — while the higher-headroom ESP rides through and never reboots, so the screen goes
  // black with no reset. Two failure modes seen on-device:
  //   (1) output LATCH cleared  -> a bare WR_IO re-write (ch422g_flush) restores it. Touch-safe,
  //       so we do it every second while healthy.
  //   (2) full CHIP reset (output-enable config lost) -> WR_IO alone does nothing; the chip needs
  //       WR_SET re-issued. But WR_SET momentarily drops EVERY expander line (backlight blink +
  //       touch reset + panel reset), so it must NOT run on a blind timer — that was what killed
  //       touch when the keepalive re-issued it every second. Instead we DETECT it: a reset holds
  //       EXIO1 low, so the GT911 stops ACKing on I2C. On two consecutive missed probes we run the
  //       full re-init (WR_SET + shadow + gt_init for the just-reset touch chip). The RGB panel
  //       self-recovers once LCD_RST is driven high again (proven by the /bl endpoint).
  // NOTE: this recovers AFTER each brownout; it does not stop the rail from dipping. The real fix
  // is an adequate/regulated 12V supply (or USB-C 5V).
  static uint32_t s_bl_t = 0;
  static uint8_t  s_bl_fail = 0;
  if(millis() - s_bl_t > 1000){
    s_bl_t = millis();
    uint8_t id[4];
    if(gt_read(0x8140, id, 4)){          // GT911 reachable -> expander config intact
      s_bl_fail = 0;
      ch422g_flush();                    // refresh the output latch (covers a latch-only BL drop)
    } else if(++s_bl_fail >= 2){         // two consecutive misses -> the expander fully reset
      s_bl_fail = 0;
      ch422g_write(CH422G_WR_SET, 0x01); delay(20);                    // re-enable push-pull outputs
      s_io_shadow = (1u<<EXIO_BL_DISP)|(1u<<EXIO_TP_RST)|(1u<<EXIO_LCD_RST);
      ch422g_flush(); delay(20);                                       // backlight/DISP + resets high
      gt_init();                                                       // re-init the just-reset GT911
      s_bl_recoveries++;
    }
  }
  delay(5);
}
