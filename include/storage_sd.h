#pragma once
// MicroSD (SPI) -- persistent cover-thumbnail and chapter cache
// The 4.3B's TF slot is SPI on GPIO12/13/11, BUT SD_CS hangs off the CH422G (EXIO4).
#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include "hal_display.h"
#include "app_state.h"

#ifdef WOKWI_SIMULATION
// In Wokwi simulation, FSPI (11/12/13) drives the ILI9341 LCD, so the virtual wokwi-microsd-card
// uses HSPI on valid ESP32-S3 GPIOs (SCK=18, MISO=17, MOSI=16, CS=6) for zero SPI bus contention.
// Note: GPIO 23 does not exist on ESP32-S3 and causes a StoreProhibited panic in s_sdspi.begin()!
#define SD_PIN_SCK      18
#define SD_PIN_MISO     17
#define SD_PIN_MOSI     16
#define SD_PIN_CS_DUMMY 6
#else
#define SD_PIN_SCK      12
#define SD_PIN_MISO     13
#define SD_PIN_MOSI     11
#define SD_PIN_CS_DUMMY 6
#endif
#define SD_DIR          "/ythumbs"

inline bool& get_sd_ok() {
  static bool s_sd_ok = false;
  return s_sd_ok;
}
#define s_sd_ok (get_sd_ok())

static SPIClass s_sdspi(HSPI);

inline bool sd_begin(){
  ch422g_set(EXIO_SD_CS, false);
  s_sdspi.begin(SD_PIN_SCK, SD_PIN_MISO, SD_PIN_MOSI, SD_PIN_CS_DUMMY);
  if(!SD.begin(SD_PIN_CS_DUMMY, s_sdspi, 20000000)) return false;
  if(SD.cardType() == CARD_NONE) return false;
  if(!SD.exists(SD_DIR)) SD.mkdir(SD_DIR);
  s_sd_ok = true;
  return true;
}

// FNV-1a 32-bit hash of a cover URL (including ?v= / ?t= version tags) so SD cache detects changed covers
inline uint32_t sd_cover_url_hash(const String& url){
  if(url.length() == 0) return 0;
  uint32_t h = 2166136261u;
  for(size_t i = 0; i < url.length(); i++){
    h ^= (uint8_t)url[i];
    h *= 16777619u;
  }
  return h ? h : 1u;
}

inline String sd_cache_path(const String& id, int W){
  return String(SD_DIR) + "/" + id + "_" + W + ".565";
}

inline size_t sd_expected_cover_size(int W){
  if(W == 160) return 81280;   // THUMB_W * THUMB_H * 2 (160 * 254 * 2)
  if(W == 210) return 139860;  // DCOVER_W * DCOVER_H * 2 (210 * 333 * 2)
  if(W == 170) return 91800;   // NCOVER_W * NCOVER_H * 2 (170 * 270 * 2)
  return (size_t)W * (size_t)(W * 1011 / 638) * 2;
}

inline bool sd_cache_exists(const String& id, int W, const String& coverUrl = ""){
  if(!s_sd_ok) return false;
  File f = SD.open(sd_cache_path(id, W), FILE_READ);
  if(!f) return false;
  size_t fsz = (size_t)f.size();
  size_t want = sd_expected_cover_size(W);
  if(fsz == want + 4 && coverUrl.length() > 0){
    // File has a 4-byte trailing URL hash footer: verify it matches current coverUrl
    uint32_t stored_h = 0;
    f.seek(fsz - 4);
    bool r_ok = (f.read((uint8_t*)&stored_h, 4) == 4);
    f.close();
    return r_ok && (stored_h == sd_cover_url_hash(coverUrl));
  }
  f.close();
  return (fsz == want || fsz == want + 4 || fsz > 0);
}

inline bool sd_cache_read(const String& id, int W, uint8_t* buf, size_t want, const String& coverUrl = ""){
  if(!s_sd_ok) return false;
  File f = SD.open(sd_cache_path(id, W), FILE_READ);
  if(!f) return false;
  size_t fsz = (size_t)f.size();
  bool ok = false;
  if(fsz == want + 4){
    // Versioned cache file: read pixels + verify 4-byte URL hash footer
    if(f.read(buf, want) == (int)want){
      uint32_t stored_h = 0;
      if(f.read((uint8_t*)&stored_h, 4) == 4){
        uint32_t cur_h = sd_cover_url_hash(coverUrl);
        ok = (coverUrl.length() == 0 || stored_h == cur_h);
      }
    }
  } else if(fsz == want){
    // Legacy/pre-populated cache file without footer: accept and upgrade footer if coverUrl is known
    ok = (f.read(buf, want) == (int)want);
  }
  f.close();
  return ok;
}

inline void sd_cache_write(const String& id, int W, const uint8_t* buf, size_t want, const String& coverUrl = ""){
  if(!s_sd_ok) return;
  File f = SD.open(sd_cache_path(id, W), FILE_WRITE);
  if(!f) return;
  f.write(buf, want);
  uint32_t h = sd_cover_url_hash(coverUrl);
  if(h != 0){
    f.write((const uint8_t*)&h, 4);
  }
  f.close();
}

inline String sd_chap_path(const String& id){
  return String(SD_DIR) + "/" + id + ".chap";
}

static const uint16_t CHAP_MAGIC = 0x434A;

inline void sd_wr_str(File& f, const String& s){
  uint16_t n = (uint16_t)s.length();
  f.write((const uint8_t*)&n, 2);
  if(n) f.write((const uint8_t*)s.c_str(), n);
}

