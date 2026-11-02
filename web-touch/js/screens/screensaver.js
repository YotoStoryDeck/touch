import { state } from '../state.js';

/**
 * 7-Segment Night-Clock Screensaver
 * Mirrors ESP32 LVGL implementation from yoto-esp32-controller (main.cpp: saver_open / saver_tick)
 *
 * Segment layout:
 *        A (0)
 *      +-------+
 *    F |       | B (1)
 *   (5)|   G   |
 *      +--(6)--+
 *    E |       | C (2)
 *   (4)|       |
 *      +-------+
 *        D (3)
 *
 * Color rules:
 * - 🔴 Bedtime (night): 0xE5352B (#E5352B)
 * - 🟢 OK-to-Wake (morning wake_time -> wake_time + 120min): 0x3AB06A (#3AB06A)
 * - ⚪ Daytime: 0xF2EDE0 (#F2EDE0)
 */

// 7-segment bitmask for digits 0-9
// Bit 0 = A, Bit 1 = B, Bit 2 = C, Bit 3 = D, Bit 4 = E, Bit 5 = F, Bit 6 = G
const SEG_MAP = [0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F];

// Heights matching firmware: 0: small 200px, 1: medium 260px, 2: large 320px
const DIG_HEIGHTS = [200, 260, 320];

export const ScreensaverScreen = {
  /**
   * Determine active clock color based on bedtime and wake time
   */
  getColorInfo() {
    const now = new Date();
    const nowM = now.getHours() * 60 + now.getMinutes();

    const [bedH, bedM] = (state.parentSettings.bedtime || '19:00').split(':').map(Number);
    const [wakeH, wakeM] = (state.parentSettings.wakeTime || '07:00').split(':').map(Number);

    const nightT = bedH * 60 + bedM;
    const wakeT = wakeH * 60 + wakeM;

    let isNight = false;
    if (nightT > wakeT) {
      // Overnight (e.g. 19:00 -> 07:00)
      isNight = (nowM >= nightT || nowM < wakeT);
    } else {
      // Daytime window
      isNight = (nowM >= nightT && nowM < wakeT);
    }

    const isMorning = !isNight && (nowM >= wakeT && nowM < wakeT + 120);

    let color = '#F2EDE0'; // Day warm off-white
    let statusText = 'Daytime';
    if (isNight) {
      color = '#E5352B'; // Red bedtime
      statusText = 'Bedtime';
    } else if (isMorning) {
      color = '#3AB06A'; // Green OK-to-wake
      statusText = 'OK to Wake';
    }

    return { color, isNight, isMorning, statusText, now };
  },

  /**
   * Render a single 7-segment digit
   * @param {number} digitVal 0-9, -1 for dash, -2 for blank
   * @param {number} W width of digit
   * @param {number} H height of digit
   * @param {number} t thickness of segment
   * @param {number} vlen length of vertical segments
   */
  renderDigit(digitVal, W, H, t, vlen) {
    if (digitVal === -2) {
      // Blank leading digit (e.g. 1:00 - 9:59)
      return `<div class="saver-digit" style="width:${W}px; height:${H}px; visibility:hidden;"></div>`;
    }

    const mask = digitVal >= 0 ? SEG_MAP[digitVal] : (digitVal === -1 ? 0x40 : 0x00);
    const isActive = (bit) => (mask & (1 << bit)) ? 'active' : '';

    const midTop = Math.floor((H - t) / 2);
    const botTop = Math.floor((H + t) / 2);

    return `
      <div class="saver-digit" style="width:${W}px; height:${H}px;">
        <!-- A: Top -->
        <div class="saver-seg ${isActive(0)}" style="top:0; left:${t}px; width:${W - 2 * t}px; height:${t}px;"></div>
        <!-- B: Top-Right -->
        <div class="saver-seg ${isActive(1)}" style="top:${t}px; left:${W - t}px; width:${t}px; height:${vlen}px;"></div>
        <!-- C: Bottom-Right -->
        <div class="saver-seg ${isActive(2)}" style="top:${botTop}px; left:${W - t}px; width:${t}px; height:${vlen}px;"></div>
        <!-- D: Bottom -->
        <div class="saver-seg ${isActive(3)}" style="bottom:0; left:${t}px; width:${W - 2 * t}px; height:${t}px;"></div>
        <!-- E: Bottom-Left -->
        <div class="saver-seg ${isActive(4)}" style="top:${botTop}px; left:0; width:${t}px; height:${vlen}px;"></div>
        <!-- F: Top-Left -->
        <div class="saver-seg ${isActive(5)}" style="top:${t}px; left:0; width:${t}px; height:${vlen}px;"></div>
        <!-- G: Middle -->
        <div class="saver-seg ${isActive(6)}" style="top:${midTop}px; left:${t}px; width:${W - 2 * t}px; height:${t}px;"></div>
      </div>
    `;
  },

  /**
   * Main Render function for Screensaver
   */
  render() {
    const { color, now } = this.getColorInfo();
    const sizeIdx = Math.max(0, Math.min(2, state.parentSettings.clockSize ?? 2));
    const H = DIG_HEIGHTS[sizeIdx];
    const t = Math.max(4, Math.floor(H / 8));
    const W = Math.floor(H / 2);
    const gap = Math.floor(H / 16);
    const vlen = Math.floor((H - 3 * t) / 2);

    // 12-hour clock format matching firmware: ((tm_hour + 11) % 12) + 1
    const rawHours = now.getHours();
    const hours = ((rawHours + 11) % 12) + 1;
    const minutes = now.getMinutes();

    const d0 = hours >= 10 ? 1 : -2; // Blank leading digit for 1:00 - 9:59
    const d1 = hours % 10;
    const d2 = Math.floor(minutes / 10);
    const d3 = minutes % 10;

    // Bedtime Sleep Timer status display
    let sleepBadgeHtml = '';
    if (state.sleepTimerRemainingSec > 0) {
      const minsLeft = Math.ceil(state.sleepTimerRemainingSec / 60);
      sleepBadgeHtml = `
        <div class="saver-sleep-badge">
          <span>🌙</span>
          <span>Sleep timer: ${minsLeft}m</span>
        </div>
      `;
    }

    return `
      <div class="screensaver-screen" id="screensaverScreen" style="--saver-col:${color};">
        ${sleepBadgeHtml}
        
        <div class="saver-container" style="gap:${gap}px;">
          ${this.renderDigit(d0, W, H, t, vlen)}
          ${this.renderDigit(d1, W, H, t, vlen)}

          <!-- Colon -->
          <div class="saver-colon" style="width:${t}px; height:${H}px; margin: 0 ${gap / 2}px;">
            <div class="saver-dot" style="width:${t}px; height:${t}px; margin-top:${Math.floor(H / 3 - t / 2)}px;"></div>
            <div class="saver-dot" style="width:${t}px; height:${t}px; margin-bottom:${Math.floor(H / 3 - t / 2)}px;"></div>
          </div>

          ${this.renderDigit(d2, W, H, t, vlen)}
          ${this.renderDigit(d3, W, H, t, vlen)}
        </div>

        <div class="saver-hint">Tap anywhere to wake</div>
      </div>
    `;
  }
};

