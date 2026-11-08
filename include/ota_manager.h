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
#include "app_state.h"

extern TaskHandle_t s_flick_task;
extern void flick_stress_task(void*);

// ---------------- OTA: wireless firmware updates ----------------
// Two doors into the same flash-over-WiFi mechanism (both need WiFi up first; USB-C stays
// the recovery path if a bad image ever wedges the app):
//   1. ArduinoOTA — push from the Mac:  pio run -e waveshare-s3-43b-ota -t upload
//   2. ElegantOTA — open http://storydeck-touch.local/update (or http://<board-ip>/update)
//      in any browser and drop in .pio/build/waveshare-s3-43b/firmware.bin
// Updates land in the spare app slot and only swap over on success, and they DON'T touch NVS,
// so the Yoto tokens survive. mDNS advertises the board as "storydeck-touch.local".
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
  ArduinoOTA.setHostname("storydeck-touch");
  ArduinoOTA.setTimeout(10000);   // Increase receive timeout from 1000ms default to 10000ms to eliminate Receive Failed errors
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
      LCD_H_RES*20, 12*1000*1000, DRAW_LINES, (int)left,
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
    out += String(",\"id\":\"") + c.id.c_str() + "\"";
    if(s_ota_web.arg("expire") == "1"){
      yoto::s_access = "expired-on-purpose";   // NOT persisted; next refresh overwrites
      out += ",\"forcedExpiry\":true";
    }
    int apiCode; size_t blen = 0; uint32_t apiMs;
    {
      tlsarena::Guard tg;
      WiFiClientSecure cli; HTTPClient http;
      uint32_t t0 = millis();
      apiCode = yoto::get(cli, http, String("/card/") + c.id.c_str());
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
      if(buf){ ok = fetch_cover(c.cover.c_str(), DCOVER_W, DCOVER_H, buf); free(buf); }
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
  // Live 1:1 800x480 RGB565 Framebuffer Viewer & Touch Injector (/screen, /screen.raw, /touch)
  s_ota_web.on("/screen.raw", HTTP_GET, [](){
    const uint16_t* fb = hal_get_framebuffer();
    if(!fb){
      s_ota_web.send(503, "text/plain", "Framebuffer unavailable");
      return;
    }
    WiFiClient client = s_ota_web.client();
    size_t total_bytes = (size_t)LCD_H_RES * (size_t)LCD_V_RES * 2;
    String hdr = "HTTP/1.1 200 OK\r\n"
                 "Content-Type: application/octet-stream\r\n"
                 "Access-Control-Allow-Origin: *\r\n"
                 "Cache-Control: no-store\r\n"
                 "Content-Length: " + String((unsigned)total_bytes) + "\r\n"
                 "Connection: close\r\n\r\n";
    client.print(hdr);
    const uint8_t* ptr = (const uint8_t*)fb;
    size_t rem = total_bytes;
    while(rem > 0 && client.connected()){
      size_t chunk = (rem > 16384) ? 16384 : rem;
      size_t written = client.write(ptr, chunk);
      if(written == 0) break;
      ptr += written;
      rem -= written;
    }
  });

  s_ota_web.on("/touch", [](){
    int x = s_ota_web.hasArg("x") ? s_ota_web.arg("x").toInt() : 0;
    int y = s_ota_web.hasArg("y") ? s_ota_web.arg("y").toInt() : 0;
    bool down = s_ota_web.hasArg("down") ? (s_ota_web.arg("down").toInt() != 0) : false;
    hal_inject_web_touch(x, y, down);
    s_ota_web.sendHeader("Access-Control-Allow-Origin", "*");
    s_ota_web.send(200, "application/json", "{\"ok\":true}");
  });

  s_ota_web.on("/screen", HTTP_GET, [](){
    static const char html[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>StoryDeck Touch — 800×480 Live LVGL Mirror</title>
<style>
  body { margin:0; background:#141010; color:#FBF7F0; font-family:system-ui,-apple-system,sans-serif; display:flex; flex-direction:column; align-items:center; justify-content:center; min-height:100vh; user-select:none; }
  .bar { display:flex; gap:14px; align-items:center; margin-bottom:10px; font-size:13px; color:#A89F94; }
  .badge { background:#E5352B; color:#fff; font-weight:700; padding:3px 10px; border-radius:99px; font-size:12px; }
  .frame { position:relative; border:4px solid #2D2624; border-radius:16px; overflow:hidden; box-shadow:0 16px 48px rgba(0,0,0,0.6); background:#000; width:800px; height:480px; }
  canvas { display:block; width:800px; height:480px; cursor:pointer; touch-action:none; }
  a { color:#FF8A65; text-decoration:none; }
</style>
</head>
<body>
  <div class="bar">
    <span class="badge">LIVE LVGL 800×480</span>
    <span id="st">Connecting...</span>
    <span>• Click / Drag to interact</span>
    <span>• <a href="/settings" target="_blank">Web Settings</a></span>
  </div>
  <div class="frame">
    <canvas id="cv" width="800" height="480"></canvas>
  </div>
<script>
const cv = document.getElementById('cv');
const ctx = cv.getContext('2d');
const st = document.getElementById('st');
const img = ctx.createImageData(800, 480);
const rgba = img.data;
let busy = false, isDown = false;

async function pullFrame(){
  if(busy) return;
  busy = true;
  const t0 = performance.now();
  try {
    const r = await fetch('/screen.raw?t=' + Date.now(), {cache:'no-store'});
    if(r.ok){
      const buf = await r.arrayBuffer();
      const u16 = new Uint16Array(buf);
      for(let i=0, j=0; i<u16.length; i++, j+=4){
        const p = u16[i];
        const r5 = (p >> 11) & 0x1F;
        const g6 = (p >> 5)  & 0x3F;
        const b5 =  p        & 0x1F;
        rgba[j]   = (r5 << 3) | (r5 >> 2);
        rgba[j+1] = (g6 << 2) | (g6 >> 4);
        rgba[j+2] = (b5 << 3) | (b5 >> 2);
        rgba[j+3] = 255;
      }
      ctx.putImageData(img, 0, 0);
      st.textContent = Math.round(performance.now() - t0) + ' ms';
    }
  } catch(e){
    st.textContent = 'Retrying...';
  }
  busy = false;
}
setInterval(pullFrame, 220);
pullFrame();

function coords(e){
  const rect = cv.getBoundingClientRect();
  const cx = (e.touches && e.touches[0]) ? e.touches[0].clientX : e.clientX;
  const cy = (e.touches && e.touches[0]) ? e.touches[0].clientY : e.clientY;
  return {
    x: Math.max(0, Math.min(799, Math.round((cx - rect.left) * 800 / rect.width))),
    y: Math.max(0, Math.min(479, Math.round((cy - rect.top)  * 480 / rect.height)))
  };
}
function sendTouch(x, y, down){
  fetch(`/touch?x=${x}&y=${y}&down=${down?1:0}`, {cache:'no-store'}).then(()=>{ setTimeout(pullFrame, 60); }).catch(()=>{});
}
cv.addEventListener('pointerdown', e => { isDown = true; cv.setPointerCapture(e.pointerId); const p = coords(e); sendTouch(p.x, p.y, true); });
cv.addEventListener('pointermove', e => { if(!isDown) return; const p = coords(e); sendTouch(p.x, p.y, true); });
cv.addEventListener('pointerup',   e => { if(!isDown) return; isDown = false; const p = coords(e); sendTouch(p.x, p.y, false); });
cv.addEventListener('pointercancel', e => { isDown = false; sendTouch(0, 0, false); });
</script>
</body>
</html>)rawliteral";
    s_ota_web.send_P(200, "text/html", html);
  });

    // Live debug logger: /api/logs (JSON) & /logs (HTML live stream)
  s_ota_web.on("/api/logs", HTTP_GET, [](){
    time_t now_epoch = time(nullptr);
    uint32_t now_ms = millis();
    bool ntp_synced = (now_epoch > 100000000);

    String json;
    json.reserve(256 + s_log_total * 140);
    json = "{\"total\":" + String(s_log_total) + ",\"synced\":" + (ntp_synced ? "true" : "false") + ",\"entries\":[";
    int start = (s_log_total < MAX_LOG_ENTRIES) ? 0 : s_log_head;
    for(int i = 0; i < s_log_total; i++){
      int idx = (start + i) % MAX_LOG_ENTRIES;
      if(i > 0) json += ",";

      time_t ent_epoch = s_log_ring[idx].epoch;
      if(ntp_synced && (ent_epoch <= 100000000)){
        int32_t diff_sec = (int32_t)((now_ms - s_log_ring[idx].ms) / 1000);
        ent_epoch = (now_epoch > diff_sec) ? (now_epoch - diff_sec) : 0;
      }

      char time_str[32] = "";
      if(ent_epoch > 100000000){
        struct tm tm_val;
        localtime_r(&ent_epoch, &tm_val);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_val);
      }

      json += "{\"t\":" + String(s_log_ring[idx].ms) +
              ",\"epoch\":" + String((unsigned long)ent_epoch) +
              ",\"time\":\"" + String(time_str) +
              "\",\"msg\":\"";
      for(const char* p = s_log_ring[idx].msg; *p; p++){
        if(*p == '"') json += "\\\"";
        else if(*p == '\\') json += "\\\\";
        else if(*p == '\n') json += "\\n";
        else if(*p == '\r') continue;
        else json += *p;
      }
      json += "\"}";
    }
    json += "]}";
    s_ota_web.send(200, "application/json", json);
  });

  s_ota_web.on("/logs", HTTP_GET, [](){
    static const char log_html[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>StoryDeck Touch — Live Console Logs</title>
<style>
  body { margin:0; padding:18px; background:#121214; color:#E4E4E7; font-family:ui-monospace,SFMono-Regular,Consolas,monospace; font-size:13px; line-height:1.5; }
  .header { display:flex; justify-content:space-between; align-items:center; border-bottom:1px solid #27272A; padding-bottom:12px; margin-bottom:14px; }
  .title { font-size:16px; font-weight:700; color:#E5352B; display:flex; align-items:center; gap:8px; }
  .badge { background:#27272A; color:#A1A1AA; font-size:11px; padding:2px 8px; border-radius:4px; font-weight:600; }
  .log-box { display:flex; flex-direction:column; gap:4px; }
  .entry { display:flex; gap:12px; border-bottom:1px solid rgba(255,255,255,0.03); padding:4px 0; }
  .time { color:#A1A1AA; font-weight:600; min-width:155px; flex-shrink:0; letter-spacing:-0.2px; }
  .msg { color:#E4E4E7; word-break:break-all; }
  .tag-core { color:#38BDF8; font-weight:700; }
  .tag-sd { color:#FBBF24; font-weight:700; }
  .tag-cache { color:#34D399; font-weight:700; }
  .tag-err { color:#F87171; font-weight:700; }
  .controls { display:flex; gap:10px; align-items:center; }
  button { background:#E5352B; color:#fff; border:none; padding:4px 12px; border-radius:6px; cursor:pointer; font-weight:600; font-family:inherit; }
  button:hover { background:#DC2626; }
</style>
</head>
<body>
  <div class="header">
    <div class="title">⚡ StoryDeck Touch Backend Console <span class="badge" id="count">0 msgs</span></div>
    <div class="controls">
      <label><input type="checkbox" id="autoScroll" checked> Auto-scroll</label>
      <button onclick="fetchLogs()">Refresh</button>
      <a href="/screen" target="_blank" style="color:#A1A1AA; text-decoration:none; font-size:12px;">📺 Screen Mirror</a>
      <a href="/settings" target="_blank" style="color:#A1A1AA; text-decoration:none; font-size:12px;">⚙️ Settings</a>
    </div>
  </div>
  <div class="log-box" id="logBox">Loading backend logs...</div>
<script>
async function fetchLogs(){
  try {
    const res = await fetch('/api/logs?t=' + Date.now());
    if(!res.ok) return;
    const data = await res.json();
    document.getElementById('count').textContent = data.entries.length + ' entries' + (data.synced ? ' (NTP synced)' : '');
    const box = document.getElementById('logBox');
    box.innerHTML = '';
    data.entries.forEach(e => {
      const row = document.createElement('div');
      row.className = 'entry';
      const upSec = (e.t / 1000).toFixed(2) + 's';
      let timeHtml = '';
      if(e.time && e.time.length > 0) {
        timeHtml = `<span class="time" title="Uptime: ${upSec} | Epoch: ${e.epoch}">${e.time}</span>`;
      } else {
        timeHtml = `<span class="time" title="NTP pending (boot +${upSec})">+${upSec}</span>`;
      }
      let formatted = e.msg
        .replace(/\[CORE\]/g, '<span class="tag-core">[CORE]</span>')
        .replace(/\[SD\]/g, '<span class="tag-sd">[SD]</span>')
        .replace(/\[CACHE\]/g, '<span class="tag-cache">[CACHE]</span>')
        .replace(/\[WATCHDOG\]/g, '<span class="tag-cache">[WATCHDOG]</span>')
        .replace(/\[USER\]/g, '<span class="tag-core">[USER]</span>')
        .replace(/\[PLAY\]/g, '<span class="tag-core">[PLAY]</span>')
        .replace(/\[CTRL\]/g, '<span class="tag-core">[CTRL]</span>')
        .replace(/\[WIFI\]/g, '<span class="tag-sd">[WIFI]</span>')
        .replace(/\[NTP\]/g, '<span class="tag-cache">[NTP]</span>')
        .replace(/\[ERR\]|fail|error/gi, '<span class="tag-err">$&</span>');
      row.innerHTML = `${timeHtml}<span class="msg">${formatted}</span>`;
      box.appendChild(row);
    });
    if(document.getElementById('autoScroll').checked){
      window.scrollTo(0, document.body.scrollHeight);
    }
  } catch(err){}
}
setInterval(fetchLogs, 1500);
fetchLogs();
</script>
</body>
</html>)rawliteral";
    s_ota_web.send_P(200, "text/html", log_html);
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
