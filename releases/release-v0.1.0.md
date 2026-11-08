# StoryDeck Touch v0.1.0 — Official Release Notes

**Release Tag:** `v0.1.0`  
**Classification:** Full Production Release (Initial Stable Release)  
**Target Architecture:** ESP32-S3 (8MB Octal PSRAM, 16MB Flash, 480x270 LCD)  
**Release Date:** November 8, 2026  

---

## 📟 Overview

We are thrilled to present the initial **v0.1.0 production release of StoryDeck Touch**, the dedicated, child-safe smart audio controller for the StoryDeck ecosystem!

StoryDeck Touch is a purpose-built, tactile touchscreen appliance engineered for little hands and bedside nightstands. Designed specifically for the Guition / JC4827W543 4.3" capacitive touch display (and compatible ESP32-S3 hardware), StoryDeck Touch empowers children to browse their curated digital audio library, select audiobooks and podcasts independently, and trigger playback directly onto their physical [Yoto](https://my.yotoplay.com/) players without smartphones, ads, or algorithmic distractions.

---

## 🌟 Major Highlights & New Features

### 1. 🤹 Child-Centric 3-Tab Interface
StoryDeck Touch features an intuitive, high-contrast user interface designed specifically for pre-readers and young children:
* **📚 Library Catalog:** Fluid horizontal paginated grid displaying high-resolution book jacket art with crisp child-friendly titles.
* **📂 Series Collections & Chronological Drill-Down:** Automatically groups multi-book series under Book #1 collection covers. Tapping a series opens a dedicated drill-down view showing each volume in chronological order with explicit sequence badges (`#1`, `#2`, `#3`, etc.).
* **🕒 Recently Played Shelf:** Instant access to currently in-progress stories, displaying real-time percentage progress indicators so children can resume their favorite book with a single tap.

---

### 2. ⚡ High-Performance Display & Touch Engine
Engineered from the ground up on ESP-IDF and PlatformIO for silky-smooth responsiveness:
* **ST7262 RGB Driver with DMA Bounce Buffers:** Utilizes a tuned 12MHz RGB interface and a 16,000-pixel DMA bounce buffer, completely eliminating LCD tearing, horizontal scanline shear, and flickering.
* **Persistent 32-Tile PSRAM Pool:** Leverages 8MB Octal PSRAM with an LRU (Least-Recently-Used) cache to pre-render and hold decoded image surfaces in fast memory.
* **Dedicated Dual-Core Pipelining:** Network downloads, JSON parsing, and JPEG decoding execute on Core 0 / Core 1 background tasks, ensuring the UI rendering and capacitive touch polling loop remain locked at a responsive 60fps.
* **GT911 Capacitive Touch Optimization:** Low-latency touch scanning with built-in noise filtering, edge rejection, and natural kinetic scroll deceleration.

---

### 3. 🌐 Dual-Mode Architecture: Core Managed vs. Standalone
* **StoryDeck Core Managed Mode (Recommended):** Connects directly to a self-hosted [StoryDeck Core](https://github.com/YotoStoryDeck/core) hub. Enforces zero-trust device quarantine, cryptographic device JWT authorization, automated bedtime lockouts, remote volume limits, and server-managed catalog rotations.
* **Standalone Cloud Mode:** Can connect directly to Audiobookshelf and Yoto Cloud APIs without needing a local StoryDeck Core server, ideal for travel or minimal lightweight deployments.

---

### 4. 💾 MicroSD Offline Art Cache & Resilience
* **Local Storage Caching:** Automatically saves downloaded album artwork and book covers to the onboard MicroSD card (FAT32).
* **Cryptographic URL Hashing:** Verifies asset integrity using deterministic hash identifiers, guaranteeing fast instant boots and preventing redundant network requests on subsequent wakeups.
* **Graceful Network Degradation:** If home Wi-Fi drops, cached book metadata and artwork remain instantly browseable.

---

### 5. 🔒 Grown-Ups Portal, Night Clock & Safety Guards
* **Parental Gate PIN Portal:** Secure 4-digit numeric keypad prevents accidental changes to Wi-Fi credentials, brightness, volume bounds, or child profile switching.
* **OK-to-Wake Night Clock:** Dims to a low-glare, dark bedtime clock during nighttime hours. Features an intuitive green wake glow (`#00FF33` / 0x07E0) signaling to toddlers and young children when it is okay to get out of bed.
* **Crash-Loop Guard & Safe Mode Recovery:** Non-Volatile Storage (NVS) boot counter tracks startup stability. If three unexpected restarts occur within 15 seconds, Touch automatically engages a safe recovery mode and broadcasts a setup Wi-Fi hotspot (`StoryDeck-Recovery`) with an embedded captive portal for diagnostics and re-configuration.

---

## 🛠️ Hardware Requirements & BOM

| Component | Specification | Supported Models |
| :--- | :--- | :--- |
| **Microcontroller & Screen** | ESP32-S3, 8MB PSRAM, 16MB Flash, 480x270 LCD | Guition JC4827W543 (Capacitive Touch) / Elecrow 4.3" |
| **Touch Controller** | I2C Capacitive Touch | Goodix GT911 |
| **LCD Controller** | 16-bit RGB Interface | Sitronix ST7262 |
| **Storage** | MicroSD Card Slot | Class 10 / UHS-I formatted FAT32 (4GB–64GB) |
| **Power** | 5V / 1.5A via USB-C | Standard USB-C bedside charger |

---

## 🚀 Flashing & Setup Guide

### Method A: One-Click Browser Flashing (Recommended)

No command line, Python, or compiling required:
1. Connect your JC4827W543 ESP32-S3 board to your PC or Mac via USB-C (ensure data cable).
2. Open Google Chrome or Microsoft Edge and navigate to the **[StoryDeck Web Flasher](https://yotostorydeck.github.io/touch/)**.
3. Click **Connect**, select the serial port (e.g. `CP210x` or `CH340`), and click **Install StoryDeck Touch v0.1.0**.
4. Once completed, the device boots into the on-screen Setup Wizard to connect to Wi-Fi and pair with your StoryDeck Core hub.

---

### Method B: PlatformIO Command Line

For developers building from source:

```bash
git clone https://github.com/YotoStoryDeck/touch.git
cd touch
git checkout v0.1.0

# Build and flash release firmware
pio run --target upload
```

---

## 📋 Ecosystem Compatibility

| Component | Minimum Version | Recommended | Notes |
| :--- | :--- | :--- | :--- |
| **StoryDeck Core Hub** | `v1.0.0` | `v1.1.0+` | Provides dynamic catalog curation & quarantine control |
| **Yoto Player** | Yoto Gen 2, Gen 3, Mini | Current Yoto OS | Target playback destination |
| **Audiobookshelf** | `v2.4.0` | `v2.10.0+` | Content library source |

---

## 🤝 Repositories & Community

* **StoryDeck Touch Repository:** [github.com/YotoStoryDeck/touch](https://github.com/YotoStoryDeck/touch)
* **StoryDeck Core Repository:** [github.com/YotoStoryDeck/core](https://github.com/YotoStoryDeck/core)
* **Project Specifications:** [github.com/alicia86/projects/tree/main/yotostorydeck](https://github.com/alicia86/projects/tree/main/yotostorydeck)
* **Web Flasher Tool:** [yotostorydeck.github.io/touch/](https://yotostorydeck.github.io/touch/)
