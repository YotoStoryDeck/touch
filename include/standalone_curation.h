#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include <Preferences.h>
#include "app_state.h"
#include "storage_sd.h"
#include "app_config.h"

namespace curation {

static std::vector<PsramString, PsramVectorAlloc<PsramString>> s_pinned_ids;
static std::vector<PsramString, PsramVectorAlloc<PsramString>> s_physical_ids;
static std::vector<PsramString, PsramVectorAlloc<PsramString>> s_hidden_ids;
static bool s_hide_physical = false;
static bool s_loaded = false;

inline bool is_card_pinned(const String& id){
  for(const auto& pid : s_pinned_ids){
    if(pid == id.c_str()) return true;
  }
  return false;
}

inline bool is_card_physical(const String& id){
  for(const auto& pid : s_physical_ids){
    if(pid == id.c_str()) return true;
  }
  return false;
}

inline bool is_card_hidden(const String& id){
  for(const auto& hid : s_hidden_ids){
    if(hid == id.c_str()) return true;
  }
  return false;
}

inline bool get_hide_physical(){
  return s_hide_physical;
}

inline void set_hide_physical(bool hide){
  s_hide_physical = hide;
}

inline void toggle_card_pinned(const String& id, bool pin){
  for(auto it = s_pinned_ids.begin(); it != s_pinned_ids.end(); ++it){
    if(*it == id.c_str()){
      if(!pin) s_pinned_ids.erase(it);
      return;
    }
  }
  if(pin){
    s_pinned_ids.push_back(id.c_str());
  }
}

inline void toggle_card_physical(const String& id, bool phys){
  for(auto it = s_physical_ids.begin(); it != s_physical_ids.end(); ++it){
    if(*it == id.c_str()){
      if(!phys) s_physical_ids.erase(it);
      return;
    }
  }
  if(phys){
    s_physical_ids.push_back(id.c_str());
  }
}

inline void toggle_card_hidden(const String& id, bool hide){
  for(auto it = s_hidden_ids.begin(); it != s_hidden_ids.end(); ++it){
    if(*it == id.c_str()){
      if(!hide) s_hidden_ids.erase(it);
      return;
    }
  }
  if(hide){
    s_hidden_ids.push_back(id.c_str());
  }
}

inline bool has_nvs_curation_data(){
  Preferences p;
  if(!p.begin("curation", true)) return false;
  bool has_data = p.isKey("pinned") || p.isKey("phys") || p.isKey("hidden") || p.isKey("hidep");
  p.end();
  return has_data;
}

inline bool migrate_nvs_to_sd(){
  if(!s_sd_ok) return false;
  if(!has_nvs_curation_data()) return false;

  Preferences p;
  if(!p.begin("curation", false)) return false;
  bool nvs_hidep   = p.getBool("hidep", false);
  String nvs_pins  = p.getString("pinned", "");
  String nvs_phys  = p.getString("phys", "");
  String nvs_hid   = p.getString("hidden", "");

  auto parse_csv_add = [](const String& csv, std::vector<PsramString, PsramVectorAlloc<PsramString>>& target){
    if(csv.length() == 0) return;
    int start = 0;
    while(start < csv.length()){
      int comma = csv.indexOf(',', start);
      if(comma == -1) comma = csv.length();
      String token = csv.substring(start, comma);
      token.trim();
      if(token.length() > 0){
        bool exists = false;
        for(const auto& existing : target){
          if(existing == token.c_str()){ exists = true; break; }
        }
        if(!exists) target.push_back(token.c_str());
      }
      start = comma + 1;
    }
  };

  // If SD already has a curation file, load it first to merge
  if(SD.exists("/config/curation.json")){
    File f = SD.open("/config/curation.json", FILE_READ);
    if(f){
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, f);
      f.close();
      if(!err){
        if(doc["hide_physical"].is<bool>() && doc["hide_physical"].as<bool>()) s_hide_physical = true;
        if(!doc["pinned"].isNull()){
          for(JsonVariant v : doc["pinned"].as<JsonArray>()){
            String sid = v.as<String>();
            if(sid.length() > 0){
              bool exists = false;
              for(const auto& existing : s_pinned_ids){ if(existing == sid.c_str()){ exists = true; break; } }
              if(!exists) s_pinned_ids.push_back(sid.c_str());
            }
          }
        }
        if(!doc["physical"].isNull()){
          for(JsonVariant v : doc["physical"].as<JsonArray>()){
            String sid = v.as<String>();
            if(sid.length() > 0){
              bool exists = false;
              for(const auto& existing : s_physical_ids){ if(existing == sid.c_str()){ exists = true; break; } }
              if(!exists) s_physical_ids.push_back(sid.c_str());
            }
          }
        }
        if(!doc["hidden"].isNull()){
          for(JsonVariant v : doc["hidden"].as<JsonArray>()){
            String sid = v.as<String>();
            if(sid.length() > 0){
              bool exists = false;
              for(const auto& existing : s_hidden_ids){ if(existing == sid.c_str()){ exists = true; break; } }
              if(!exists) s_hidden_ids.push_back(sid.c_str());
            }
          }
        }
      }
    }
  }

  if(nvs_hidep) s_hide_physical = true;
  parse_csv_add(nvs_pins, s_pinned_ids);
  parse_csv_add(nvs_phys, s_physical_ids);
  parse_csv_add(nvs_hid, s_hidden_ids);