inline bool sd_rd_str(File& f, String& out){
  uint16_t n;
  if(f.read((uint8_t*)&n, 2) != 2) return false;
  out = "";
  if(n){
    char* b = (char*)malloc(n + 1);
    if(!b) return false;
    if(f.read((uint8_t*)b, n) != (int)n) { free(b); return false; }
    b[n] = 0; out = b; free(b);
  }
  return true;
}

// Persist the freshly-parsed chapter list (in the s_ch globals) for this card to SD.
inline void sd_chap_write(const String& id){
  if(!s_sd_ok) return;
  File f = SD.open(sd_chap_path(id), FILE_WRITE);
  if(!f) return;
  f.write((const uint8_t*)&CHAP_MAGIC, 2);
  sd_wr_str(f, s_detail_title);
  uint16_t n = (uint16_t)s_ch_count;
  f.write((const uint8_t*)&n, 2);
  for(int k=0;k<s_ch_count;k++){
    int32_t d = s_ch[k].dur;
    f.write((const uint8_t*)&d, 4);
    sd_wr_str(f, s_ch[k].key);
    sd_wr_str(f, s_ch[k].track_key);
    sd_wr_str(f, s_ch[k].title);
  }
  f.close();
}

// Load this card's chapter list from SD into the s_ch globals. Returns false on any miss/short
// read so the caller falls back to a live fetch.
inline bool sd_chap_read(const String& id){
  if(!s_sd_ok) return false;
  File f = SD.open(sd_chap_path(id), FILE_READ);
  if(!f) return false;
  bool ok = false;
  do {
    uint16_t magic;
    if(f.read((uint8_t*)&magic, 2) != 2 || magic != CHAP_MAGIC) break;
    if(!sd_rd_str(f, s_detail_title)) break;
    uint16_t n;
    if(f.read((uint8_t*)&n, 2) != 2) break;
    if(n > MAX_CH) n = MAX_CH;
    int k = 0;
    for(; k < n; k++){
      int32_t d;
      if(f.read((uint8_t*)&d, 4) != 4) break;
      s_ch[k].dur = d;
      if(!sd_rd_str(f, s_ch[k].key))       break;
      if(!sd_rd_str(f, s_ch[k].track_key)) break;
      if(!sd_rd_str(f, s_ch[k].title))     break;
    }
    if(k == n){ s_ch_count = n; ok = true; }
  } while(0);
  f.close();
  return ok;
}

// Prune stale or superseded avatar cache files (av_*.565) from /ythumbs that do not match
// any active profile's current avatar_cache_key.
inline int sd_prune_stale_avatars(const String* valid_keys, int key_count){
#ifdef WOKWI_SIMULATION
  (void)valid_keys; (void)key_count;
  return 0; // In Wokwi simulation, host script prunes sdcard/ythumbs; avoid slow emulated SPI FAT scan
#else
  if(!s_sd_ok || key_count <= 0) return 0;
  File dir = SD.open(SD_DIR);
  if(!dir || !dir.isDirectory()) return 0;
  int removed = 0;
  char stale_paths[16][64];
  int stale_cnt = 0;
  while(File f = dir.openNextFile()){
    String name = f.name();
    int slash = name.lastIndexOf('/');
    if(slash >= 0) name = name.substring(slash + 1);
    f.close();
    if(name.startsWith("av_") && name.endsWith(".565")){
      bool keep = false;
      for(int i = 0; i < key_count; i++){
        if(valid_keys[i].length() > 0 && name == (valid_keys[i] + "_130.565")){
          keep = true;
          break;
        }
      }
      if(!keep && stale_cnt < 16){
        String full = String(SD_DIR) + "/" + name;
        strncpy(stale_paths[stale_cnt], full.c_str(), 63);
        stale_paths[stale_cnt][63] = '\0';
        stale_cnt++;
      }
    }
  }
  dir.close();
  for(int i = 0; i < stale_cnt; i++){
    if(SD.remove(stale_paths[i])) removed++;
  }
  return removed;
#endif
}

inline int sd_prune_stale_covers(bool clear_all = false){
#ifdef WOKWI_SIMULATION
  if(!clear_all) return 0; // Host script prunes sdcard/ythumbs in Wokwi
#endif
  if(!s_sd_ok) return 0;
  if(!clear_all && s_card_count <= 0) return 0;
  File dir = SD.open(SD_DIR);
  if(!dir || !dir.isDirectory()) return 0;
  int removed = 0;
  char batch[24][64];
  int batch_cnt = 0;
  while(File f = dir.openNextFile()){
    String name = f.name();
    int slash = name.lastIndexOf('/');
    if(slash >= 0) name = name.substring(slash + 1);
    f.close();
    if(name.startsWith(".")) continue;
    if(!clear_all && name.startsWith("av_")) continue;

    bool keep = false;
    if(!clear_all){
      String item_id = "";
      if(name.endsWith(".chap")){
        item_id = name.substring(0, name.length() - 5);
      } else if(name.endsWith("_160.565") || name.endsWith("_210.565") || name.endsWith("_170.565")){
        int us = name.lastIndexOf('_');
        if(us > 0) item_id = name.substring(0, us);
      }
      if(item_id.length() > 0){
        for(int i = 0; i < s_card_count; i++){
          if(s_cards[i].id == item_id){ keep = true; break; }
        }
      }
    }
    if(!keep && batch_cnt < 24){
      String full = String(SD_DIR) + "/" + name;
      strncpy(batch[batch_cnt], full.c_str(), 63);
      batch[batch_cnt][63] = '\0';
      batch_cnt++;
    }
  }
  dir.close();
  for(int i = 0; i < batch_cnt; i++){
    if(SD.remove(batch[i])) removed++;
  }
  return removed;
}
