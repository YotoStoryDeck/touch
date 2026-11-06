#pragma once
// WiFi Setup Hotspot Watchdog, QR Diagnostic Error Screen, and StoryDeck Core Hardware Pairing Screen
#include <Arduino.h>
#include <WiFi.h>
#include <lvgl.h>
#include "app_state.h"
#include "core_api.h"
#include "ui_grownups.h"

// ---------------- Hotspot Watchdog & Visual UI Indicators ----------------
static lv_obj_t* s_err_hs_badge = nullptr;
static lv_obj_t* s_err_qr = nullptr;

static void start_hotspot(){
  s_hotspot_wanted = true;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("Yoto-Setup");
  Serial.println("[WIFI] Setup Hotspot asserted: 'Yoto-Setup' (192.168.4.1)");
}

static void ensure_hotspot_active(){
  if(s_hotspot_wanted && !s_lib_ok){
    // If we want the hotspot and are not yet fully online with a valid library, ensure AP mode is alive
    if(!(WiFi.getMode() & WIFI_MODE_AP) || WiFi.softAPIP() == IPAddress(0,0,0,0)){
      Serial.println("[WIFI] Watchdog: Hotspot went down! Re-enabling 'Yoto-Setup'...");
      WiFi.mode(WIFI_AP_STA);
      WiFi.softAP("Yoto-Setup");
    }
  }
}

void start_hotspot_cb(lv_event_t*){
  start_hotspot();
  show_error("Setup Hotspot Active",
             "Connect your phone to WiFi 'Yoto-Setup' or scan the QR code to join.\nNavigate to http://192.168.4.1/settings to set up WiFi.",
             "Hotspot is live. Open browser to configure.");
}

