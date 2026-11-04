#pragma once
// MicroSD (SPI) -- persistent cover-thumbnail and chapter cache
// The 4.3B's TF slot is SPI on GPIO12/13/11, BUT SD_CS hangs off the CH422G (EXIO4).
#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include "hal_display.h"
#include "app_state.h"

#define SD_PIN_SCK      12
#define SD_PIN_MISO     13
#define SD_PIN_MOSI     11
#define SD_PIN_CS_DUMMY 6
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

inline String sd_cache_path(const String& id, int W){
  return String(SD_DIR) + "/" + id + "_" + W + ".565";
}

inline bool sd_cache_read(const String& id, int W, uint8_t* buf, size_t want){
  if(!s_sd_ok) return false;
  File f = SD.open(sd_cache_path(id, W), FILE_READ);
  if(!f) return false;
  bool ok = (((size_t)f.size() == want) && (f.read(buf, want) == (int)want));
  f.close();
  return ok;
}

inline void sd_cache_write(const String& id, int W, const uint8_t* buf, size_t want){
  if(!s_sd_ok) return;
  File f = SD.open(sd_cache_path(id, W), FILE_WRITE);
  if(!f) return;
  f.write(buf, want);
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
