#pragma once
// Cover art decoding engine for YotoStoryDeck
// PNG (via pngle) and JPEG (via JPEGDEC) decoders to RGB565 with PSRAM caching
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <pngle.h>
#include <JPEGDEC.h>
#include <lvgl.h>
#include "tls_arena.h"
#include "storage_sd.h"

// ---- cover art: fetch a CDN-resized PNG (no auth) + decode to RGB565 on the MCU ----
// Yoto's image CDN resizes server-side via ?width=&quality= (only PNG out), so we ask for the
// exact tile size (~5-20KB) and stream it through pngle straight into the panel's pixel buffer.
// Decode target: a W×H RGB565 tile. xoff/yoff center a smaller (e.g. square podcast) image
// so it sits letterboxed in the tile instead of filling from the corner.
struct DecCtx { uint8_t* buf; int W; int H; int xoff; int yoff; };
static String s_thumb_diag;     // why the last cover decode failed (surfaced on-screen, no serial)
static bool   s_png_done;       // set by pngle's done-callback when IEND is reached

static inline void put_px(DecCtx* c, int px, int py, uint16_t v){
  if(px < 0 || px >= c->W || py < 0 || py >= c->H) return;
  size_t idx = ((size_t)py * c->W + px) * 2;
  c->buf[idx] = v & 0xFF; c->buf[idx+1] = v >> 8;     // little-endian RGB565
}
static void png_on_init(pngle_t* p, uint32_t iw, uint32_t ih){
  DecCtx* c = (DecCtx*)pngle_get_user_data(p);
  c->xoff = ((int)c->W - (int)iw) / 2; if(c->xoff < 0) c->xoff = 0;
  c->yoff = ((int)c->H - (int)ih) / 2; if(c->yoff < 0) c->yoff = 0;
}
static void png_on_draw(pngle_t* p, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t rgba[4]){
  DecCtx* c = (DecCtx*)pngle_get_user_data(p);
  uint16_t v = ((rgba[0] & 0xF8) << 8) | ((rgba[1] & 0xFC) << 3) | (rgba[2] >> 3);
  for(uint32_t yy=0; yy<h; yy++)
    for(uint32_t xx=0; xx<w; xx++)
      put_px(c, c->xoff + (int)(x+xx), c->yoff + (int)(y+yy), v);
}
static void png_on_done(pngle_t*){ s_png_done = true; }
static int jpg_draw(JPEGDRAW* d){
  DecCtx* c = (DecCtx*)d->pUser;
  uint16_t* px = (uint16_t*)d->pPixels;        // RGB565 little-endian (set below)
  for(int yy=0; yy<d->iHeight; yy++)
    for(int xx=0; xx<d->iWidth; xx++)
      put_px(c, c->xoff + d->x + xx, c->yoff + d->y + yy, px[yy*d->iWidth + xx]);
  return 1;
}