  if(!SD.exists("/config")) SD.mkdir("/config");
  File f = SD.open("/config/curation.json", FILE_WRITE);
  if(f){
    JsonDocument doc;
    doc["hide_physical"] = s_hide_physical;
    JsonArray pArr = doc["pinned"].to<JsonArray>();
    for(const auto& id : s_pinned_ids) pArr.add(id.c_str());
    JsonArray phArr = doc["physical"].to<JsonArray>();
    for(const auto& id : s_physical_ids) phArr.add(id.c_str());
    JsonArray hArr = doc["hidden"].to<JsonArray>();
    for(const auto& id : s_hidden_ids) hArr.add(id.c_str());
    serializeJson(doc, f);
    f.close();

    // Clear NVS since migration is fully persisted on SD
    p.clear();
    p.end();

    log_msg("[CURATION] Migrated NVS items to /sd/config/curation.json (%d pinned, %d physical, %d hidden). NVS cleared.",
            (int)s_pinned_ids.size(), (int)s_physical_ids.size(), (int)s_hidden_ids.size());
    return true;
  }
  p.end();
  return false;
}

inline void load(){
  s_pinned_ids.clear();
  s_physical_ids.clear();
  s_hidden_ids.clear();
  s_hide_physical = false;

  if(s_sd_ok){
    // Auto-migrate any existing NVS curation data to SD if found
    if(has_nvs_curation_data()){
      if(migrate_nvs_to_sd()){
        s_loaded = true;
        log_msg("[CURATION] Active after migration: %d pinned, %d physical, %d hidden",
                (int)s_pinned_ids.size(), (int)s_physical_ids.size(), (int)s_hidden_ids.size());
        return;
      }
    }

    s_pinned_ids.clear();
    s_physical_ids.clear();
    s_hidden_ids.clear();

    if(!SD.exists("/config")) SD.mkdir("/config");
    if(SD.exists("/config/curation.json")){
      File f = SD.open("/config/curation.json", FILE_READ);
      if(f){
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, f);
        f.close();
        if(!err){
          s_hide_physical = (bool)(doc["hide_physical"] | false);
          if(!doc["pinned"].isNull()){
            for(JsonVariant v : doc["pinned"].as<JsonArray>()){
              String sid = v.as<String>();
              if(sid.length() > 0 && !is_card_pinned(sid)) s_pinned_ids.push_back(sid.c_str());
            }
          }
          if(!doc["physical"].isNull()){
            for(JsonVariant v : doc["physical"].as<JsonArray>()){
              String sid = v.as<String>();
              if(sid.length() > 0 && !is_card_physical(sid)) s_physical_ids.push_back(sid.c_str());
            }
          }
          if(!doc["hidden"].isNull()){
            for(JsonVariant v : doc["hidden"].as<JsonArray>()){
              String sid = v.as<String>();
              if(sid.length() > 0 && !is_card_hidden(sid)) s_hidden_ids.push_back(sid.c_str());
            }
          }
          s_loaded = true;
          log_msg("[CURATION] Loaded from SD: %d pinned, %d physical, %d hidden, hide_phys=%d",
                  (int)s_pinned_ids.size(), (int)s_physical_ids.size(), (int)s_hidden_ids.size(), s_hide_physical ? 1 : 0);
          return;
        }
      }
    }
  }

  // Fallback to NVS if SD not available
  Preferences p;
  p.begin("curation", true);
  s_hide_physical = p.getBool("hidep", false);
  String pins = p.getString("pinned", "");
  String phys = p.getString("phys", "");
  String hidden = p.getString("hidden", "");
  p.end();

  auto parse_csv = [](const String& csv, std::vector<PsramString, PsramVectorAlloc<PsramString>>& target){
    if(csv.length() == 0) return;
    int start = 0;
    while(start < csv.length()){
      int comma = csv.indexOf(',', start);
      if(comma == -1) comma = csv.length();
      String token = csv.substring(start, comma);
      token.trim();
      if(token.length() > 0) target.push_back(token.c_str());
      start = comma + 1;
    }
  };

  parse_csv(pins, s_pinned_ids);
  parse_csv(phys, s_physical_ids);
  parse_csv(hidden, s_hidden_ids);
  s_loaded = true;
  log_msg("[CURATION] Loaded from NVS fallback: %d pinned, %d physical, %d hidden",
          (int)s_pinned_ids.size(), (int)s_physical_ids.size(), (int)s_hidden_ids.size());
}

inline void save(){
  if(s_sd_ok){
    if(!SD.exists("/config")) SD.mkdir("/config");
    File f = SD.open("/config/curation.json", FILE_WRITE);
    if(f){
      JsonDocument doc;
      doc["hide_physical"] = s_hide_physical;
      JsonArray pArr = doc["pinned"].to<JsonArray>();
      for(const auto& id : s_pinned_ids) pArr.add(id.c_str());
      JsonArray phArr = doc["physical"].to<JsonArray>();
      for(const auto& id : s_physical_ids) phArr.add(id.c_str());
      JsonArray hArr = doc["hidden"].to<JsonArray>();
      for(const auto& id : s_hidden_ids) hArr.add(id.c_str());
      serializeJson(doc, f);
      f.close();
      log_msg("[CURATION] Saved to /sd/config/curation.json");

      // Also clean up NVS so NVS doesn't hold stale data
      Preferences p;
      if(p.begin("curation", false)){
        p.clear();
        p.end();
      }
      return;
    }
  }

  // Fallback to NVS
  Preferences p;
  p.begin("curation", false);
  p.putBool("hidep", s_hide_physical);
  auto make_csv = [](const std::vector<PsramString, PsramVectorAlloc<PsramString>>& src){
    String res = "";
    for(size_t i = 0; i < src.size(); i++){
      if(i > 0) res += ",";
      res += src[i].c_str();
    }
    return res;
  };
  p.putString("pinned", make_csv(s_pinned_ids));
  p.putString("phys", make_csv(s_physical_ids));
  p.putString("hidden", make_csv(s_hidden_ids));
  p.end();
  log_msg("[CURATION] Saved to NVS fallback");
}

} // namespace curation
