#pragma once
// OTA Manager & Web Portal API Endpoints
#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <ElegantOTA.h>
#include "app_config.h"
#include "core_api.h"
#include "hal_display.h"

extern TaskHandle_t s_flick_task;
extern void flick_stress_task(void*);

// ---------------- OTA: wireless firmware updates ----------------
// Two doors into the same flash-over-WiFi mechanism (both need WiFi up first; USB-C stays
// the recovery path if a bad image ever wedges the app):
//   1. ArduinoOTA — push from the Mac:  pio run -e waveshare-s3-43b-ota -t upload
//   2. ElegantOTA — open http://yoto-controller.local/update (or http://<board-ip>/update)
//      in any browser and drop in .pio/build/waveshare-s3-43b/firmware.bin
// Updates land in the spare app slot and only swap over on success, and they DON'T touch NVS,
// so the Yoto tokens survive. mDNS advertises the board as "yoto-controller.local".
static WebServer s_ota_web(80);

// "Loading new software" splash for firmware updates. The hard constraint: on the ESP32-S3 every
// flash write/erase momentarily DISABLES the MMU cache, and the LCD framebuffer is read from PSRAM
// THROUGH that cache, so the panel literally scans garbage for the whole transfer no matter what's
// in the FB ("screen goes nuts"). We can't hold a live image during the write. So instead: paint
// the message ONCE (this fires before the first flash write, while the cache is still on, so it
// renders clean), hold it briefly so it's readable, then BLANK the backlight (EXIO_BL_DISP) so the
// transfer runs on a clean dark screen instead of garbage. On success the board reboots and setup()
// turns the backlight back on; on failure ota_restore_display() brings it back (see onError/onEnd).
static lv_obj_t* s_scr_ota = nullptr;
static void ota_splash_and_blank(){
  if(!s_scr_ota){
    s_scr_ota = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_ota, lv_color_hex(0xE5352B), 0);   // red hero bg
    lv_obj_clear_flag(s_scr_ota, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* t = lv_label_create(s_scr_ota);
    lv_label_set_text(t, "Loading new software");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -18);

    lv_obj_t* sub = lv_label_create(s_scr_ota);
    lv_label_set_text(sub, "Please wait...");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(0xFFE3E0), 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 28);

    lv_scr_load(s_scr_ota);
  }
  lv_refr_now(lv_disp_get_default());   // clean paint: runs before the first flash write
  delay(1200);                          // hold long enough to read
  ch422g_set(EXIO_BL_DISP, false);      // blank: dark beats garbage during the cache-disabling writes
}
// Recover the display if an OTA FAILS (no reboot happens, loop() resumes — and its 1 Hz backlight
// self-heal would otherwise keep re-asserting the now-OFF shadow bit, leaving the screen dark).
static void ota_restore_display(){
  ch422g_set(EXIO_BL_DISP, true);
  if(s_scr_home) lv_scr_load(s_scr_home);
  lv_refr_now(lv_disp_get_default());
}

