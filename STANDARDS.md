# StoryDeck Touch — Development & Engineering Standards

This document establishes the mandatory engineering standards, dual-platform parity rules, and release lifecycle requirements for the **StoryDeck Touch** repository.

---

## 🔄 Dual-Platform Parity Standard (`web-touch` ↔ ESP32 Firmware)

The web prototype (`storydeck-touch/web-touch`) and the native ESP32-S3 firmware (`storydeck-touch/src/main.cpp`, `include/*.h`) must **always remain mirror images** of each other.

### 1. Lockstep Implementation Rule
* **Simultaneous Drafting**: Whenever a screen, modal, setting, control widget, or architectural behavior is added or modified in `web-touch`, the identical counterpart must be implemented in the native ESP32 C++ firmware (using LVGL and Arduino libraries).
* **Zero UI Drift**: Neither `web-touch` nor `main.cpp` may pull ahead in functionality. Visual layouts, touch targets, colors, padding, and screen navigation hierarchies must correspond 1:1.
* **Component Mapping**:
  | Feature / Screen | `web-touch` Implementation | ESP32 Firmware Implementation |
  |---|---|---|
  | **Parent PIN Modal** | `js/screens/pin.js`, `css/pin.css` | `open_pin_modal()`, `pin_digit_cb()` in `src/main.cpp` |
  | **Core Integration & Broker** | `js/broker-api.js`, `js/state.js` | `include/core_api.h`, `send_cmd()` in `src/main.cpp` |
  | **Child Profiles Screen** | `js/screens/profile.js`, `css/profile.css` | `open_profile_picker()`, `profile_card_cb()` in `src/main.cpp` |
  | **Catalog & Category Tabs** | `js/screens/catalog.js`, `css/catalog.css` | `build_grid()`, `tab_btn_cb()`, `apply_tab_filter()` in `src/main.cpp` |
  | **Grown-ups 4-Tab Settings** | `js/screens/grownups.js`, `css/grownups.css` | `open_settings()`, `gu_tab_switch()` in `src/main.cpp` |
  | **Audio Target / Player Lock** | `state.lockPlayer`, `state.lockedPlayerId` | `cfg::lock_player`, `cfg::locked_dev_id` in `include/app_config.h` |
  | **Bedside Screensaver Clock** | `js/screens/screensaver.js`, `css/screensaver.css` | `saver_open()`, `sv_set_digit()` in `src/main.cpp` |

---

## 🚦 Verification, Approval & Compilation Gate

To ensure developer agility without wasted compilation cycles or publishing untested binaries, follow this strict lifecycle:

### Phase 1: Rapid Iteration & Visual Review
* Prototype and polish layout, flow, animations, and edge-cases directly in `web-touch`.
* Mirror the changes into C++ (`src/main.cpp` / `include/*.h`) during development.
* **DO NOT** execute time-intensive PlatformIO compilation builds or generate firmware binaries while iterative prototyping is ongoing in `web-touch`.

### Phase 2: User Approval Gate
* Present the running `web-touch` interface to the user for interactive inspection and review.
* Iterate until the user explicitly approves the web-touch behavior and declares the milestone **ready for pre-release submission**.

### Phase 3: Compilation Verification & Pre-Release Tagging
1. **Compilation Check**: Once approved, run PlatformIO build verification (`pio run -e waveshare-s3-43b`) with temporary test credentials (`include/secrets.h`), verifying:
   - Zero syntax errors or missing symbol declarations.
   - Internal SRAM usage remains within safe limits (≤ 60% internal RAM, leaving headroom for DMA and networking).
   - Clean deletion of test `secrets.h` before git staging.
2. **Git Repository Synchronization**: Stage and commit changes using Conventional Commits (`feat:`, `fix:`, `docs:`), rebase, and push to `origin/dev`.
3. **Workflow Dispatch / Release Pipeline**: Trigger the GitHub Actions release workflow (`.github/workflows/release.yml`) by publishing a release tag (e.g. `v1.0.0-rc.2`) or via manual `workflow_dispatch`.
4. **Website Flasher Manifest Sync**: Never manually overwrite binaries in `storydeck-touch-website/bin/prerelease/`. Release assets are built in GitHub Actions and updated in the website repo strictly from sanctioned workflow artifacts.

---

## 🛠️ Hardware & Firmware Specifications
* **Target Board**: Waveshare ESP32-S3-Touch-LCD-4.3B (Rev 1.1)
* **Microcontroller**: ESP32-S3, 8MB Octal SPI Flash, 8MB PSRAM (Quad/Octal)
* **Display**: 800×480 RGB interface (ST7262 driver), 16-bit color depth (RGB565)
* **Touch Controller**: Goodix GT911 (I2C)
* **I/O Expander**: CH422G (I2C address 0x24/0x38)
  - `EXIO1`: GT911 Touch Reset (`EXIO_TP_RST`)
  - `EXIO2`: Backlight PWM & Panel DISP Enable (`EXIO_BL_DISP`)
  - `EXIO3`: LCD Reset (`EXIO_LCD_RST`)
  - `EXIO4`: MicroSD Chip Select (`EXIO_SD_CS`, Active Low)