static void show_error(const char* title, const char* message, const char* action_hint){
  if(s_scr_home && s_lib_ok) return; // don't cover home if already healthy
  if(!s_scr_error){
    s_scr_error = lv_obj_create(NULL);
  }
  lv_obj_clean(s_scr_error);
  lv_obj_set_style_bg_color(s_scr_error, lv_color_hex(0xFBF7F0), 0);
  s_err_hs_badge = nullptr;
  s_err_hs_client_lbl = nullptr;
  s_err_qr = nullptr;

  bool is_ap = (WiFi.getMode() & WIFI_MODE_AP);

  // Red accent header
  lv_obj_t* t = lv_label_create(s_scr_error);
  lv_label_set_text(t, title ? title : (is_ap ? "Setup Hotspot Active" : "Setup / Connection Needed"));
  lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(t, is_ap ? lv_color_hex(0x23303A) : lv_color_hex(0xE5352B), 0);
  lv_obj_set_pos(t, 24, 20);

  // Left Content Container (Width: 510px)
  lv_obj_t* left_box = lv_obj_create(s_scr_error);
  lv_obj_set_pos(left_box, 24, 64);
  lv_obj_set_size(left_box, 500, 310);
  lv_obj_set_style_bg_opa(left_box, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(left_box, 0, 0);
  lv_obj_set_style_pad_all(left_box, 0, 0);
  lv_obj_clear_flag(left_box, LV_OBJ_FLAG_SCROLLABLE);

  // Message body
  lv_obj_t* m = lv_label_create(left_box);
  lv_label_set_text(m, message ? message : "");
  lv_obj_set_style_text_font(m, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(m, lv_color_hex(0x23303A), 0);
  lv_obj_set_width(m, 500);

  if(is_ap){
    // Hotspot Active Card Badge
    lv_obj_t* hs_card = lv_obj_create(left_box);
    lv_obj_set_width(hs_card, 500);
    lv_obj_set_height(hs_card, 110);
    lv_obj_set_style_bg_color(hs_card, lv_color_hex(0xE6F4EA), 0); // Soft sage green
    lv_obj_set_style_border_width(hs_card, 1, 0);
    lv_obj_set_style_border_color(hs_card, lv_color_hex(0x3AB06A), 0);
    lv_obj_set_style_radius(hs_card, 12, 0);
    lv_obj_set_style_pad_all(hs_card, 12, 0);
    lv_obj_align(hs_card, LV_ALIGN_TOP_LEFT, 0, 60);

    lv_obj_t* hs_title = lv_label_create(hs_card);
    lv_label_set_text(hs_title, LV_SYMBOL_WIFI " Hotspot Active: 'Yoto-Setup'");
    lv_obj_set_style_text_font(hs_title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(hs_title, lv_color_hex(0x137333), 0);

    s_err_hs_client_lbl = lv_label_create(hs_card);
    int stations = WiFi.softAPgetStationNum();
    if(stations > 0){
      lv_label_set_text_fmt(s_err_hs_client_lbl, "%d device(s) connected! Open http://192.168.4.1/settings", stations);
      lv_obj_set_style_text_color(s_err_hs_client_lbl, lv_color_hex(0x137333), 0);
    } else {
      lv_label_set_text(s_err_hs_client_lbl, "1. Connect to WiFi 'Yoto-Setup' (No pass)\n2. Open browser: http://192.168.4.1/settings");
      lv_obj_set_style_text_color(s_err_hs_client_lbl, lv_color_hex(0x137333), 0);
    }
    lv_obj_set_style_text_font(s_err_hs_client_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(s_err_hs_client_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  }

  // Network Details / Hint
  String details = "";
  if(WiFi.status() == WL_CONNECTED){
    details += "WiFi Connected: " + WiFi.SSID() + " (" + String(WiFi.RSSI()) + " dBm)\n";
    details += "Device IP: " + WiFi.localIP().toString() + "\n";
    details += "Web Settings: http://" + WiFi.localIP().toString() + "/settings\n";
  } else if(is_ap){
    details += "Hotspot IP: http://192.168.4.1/settings\n";
  } else {
    details += "WiFi not connected. Background reconnection active.\n";
  }
  if(action_hint && action_hint[0]){
    details += String("Hint: ") + action_hint;
  }

  lv_obj_t* det = lv_label_create(left_box);
  lv_label_set_text(det, details.c_str());
  lv_obj_set_style_text_font(det, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(det, lv_color_hex(0x7A7060), 0);
  lv_obj_set_width(det, 500);
  lv_obj_align(det, LV_ALIGN_TOP_LEFT, 0, is_ap ? 180 : 80);

  // Right QR Code Card (Width: 230px, Height: 290px)
  lv_obj_t* qr_card = lv_obj_create(s_scr_error);
  lv_obj_set_pos(qr_card, 545, 64);
  lv_obj_set_size(qr_card, 230, 290);
  lv_obj_set_style_bg_color(qr_card, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_border_width(qr_card, 1, 0);
  lv_obj_set_style_border_color(qr_card, lv_color_hex(0xE7E0D2), 0);
  lv_obj_set_style_radius(qr_card, 16, 0);
  lv_obj_set_style_pad_all(qr_card, 10, 0);
  lv_obj_clear_flag(qr_card, LV_OBJ_FLAG_SCROLLABLE);

  // Generate QR Code data
  String qr_data = is_ap ? "WIFI:T:nopass;S:Yoto-Setup;;" : ("http://" + WiFi.localIP().toString() + "/settings");
  lv_obj_t* qr = lv_qrcode_create(qr_card, 160, lv_color_hex(0x23303A), lv_color_hex(0xFFFFFF));
  lv_qrcode_update(qr, qr_data.c_str(), qr_data.length());
  lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 8);
  s_err_qr = qr;

  lv_obj_t* qr_label = lv_label_create(qr_card);
  lv_label_set_text(qr_label, is_ap ? "Scan to Join Hotspot\nWiFi: Yoto-Setup" : "Scan for Settings\nWeb Portal");
  lv_obj_set_style_text_font(qr_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(qr_label, lv_color_hex(0x7A7060), 0);
  lv_obj_set_style_text_align(qr_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(qr_label, LV_ALIGN_BOTTOM_MID, 0, -4);

  // Buttons container (Row at bottom)
  lv_obj_t* bcont = set_row(s_scr_error);
  lv_obj_set_pos(bcont, 24, 395);
  lv_obj_set_style_pad_column(bcont, 16, 0);

  lv_obj_t* b_set = pill_btn(bcont, "Open Settings", 0, [](lv_event_t*){ open_settings(); });
  lv_obj_set_size(b_set, 210, 56);
  lv_obj_set_style_bg_color(b_set, lv_color_hex(0xE5352B), 0);
  lv_obj_set_style_text_color(lv_obj_get_child(b_set, 0), lv_color_hex(0xFFFFFF), 0);

  lv_obj_t* b_hs = pill_btn(bcont, is_ap ? "Hotspot Active" : "Start Hotspot", 0, start_hotspot_cb);
  lv_obj_set_size(b_hs, 190, 56);
  if(is_ap){
    lv_obj_set_style_bg_color(b_hs, lv_color_hex(0x3AB06A), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(b_hs, 0), lv_color_hex(0xFFFFFF), 0);
  }

  lv_obj_t* b_try = pill_btn(bcont, "Retry", 0, [](lv_event_t*){ s_safe_mode = false; net_init_and_fetch(); });
  lv_obj_set_size(b_try, 140, 56);

  lv_scr_load(s_scr_error);
  lv_refr_now(lv_disp_get_default());
}

// ---------------- Pairing / Waiting for Admin Approval Screen ----------------
static lv_obj_t* s_pairing_status_lbl = nullptr;

static void show_pairing_screen(const String& core_url, const String& status_msg){
  status_hide();
  if(s_scr_pairing){
    if(s_pairing_status_lbl) lv_label_set_text(s_pairing_status_lbl, status_msg.c_str());
    return;
  }
  lv_obj_t* scr = lv_obj_create(NULL);
  s_scr_pairing = scr;
  lv_obj_set_style_bg_color(scr, lv_color_hex(0xFBF7F0), 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // Red accent header
  lv_obj_t* t = lv_label_create(scr);
  lv_label_set_text(t, "Pairing with StoryDeck Core");
  lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(t, lv_color_hex(0xE5352B), 0);
  lv_obj_set_pos(t, 24, 20);

  // Left Content Container
  lv_obj_t* left_box = lv_obj_create(scr);
  lv_obj_set_pos(left_box, 24, 64);
  lv_obj_set_size(left_box, 500, 310);
  lv_obj_set_style_bg_opa(left_box, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(left_box, 0, 0);
  lv_obj_set_style_pad_all(left_box, 0, 0);
  lv_obj_clear_flag(left_box, LV_OBJ_FLAG_SCROLLABLE);

  // Instructions
  lv_obj_t* m = lv_label_create(left_box);
  lv_label_set_text(m, "This bedside display has announced itself to StoryDeck Core and is awaiting approval in the Admin Portal.\n\nOnce approved, this controller will automatically pair, load child profiles, and unlock playback.");
  lv_obj_set_style_text_font(m, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(m, lv_color_hex(0x23303A), 0);
  lv_obj_set_width(m, 500);

  // Status Badge / Card
  lv_obj_t* stat_card = lv_obj_create(left_box);
  lv_obj_set_width(stat_card, 500);
  lv_obj_set_height(stat_card, 90);
  lv_obj_set_style_bg_color(stat_card, lv_color_hex(0xFFF3E0), 0); // Amber notice
  lv_obj_set_style_border_width(stat_card, 1, 0);
  lv_obj_set_style_border_color(stat_card, lv_color_hex(0xFF9800), 0);
  lv_obj_set_style_radius(stat_card, 12, 0);
  lv_obj_set_style_pad_all(stat_card, 12, 0);
  lv_obj_align(stat_card, LV_ALIGN_TOP_LEFT, 0, 100);

  s_pairing_status_lbl = lv_label_create(stat_card);
  lv_label_set_text(s_pairing_status_lbl, status_msg.c_str());
  lv_obj_set_style_text_font(s_pairing_status_lbl, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(s_pairing_status_lbl, lv_color_hex(0xE65100), 0);
  lv_obj_set_width(s_pairing_status_lbl, 470);

  // Device Info Footprint
  String dev_info = "Device MAC:  " + WiFi.macAddress() + "\n"
                  + "Local IP:    " + WiFi.localIP().toString() + "\n"
                  + "Core URL:    " + core_url;
  lv_obj_t* info_lbl = lv_label_create(left_box);
  lv_label_set_text(info_lbl, dev_info.c_str());
  lv_obj_set_style_text_font(info_lbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(info_lbl, lv_color_hex(0x7A7060), 0);
  lv_obj_set_width(info_lbl, 500);
  lv_obj_align(info_lbl, LV_ALIGN_TOP_LEFT, 0, 205);

  // Right QR Code Card (Links directly to Admin Portal Hardware Tab)
  lv_obj_t* qr_card = lv_obj_create(scr);
  lv_obj_set_pos(qr_card, 545, 64);
  lv_obj_set_size(qr_card, 230, 290);
  lv_obj_set_style_bg_color(qr_card, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_border_width(qr_card, 1, 0);
  lv_obj_set_style_border_color(qr_card, lv_color_hex(0xE7E0D2), 0);
  lv_obj_set_style_radius(qr_card, 16, 0);
  lv_obj_set_style_pad_all(qr_card, 10, 0);
  lv_obj_clear_flag(qr_card, LV_OBJ_FLAG_SCROLLABLE);

  String admin_url = core_url + "/admin#tab-hardware";
  lv_obj_t* qr = lv_qrcode_create(qr_card, 160, lv_color_hex(0x23303A), lv_color_hex(0xFFFFFF));
  lv_qrcode_update(qr, admin_url.c_str(), admin_url.length());
  lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 8);

  lv_obj_t* qr_lbl = lv_label_create(qr_card);
  lv_label_set_text(qr_lbl, "Scan to open\nAdmin Portal to approve");
  lv_obj_set_style_text_font(qr_lbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(qr_lbl, lv_color_hex(0x7A7060), 0);
  lv_obj_set_style_text_align(qr_lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(qr_lbl, LV_ALIGN_BOTTOM_MID, 0, -4);

  // Bottom action buttons
  lv_obj_t* bcont = set_row(scr);
  lv_obj_set_pos(bcont, 24, 395);
  lv_obj_set_style_pad_column(bcont, 16, 0);

  lv_obj_t* b_set = pill_btn(bcont, "Open Settings", 0, [](lv_event_t*){
    if(s_pairing_timer){ lv_timer_del(s_pairing_timer); s_pairing_timer=nullptr; }
    open_settings();
  });
  lv_obj_set_size(b_set, 210, 56);
  lv_obj_set_style_bg_color(b_set, lv_color_hex(0xE5352B), 0);
  lv_obj_set_style_text_color(lv_obj_get_child(b_set, 0), lv_color_hex(0xFFFFFF), 0);

  lv_obj_t* b_try = pill_btn(bcont, "Check Now", 0, [](lv_event_t*){
    if(s_pairing_timer){ lv_timer_del(s_pairing_timer); s_pairing_timer=nullptr; }
    net_init_and_fetch();
  });
  lv_obj_set_size(b_try, 160, 56);

  lv_scr_load(scr);
  lv_refr_now(lv_disp_get_default());
}

static void pairing_poll_cb(lv_timer_t* t){
  if(core::s_is_approved && s_lib_ok){
    lv_timer_del(t);
    s_pairing_timer = nullptr;
    return;
  }
  if(WiFi.status() == WL_CONNECTED && cfg::op_mode == "core" && cfg::core_url.length() > 0){
    String err;
    if(core::register_device(cfg::core_url, err)){
      // Approved! Kill pairing timer and fetch library
      lv_timer_del(t);
      s_pairing_timer = nullptr;
      if(s_scr_pairing){
        lv_obj_del_async(s_scr_pairing);
        s_scr_pairing = nullptr;
      }
      net_init_and_fetch();
    } else {
      if(s_pairing_status_lbl){
        lv_label_set_text(s_pairing_status_lbl, err.c_str());
      }
    }
  }
}