static void ota_begin(){
  ArduinoOTA.setHostname("yoto-controller");
  if(sizeof(OTA_PASSWORD) > 1) ArduinoOTA.setPassword(OTA_PASSWORD);   // empty string => no auth
  ArduinoOTA.onStart([](){ Serial.println("[OTA] push start"); ota_splash_and_blank(); });
  ArduinoOTA.onEnd([](){   Serial.println("[OTA] push done");  });   // success -> board reboots
  ArduinoOTA.onError([](ota_error_t e){ Serial.printf("[OTA] error %u\n", e); ota_restore_display(); });
  ArduinoOTA.begin();

  s_ota_web.on("/", [](){ s_ota_web.send(200, "text/plain",
      "Yoto Controller. Wireless firmware update at /update"); });
  // Telemetry for tuning the draw buffer vs internal-RAM headroom. cards>0 confirms the
  // library loaded; minFree is the boot-time low-water mark (worst dip during TLS handshakes).
  s_ota_web.on("/heap", [](){
    char b[240];
    snprintf(b, sizeof(b),
      "{\"drawLines\":%d,\"cards\":%d,\"freeInternal\":%u,\"largestInternal\":%u,\"minFreeInternal\":%u,\"tlsArena\":%u,\"pcCursor\":%d,\"pcDone\":%d}",
      DRAW_LINES, s_card_count,
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
      (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
      (unsigned)tlsarena::s_size, s_pc_cursor, s_pc_done ? 1 : 0);
    s_ota_web.send(200, "application/json", b);
  });
  // Display-flicker probe. GET /flick?stress=SECONDS injects heavy PSRAM bus load for that long
  // (0..120) so we can SEE whether the left-edge static flicker is PSRAM-bandwidth contention.
  // Watch the panel: edge worsens under load + recovers after = RGB-DMA underrun confirmed.
  // No-arg GET just reports the live display config + remaining stress time + free RAM.
  s_ota_web.on("/flick", [](){
    if(!s_flick_task)
      xTaskCreatePinnedToCore(flick_stress_task, "flick", 4096, nullptr, 1, &s_flick_task, 0);
    if(s_ota_web.hasArg("stress")){
      int sec = s_ota_web.arg("stress").toInt();
      if(sec < 0) sec = 0; if(sec > 120) sec = 120;
      s_flick_until = millis() + (uint32_t)sec*1000u;
    }
    int32_t left = (int32_t)(s_flick_until - millis()); if(left < 0) left = 0;
    char b[256];
    snprintf(b, sizeof(b),
      "{\"bouncePx\":%d,\"pclkHz\":%d,\"fbInPsram\":1,\"numFbs\":1,\"drawLines\":%d,"
      "\"stressMsLeft\":%d,\"freeSpiram\":%u,\"largestSpiram\":%u,\"freeInternal\":%u}",
      LCD_H_RES*10, 16*1000*1000, DRAW_LINES, (int)left,
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    s_ota_web.send(200, "application/json", b);
  });
  // SD cache health: is the card mounted, and is the cover/chapter cache actually populating?
  // Lists the first handful of files in /ythumbs with sizes so we can SEE whether detail
  // opens are writing {id}.chap (chapters) and {id}_184.565 (detail cover) blobs.
  s_ota_web.on("/sd", [](){
    String out = "{\"mounted\":" + String(s_sd_ok ? 1 : 0);
    if(s_sd_ok){
      out += ",\"type\":" + String((int)SD.cardType());
      out += ",\"sizeMB\":" + String((uint32_t)(SD.cardSize()/(1024ULL*1024ULL)));
      out += ",\"usedMB\":" + String((uint32_t)(SD.usedBytes()/(1024ULL*1024ULL)));
      int nchap=0, ncover=0, ntot=0;
      String sample = "[";
      File dir = SD.open(SD_DIR);
      if(dir){
        for(File f = dir.openNextFile(); f; f = dir.openNextFile()){
          String nm = f.name();
          if(nm.endsWith(".chap")) nchap++;
          else if(nm.endsWith(".565")) ncover++;
          if(ntot < 8){ if(ntot) sample += ","; sample += "\"" + nm + ":" + String((uint32_t)f.size()) + "\""; }
          ntot++;
          f.close();
        }
        dir.close();
      }
      sample += "]";
      out += ",\"files\":" + String(ntot) + ",\"chap\":" + String(nchap) +
             ",\"cover\":" + String(ncover) + ",\"sample\":" + sample;
    }
    out += "}";
    s_ota_web.send(200, "application/json", out);
  });
  // Live MQTT state probe: connection health, parsed playback snapshot, and the LAST RAW
  // event payload verbatim (the `event` field is the real wire JSON, embedded as-is) so the
  // parser in yoto_mqtt.h can be confirmed/corrected against what the player actually sends.
  // Play a card from the iOS app or the player's buttons, then curl this to see it land.
  s_ota_web.on("/mqtt", [](){
    if(s_ota_web.hasArg("kick")) ymqtt::kick();   // force reconnect+refresh to exercise stack peak
    ymqtt::Live lv; ymqtt::snapshot(lv);
    const char* raw = ymqtt::last_raw();
    String out = "{";
    out += "\"connected\":" + String(ymqtt::connected() ? 1 : 0);
    out += ",\"supHWM\":" + String(ymqtt::sup_hwm());
    out += ",\"events\":"   + String(ymqtt::events());
    out += ",\"errors\":"   + String(ymqtt::errors());
    out += ",\"seq\":"      + String(lv.seq);
    out += ",\"online\":"   + String(lv.online ? 1 : 0);
    out += ",\"cardId\":\"" + String(lv.cardId) + "\"";
    out += ",\"chapterKey\":\"" + String(lv.chapterKey) + "\"";
    out += ",\"status\":"   + String((int)lv.status);   // 0 unknown 1 stopped 2 paused 3 playing
    out += ",\"pos\":"      + String(lv.position);
    out += ",\"dur\":"      + String(lv.trackLength);
    out += ",\"vol\":"      + String(lv.volume);
    out += ",\"ageMs\":"    + String(lv.ts ? (millis() - lv.ts) : 0);
    out += ",\"topic\":\""  + String(ymqtt::last_topic()) + "\"";
    out += ",\"event\":";   out += (raw && raw[0]) ? raw : "null";
    out += "}";
    s_ota_web.send(200, "application/json", out);
  });
  // Screensaver-arming diagnostics: poll this hands-off and watch inactiveMs climb toward
  // saverMin*60000. If it resets with nobody touching the panel, touchSamples/lastTouchMs
  // will show whether the GT911 reported a phantom press (LVGL resets its inactivity clock
  // on every PRESSED sample). Reads shared state from loop() context — same thread as
  // lv_timer_handler, so no race.
  s_ota_web.on("/saver", [](){
    // resetReason (esp_reset_reason_t): 1 POWERON, 3 SW, 4 PANIC, 5 INT_WDT, 6 TASK_WDT,
    // 7 other WDT, 9 BROWNOUT — the key forensic bit for the 2026-06-10 reboot loop
    char b[360];
    snprintf(b, sizeof(b),
      "{\"uptimeMs\":%lu,\"inactiveMs\":%lu,\"saverMin\":%d,\"haveHome\":%d,\"saverShown\":%d,"
      "\"touchSamples\":%lu,\"lastTouchMs\":%lu,\"tstate\":%d,\"resetReason\":%d,"
      "\"blRecoveries\":%lu,\"freeInternal\":%u,\"minFreeInternal\":%u}",
      (unsigned long)millis(), (unsigned long)lv_disp_get_inactive_time(NULL),
      cfg::saver_min, s_scr_home ? 1 : 0, s_scr_saver ? 1 : 0,
      (unsigned long)s_touch_samples, (unsigned long)s_tlast, (int)s_tstate,
      (int)esp_reset_reason(),
      (unsigned long)s_bl_recoveries,
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
      (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    s_ota_web.send(200, "application/json", b);
  });
  // Live probe of the detail-screen fetch paths (authed /card/{id} + CDN cover), since the
  // failures only reproduce on-device. ?card=N picks a grid index; ?expire=1 corrupts the
  // cached access token first so the 403-refresh-retry path runs NOW instead of in an hour.
  // Blocks the UI for a few seconds while it runs — it's a diagnostic, not a feature.
  s_ota_web.on("/diag", [](){
    String out = "{\"freeInt\":" + String(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)) +
                 ",\"largest\":" + String(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)) +
                 ",\"arena\":" + String(tlsarena::s_size);
    size_t hole;
    {
      // what mbedTLS actually sees when the arena opens for a call (no String growth in
      // here — an alloc landing inside the open hole would split it permanently)
      tlsarena::Guard g;
      hole = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    }
    out += ",\"holeLargest\":" + String(hole);
    if(s_card_count == 0){
      out += ",\"err\":\"no cards loaded\"}";
      s_ota_web.send(200, "application/json", out);
      return;
    }
    int i = s_ota_web.hasArg("card") ? s_ota_web.arg("card").toInt() : 0;
    if(i < 0 || i >= s_card_count) i = 0;
    Card& c = s_cards[i];
    out += ",\"id\":\"" + c.id + "\"";
    if(s_ota_web.arg("expire") == "1"){
      yoto::s_access = "expired-on-purpose";   // NOT persisted; next refresh overwrites
      out += ",\"forcedExpiry\":true";
    }
    int apiCode; size_t blen = 0; uint32_t apiMs;
    {
      tlsarena::Guard tg;
      WiFiClientSecure cli; HTTPClient http;
      uint32_t t0 = millis();
      apiCode = yoto::get(cli, http, "/card/" + c.id);
      if(apiCode == 200){ char* b = read_body_psram(http, blen); if(b) free(b); }
      if(apiCode > 0) http.end();
      apiMs = millis() - t0;
    }
    out += ",\"api\":" + String(apiCode) + ",\"apiBytes\":" + String((unsigned)blen) +
           ",\"apiMs\":" + String(apiMs);
    out += ",\"freeIntMid\":" + String(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)) +
           ",\"largestMid\":" + String(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    {
      // Plain-HTTP reachability (any code > 0 = DNS+TCP fine, isolating the failure to TLS)
      WiFiClient pc; HTTPClient ph;
      ph.begin(pc, "http://api.yotoplay.com/");
      ph.setTimeout(8000);
      out += ",\"plainHttp\":" + String(ph.GET());
      ph.end();
    }
    bool tlsOk; char eb[120] = "";
    {
      // Raw TLS connect + the mbedtls error string ("memory allocation failed" vs DNS etc.)
      tlsarena::Guard tg;
      WiFiClientSecure tc; tc.setInsecure();
      tlsOk = tc.connect("api.yotoplay.com", 443);
      if(!tlsOk) tc.lastError(eb, sizeof(eb));
      tc.stop();
    }
    out += ",\"tlsConnect\":" + String(tlsOk ? 1 : 0) + ",\"tlsErr\":\"" + String(eb) + "\"";
    if(s_ota_web.hasArg("status")){
      // Probe GET /device-v2/{id}/status with the board's CURRENT token — settles whether
      // live device state needs any extra scope or works with what we already hold.
      int stCode; String snip;
      {
        tlsarena::Guard tg;
        WiFiClientSecure cli; HTTPClient http;
        stCode = yoto::get(cli, http, "/device-v2/" + cfg::device_id + "/status");
        size_t sl = 0; char* sb = nullptr;
        if(stCode == 200){ sb = read_body_psram(http, sl); }
        if(stCode > 0) http.end();
        if(sb){ snip = String(sb).substring(0, 220); free(sb); }
      }
      snip.replace("\\", ""); snip.replace("\"", "'");   // keep the diag JSON parseable
      out += ",\"status\":" + String(stCode) + ",\"statusBody\":\"" + snip + "\"";
    }
    if(s_ota_web.hasArg("daily")){
      // Reproduce the exact Yoto Daily play path and surface what the firmware throws away:
      // the resolved trackKey AND the card/start response code+body. Settles whether the
      // failure is a bad/absent trackKey or the API rejecting a card/start for 3nC80.
      out += ",\"dailyId\":\"" + String(YOTO_DAILY_ID) + "\"";
      struct tm tmv;
      if(!getLocalTime(&tmv, 500)){
        out += ",\"dailyErr\":\"clock not synced\"";
      } else {
        char today[9]; strftime(today, sizeof today, "%Y%m%d", &tmv);
        out += ",\"today\":\"" + String(today) + "\"";
        String key, title; int dur = 0;
        bool found = fetch_daily_track(String(today), key, title, dur);
        out += ",\"dailyFound\":" + String(found ? 1 : 0) +
               ",\"trackKey\":\"" + key + "\",\"trackDur\":" + String(dur);
        if(found){
          // identical payload to send_cmd("play", {...}) for the daily tile
          String payload = String("{\"uri\":\"https://yoto.io/") + YOTO_DAILY_ID +
                           "\",\"chapterKey\":\"daily\",\"trackKey\":\"" + key + "\"}";
          String path = String("/device-v2/") + cfg::device_id + "/command/card/start";
          String rb; int sc;
          { tlsarena::Guard tg; sc = yoto::post(path, payload, rb); }
          rb.replace("\\", ""); rb.replace("\"", "'");   // keep diag JSON parseable
          out += ",\"startCode\":" + String(sc) + ",\"startBody\":\"" + rb.substring(0, 220) + "\"";
        }
      }
    }
    {
      uint8_t* buf = (uint8_t*)heap_caps_malloc(DCOVER_SZ, MALLOC_CAP_SPIRAM);
      bool ok = false;
      s_thumb_diag = "";
      uint32_t t0 = millis();
      if(buf){ ok = fetch_cover(c.cover, DCOVER_W, DCOVER_H, buf); free(buf); }
      else s_thumb_diag = "psram alloc fail";
      out += ",\"cover\":" + String(ok ? 1 : 0) + ",\"coverDiag\":\"" + s_thumb_diag +
             "\",\"coverMs\":" + String(millis() - t0);
    }
    out += "}";
    s_ota_web.send(200, "application/json", out);
  });
  s_ota_web.on("/bl", [](){
    // Manually re-assert the CH422G outputs (EXIO2 = backlight + DISP enable). If the screen is
    // black and hitting this brings it back, the expander's output latch was cleared by a supply
    // glitch (the ESP rides through the dip, so it never reboots) — not a dead backlight rail.
    // Full re-init (WR_SET + settle + WR_IO) so it recovers even a complete chip reset; the WR_SET
    // write momentarily blips EXIO1 (touch reset), acceptable for a manual one-shot diagnostic.
    // (The periodic keepalive in loop() deliberately does WR_IO only — see the note there.)
    ch422g_write(CH422G_WR_SET, 0x01);
    delay(20);
    ch422g_flush();
    s_ota_web.send(200, "application/json",
      String("{\"reasserted\":1,\"ioShadow\":") + String(s_io_shadow) + "}");
  });
  portal::begin(s_ota_web);   // /settings + /reboot ride the same server (see web_portal.h)
  if(sizeof(OTA_PASSWORD) > 1) ElegantOTA.setAuth("admin", OTA_PASSWORD);
  ElegantOTA.onStart([](){ Serial.println("[OTA] web upload start"); ota_splash_and_blank(); });
  ElegantOTA.onEnd([](bool ok){ Serial.printf("[OTA] web upload %s\n", ok ? "ok" : "FAILED"); if(!ok) ota_restore_display(); });
  ElegantOTA.begin(&s_ota_web);
  s_ota_web.begin();
}

// Bring up time + tokens + library once WiFi is available. Runs at boot when WiFi connects
// right away, and again from the retry timer if it only shows up later (router reboot,
// hotspot reconfiguration, transient outage).
