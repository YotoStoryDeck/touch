# StoryDeck Touch

A dedicated, kid-friendly bedside **touchscreen companion controller** for [Yoto](https://yoto.dev) players, powered by the **ESP32-S3** and **LVGL**.

Children can independently browse their audiobook and music library, view full-color cover art, choose chapters, and start playback on their bedroom Yoto Player—without needing a phone, tablet, or parental intervention.

---

## 🌟 Modes of Operation

StoryDeck Touch supports two flexible operational modes:

1. **StoryDeck Core Mode (Managed — Recommended):**
   * Pairs with a self-hosted [**StoryDeck Core**](https://github.com/YotoStoryDeck/core) hub.
   * Browses unified libraries combining **Audiobookshelf (ABS)** and Yoto playlists.
   * Features multi-child profile switching, parental tag filtering, and Home Assistant bedtime controls.
   * Secure PIN pairing: device communicates using a lightweight token with zero cloud credentials stored on the bedside unit.

2. **Standalone Mode (Direct Yoto Cloud):**
   * Operates completely independently without a local server.
   * Talks directly to the official Yoto Developer API over Wi-Fi.
   * Directly mirrors Yoto account players without child profile or avatar selection.
   * Automatically refreshes its own OAuth tokens stored in secure ESP32 NVS flash.

---

## 📺 User Interface & Screens

| Screen | Description |
| --- | --- |
| **Profile Select** | Time-aware greeting ("Who is listening this morning/afternoon/tonight?") with centered multi-child avatar grid. |
| **Catalog Grid** | Responsive grid of cover art thumbnails (122×193px) with series grouping, highlight sorting, and ABS/Yoto source badges. |
| **Book Detail** | High-res cover art, track/chapter list, and instant play trigger. |
| **Now Playing** | Real-time playback status, album art, progress bar, skip controls, and volume slider with native Yoto volume ceilings. |
| **Night Clock** | Low-glare screensaver with configurable Red (Bedtime) and Green (OK-to-wake) indicators. |
| **Grown-Ups Screen** | Hidden parental settings menu (long-press gear + PIN) for Wi-Fi, operating mode, player lock, travel safety, and sleep timers. |

---

## 🛠️ Hardware Specifications

Targeted specifically for the **[Waveshare ESP32-S3-Touch-LCD-4.3B](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-4.3B)** (Rev 1.1):

| Component | Specification |
| --- | --- |
| **SoC** | ESP32-S3 (Dual-core Xtensa LX7, 240 MHz) |
| **Display** | 4.3" 800×480 IPS LCD, 16-bit RGB565 parallel interface |
| **Touch** | Goodix GT911 capacitive multi-touch (I²C) |
| **Memory** | 16 MB QIO Flash + 8 MB Octal PSRAM |
| **I/O Expander** | CH422G (controls LCD/touch reset and backlight PWM) |
| **Power** | 5V via USB-C (continuous bedside wall power) |
| **Enclosure** | 3D printable desktop stand or wall-mount case (included in `3d-models/`) |

### Hardware & Driver Engineering Notes
* **Pixel Clock:** RGB panel requires an exact **16 MHz** pixel clock (`pclk_active_neg = 1`) for panel sync.
* **DMA & PSRAM:** Uses single-framebuffer rendering in PSRAM with an internal SRAM bounce buffer (`cfg.dma_burst_size = 64`) to eliminate scanline flicker.
* **Touch Filtering:** Custom GT911 debouncing with hold-state logic between polls prevents touch jitter and accidental chapter selections.

---

## 🚀 Getting Started

### 1. Build & Flash via PlatformIO
```bash
# Clone the repository
git clone https://github.com/YotoStoryDeck/touch.git
cd touch

# Build firmware for Waveshare ESP32-S3
pio run -e waveshare-s3-43b

# Flash to device over USB-C
pio run -e waveshare-s3-43b -t upload
```

### 2. On-Device Setup Wizard
1. Power on the device.
2. Select your Wi-Fi network and enter the password using the on-screen touchscreen keyboard.
3. Choose **StoryDeck Core Mode** (enter the 6-digit pairing PIN shown on screen into your Core Web Admin) or **Standalone Mode** (scan the on-screen QR code to authenticate with Yoto).

---

## 📜 Credits & Lineage

StoryDeck Touch builds upon and is deeply indebted to:

* **Jay Heavner** ([@jayheavner](https://github.com/jayheavner)) — Creator of the original [yoto-esp32-controller](https://github.com/jayheavner/yoto-esp32-controller). His pioneering work established the ESP32-S3 display driver optimizations, LVGL UI layout, 3D enclosure designs, and standalone Yoto API integration.
* **Mylacc** ([@mylacc](https://github.com/mylacc)) — Creator of [yoto-audiobookshelf-bridge](https://github.com/mylacc/yoto-audiobookshelf-bridge), enabling self-hosted audiobook streaming.
* **LVGL** ([@lvgl](https://github.com/lvgl)) — Light and Versatile Embedded Graphics Library.

---

## 📄 License

This project is licensed under the [MIT License](LICENSE).
3D enclosure CAD files and models are licensed under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).