// Fetch coverBase resized to width W and decode into buf (W*H*2 RGB565). Handles BOTH formats
// Yoto serves: tall PNG book covers AND square JPEG podcast covers (sniffed by magic bytes).
static bool fetch_cover(const String& coverBase, int W, int H, uint8_t* buf){
  if(coverBase.length() == 0){ s_thumb_diag = "no cover url"; return false; }
  String url = coverBase;
  if(url.indexOf("/api/thumb") >= 0){
    // StoryDeck Core /api/thumb preserves ?type=yoto&url=... or ?type=abs&id=...; append &size=W
    // Check /api/thumb BEFORE yotoplay.com because Yoto cards pass yotoplay.com inside the url= query param!
    if(url.indexOf("size=") < 0){
      url += (url.indexOf('?') >= 0 ? "&size=" : "?size=") + String(W);
    }
  } else if(url.indexOf("/api/avatar") >= 0){
    // StoryDeck Core /api/avatar/<id> supports ?size=W for exact 130x130 baseline JPEG decoding
    if(url.indexOf("size=") < 0){
      url += (url.indexOf('?') >= 0 ? "&size=" : "?size=") + String(W);
    }
  } else if(url.indexOf("yotoplay.com") >= 0){
    int q = url.indexOf('?'); if(q >= 0) url = url.substring(0, q);
    url = url + "?width=" + W + "&quality=70";
  }
  uint8_t* img = nullptr;
  size_t got = 0;
  {
    tlsarena::Guard tg;                     // CDN fetch is TLS too; closed before the decode
    WiFiClientSecure scli; scli.setInsecure();
    WiFiClient pcli;
    HTTPClient http;
    bool begin_ok = url.startsWith("https://") ? http.begin(scli, url) : http.begin(pcli, url);
    if(!begin_ok){ s_thumb_diag = "begin fail heap=" + String(ESP.getFreeHeap()); return false; }
    bool is_core = (url.indexOf("/api/thumb") >= 0);
    http.setTimeout(is_core ? 5000 : 3500);
    int code = http.GET();
    if(code != 200){ s_thumb_diag = "HTTP " + String(code) + " heap=" + String(ESP.getFreeHeap()); http.end(); return false; }
    int len = http.getSize();
    WiFiClient* stream = http.getStreamPtr();
    // Buffer the whole (small, ~5-20KB) image, then dispatch by format — JPEG isn't streamable
    // the way pngle is, and buffering keeps both decoders simple.
    size_t cap = (len > 0) ? (size_t)len : 49152;
    img = (uint8_t*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if(!img){ s_thumb_diag = "img malloc fail heap=" + String(ESP.getFreeHeap()); http.end(); return false; }
    uint32_t t0 = millis();
    while(len < 0 || got < (size_t)len){
      size_t avail = stream->available();
      if(avail){
        if(got + avail > cap){                // grow only for unknown-length (chunked) responses
          size_t ncap = cap * 2; uint8_t* n = (uint8_t*)heap_caps_realloc(img, ncap, MALLOC_CAP_SPIRAM);
          if(!n) break; img = n; cap = ncap;
        }
        int n = stream->readBytes(img + got, min(avail, cap - got));
        got += n; t0 = millis();
      } else if(!http.connected()){ break; }
      else if(millis()-t0 > 4000){ break; }
      else delay(1);
    }
    http.end();
  }
  if(got < 8){ s_thumb_diag = "short (" + String(got) + "B)"; free(img); return false; }

  memset(buf, 0, (size_t)W * H * 2);          // black letterbox behind non-filling covers
  DecCtx ctx{ buf, W, H, 0, 0 };
  bool ok = false;
  if(img[0]==0x89 && img[1]=='P'){            // ---- PNG ----
    pngle_t* p = pngle_new();
    if(!p){ s_thumb_diag = "pngle null heap=" + String(ESP.getFreeHeap()); free(img); return false; }
    pngle_set_user_data(p, &ctx);
    pngle_set_init_callback(p, png_on_init);
    pngle_set_draw_callback(p, png_on_draw);
    pngle_set_done_callback(p, png_on_done);
    s_png_done = false;
    if(pngle_feed(p, img, got) < 0) s_thumb_diag = String("png: ") + pngle_error(p);
    ok = s_png_done;
    pngle_destroy(p);
  } else if(img[0]==0xFF && img[1]==0xD8){    // ---- JPEG (covers & 160x160 profile avatars) ----
    static JPEGDEC jpg;
    if(jpg.openRAM(img, got, jpg_draw)){
      jpg.setPixelType(RGB565_LITTLE_ENDIAN);
      int iw = jpg.getWidth(), ih = jpg.getHeight();
      if((iw > W || ih > H) && iw <= 512 && ih <= 512){
        // Decode full iw x ih image into temporary PSRAM buffer and bilinear/nearest scale into W x H buf!
        // This guarantees 160x160 /api/avatar/<id> JPEGs scale cleanly into 130x130 circular bubbles without clipping!
        uint8_t* tmp_fb = (uint8_t*)heap_caps_malloc((size_t)iw * ih * 2, MALLOC_CAP_SPIRAM);
        if(tmp_fb){
          DecCtx full_ctx{ tmp_fb, iw, ih, 0, 0 };
          jpg.setUserPointer(&full_ctx);
          ok = (jpg.decode(0, 0, 0) == 1);
          jpg.close();
          if(ok){
            uint16_t* src16 = (uint16_t*)tmp_fb;
            uint16_t* dst16 = (uint16_t*)buf;
            for(int dy = 0; dy < H; dy++){
              int sy = (dy * ih + ih / 2) / H;
              if(sy >= ih) sy = ih - 1;
              for(int dx = 0; dx < W; dx++){
                int sx = (dx * iw + iw / 2) / W;
                if(sx >= iw) sx = iw - 1;
                dst16[dy * W + dx] = src16[sy * iw + sx];
              }
            }
          }
          free(tmp_fb);
        } else {
          ctx.xoff = (W - iw) / 2;
          ctx.yoff = (H - ih) / 2;
          jpg.setUserPointer(&ctx);
          ok = (jpg.decode(0, 0, 0) == 1);
          jpg.close();
        }
      } else {
        ctx.xoff = (W - iw) / 2; if(ctx.xoff < 0) ctx.xoff = 0;
        ctx.yoff = (H - ih) / 2; if(ctx.yoff < 0) ctx.yoff = 0;
        jpg.setUserPointer(&ctx);
        ok = (jpg.decode(0, 0, 0) == 1);
        jpg.close();
      }
      if(!ok) s_thumb_diag = "jpg decode fail";
    } else s_thumb_diag = "jpg open fail";
  } else {
    s_thumb_diag = "unknown fmt " + String(img[0], HEX) + String(img[1], HEX);
  }
  free(img);
  return ok;
}

// Load a W×H RGB565 cover into buf: SD cache first (instant, no WiFi/decode), else fetch from
// the CDN and write-through to SD so the next boot is offline-fast. With no SD mounted this
// is exactly the plain fetch path. Keyed by (cardId,width) so grid/detail/now-playing each
// get their own cached size.
static bool load_cover_cached(const String& id, const String& coverUrl, int W, int H, size_t sz, uint8_t* buf){
  if(sd_cache_read(id, W, buf, sz, coverUrl)) return true;
  bool ok = false;
  int attempts = (coverUrl.indexOf("/api/thumb") >= 0) ? 1 : 2;
  for(int a = 0; a < attempts && !ok; a++){
    ok = fetch_cover(coverUrl, W, H, buf);
    if(!ok && a + 1 < attempts) delay(100);
  }
  if(ok) sd_cache_write(id, W, buf, sz, coverUrl);
  return ok;
}

static void set_img(lv_img_dsc_t& dsc, lv_obj_t* obj, uint8_t* data, int w, int h, size_t sz){
  dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  dsc.header.always_zero = 0;
  dsc.header.w = w; dsc.header.h = h;
  dsc.data_size = sz; dsc.data = data;
  lv_img_set_src(obj, &dsc);
}

