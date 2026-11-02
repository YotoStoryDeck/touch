import { state } from '../state.js';
import { Utils } from '../config.js';
import { BrokerApi } from '../broker-api.js';

/**
 * Screen 5: Grown-Ups Settings Screen Component
 * Organized into four tabs matching DLP-015 and wiki documentation:
 * 1. 🌙 Sleep / Bed
 * 2. 🎯 Audio Target
 * 3. 💡 Display / Dim
 * 4. 📶 Network
 */
export const GrownUpsScreen = {
  render() {
    const isStandalone = state.mode === 'standalone';
    const isLocked = state.lockPlayer;
    const allPlayers = state.getAllAvailablePlayers();
    const activeDev = state.activeDevice;
    const activeTab = state.guActiveTab || 'sleep';

    const brightness = state.parentSettings.brightness ?? 80;
    const clockSize = state.parentSettings.clockSize ?? 2;
    const clockSizeLabels = ['Small', 'Medium', 'Large'];

    return `
      <div class="grownups-screen">
        <div class="gu-header">
          <div class="gu-back" data-act="gu-close" title="Exit to Home">&#10005;</div>
          <div class="gu-title">&#128274; Grown-ups Settings</div>
          <button class="gu-pill-btn" style="margin-left:auto; font-size:12px; padding:6px 12px;" data-act="gu-change-pin">&#128274; Change PIN</button>
        </div>

        <!-- 4-Tab Navigation Bar -->
        <div class="gu-tabs-bar">
          <button class="gu-tab-btn ${activeTab === 'sleep' ? 'active' : ''}" data-act="gu-tab" data-tab="sleep">
            <span class="gu-tab-icon">&#127769;</span>
            <span class="gu-tab-label">Sleep / Bed</span>
          </button>
          <button class="gu-tab-btn ${activeTab === 'target' ? 'active' : ''}" data-act="gu-tab" data-tab="target">
            <span class="gu-tab-icon">&#127919;</span>
            <span class="gu-tab-label">Audio Target</span>
          </button>
          <button class="gu-tab-btn ${activeTab === 'display' ? 'active' : ''}" data-act="gu-tab" data-tab="display">
            <span class="gu-tab-icon">&#128161;</span>
            <span class="gu-tab-label">Display / Dim</span>
          </button>
          <button class="gu-tab-btn ${activeTab === 'network' ? 'active' : ''}" data-act="gu-tab" data-tab="network">
            <span class="gu-tab-icon">&#128246;</span>
            <span class="gu-tab-label">Network</span>
          </button>
        </div>

        <div class="gu-scroll">
          <!-- TAB 1: 🌙 SLEEP / BEDTIME -->
          ${activeTab === 'sleep' ? `
            <div class="gu-section">
              <div class="gu-sec-title">
                <span>Sleep Timer Presets</span>
                <span style="font-size:12px; font-weight:600; color:#8c8272;">
                  ${state.parentSettings.sleepTimerMin ? `${state.parentSettings.sleepTimerMin} min (${state.parentSettings.sleepTimerMode === 'bedtime' ? 'Bedtime Only' : 'Continuous'})` : 'Off / Continuous'}
                </span>
              </div>
              <div class="gu-row" style="margin-bottom: 8px;">
                ${[0, 15, 30, 45, 60].map(m => `
                  <button class="gu-pill-btn ${state.parentSettings.sleepTimerMin === m ? 'active' : ''}" data-act="gu-set-sleep" data-val="${m}">
                    ${m === 0 ? 'Off / Continuous' : `${m}m`}
                  </button>
                `).join('')}
              </div>
              ${state.parentSettings.sleepTimerMin > 0 ? `
                <div class="gu-row" style="margin-bottom: 6px;">
                  <button class="gu-pill-btn ${state.parentSettings.sleepTimerMode !== 'always' ? 'active' : ''}" data-act="gu-set-sleep-scope" data-val="bedtime">&#127769; Bedtime Hours Only</button>
                  <button class="gu-pill-btn ${state.parentSettings.sleepTimerMode === 'always' ? 'active' : ''}" data-act="gu-set-sleep-scope" data-val="always">&#9728;&#65039; All the Time (Day &amp; Night)</button>
                </div>
              ` : ''}
              <div class="gu-info-text">
                ${state.parentSettings.sleepTimerMin === 0 
                  ? 'Continuous playback. No automatic sleep cutoff.' 
                  : (state.parentSettings.sleepTimerMode === 'bedtime' 
                      ? `Only arms between Bedtime (${state.parentSettings.bedtime}) and Wake Time (${state.parentSettings.wakeTime}). Day playback will continue uninterrupted.` 
                      : `Arms on every playback session all day long.`)}
              </div>
            </div>

            <div class="gu-section">
              <div class="gu-sec-title">
                <span>Bedtime Curfew &amp; OK-to-Wake Schedules</span>
              </div>
              <div class="gu-row" style="align-items:center; gap:20px;">
                <div>
                  <label style="font-size:12px; font-weight:600; color:#8c8272; display:block; margin-bottom:2px;">Bedtime (Clock Red):</label>
                  <input type="time" class="gu-select" style="max-width:140px; padding:6px 10px;" id="guBedtimeInput" value="${state.parentSettings.bedtime}">
                </div>
                <div>
                  <label style="font-size:12px; font-weight:600; color:#8c8272; display:block; margin-bottom:2px;">OK-to-Wake (Clock Green):</label>
                  <input type="time" class="gu-select" style="max-width:140px; padding:6px 10px;" id="guWakeInput" value="${state.parentSettings.wakeTime}">
                </div>
              </div>
              <div class="gu-info-text">
                Transitions the Night Clock screensaver to soft red at bedtime, and transitions to cheerful green when morning arrives to indicate when it is okay to get out of bed!
              </div>
            </div>

            <div class="gu-section">
              <div class="gu-sec-title">
                <span>Travel &amp; Away Player Protection</span>
                <span style="font-size:12px; font-weight:600; color:${state.blockRemotePlayback ? '#16a34a' : '#ea580c'};">
                  ${state.blockRemotePlayback ? '&#128737;&#65039; Protected (Subnet Proximity)' : '&#9888;&#65039; Prompts on Away'}
                </span>
              </div>
              <div class="gu-row">
                <button class="gu-pill-btn ${state.blockRemotePlayback ? 'active' : ''}" data-act="gu-set-remote-block" data-val="true">Block Away Players</button>
                <button class="gu-pill-btn ${!state.blockRemotePlayback ? 'active' : ''}" data-act="gu-set-remote-block" data-val="false">Prompt Before Playing</button>
              </div>
              <div class="gu-info-text">
                Prevents home bedtime sleep timers and accidental card taps from stopping or playing audio on Yoto players traveling outside the home network (car or hotel).
              </div>
            </div>
          ` : ''}

          <!-- TAB 2: 🎯 AUDIO TARGET & ROAMING -->
          ${activeTab === 'target' ? `
            <div class="gu-section">
              <div class="gu-sec-title">
                <span>Target Player Roaming vs. Locking</span>
                <span style="font-size:12px; font-weight:600; color:#8c8272;">${isLocked ? '&#128274; Hardware Locked' : '&#127760; Profile Roaming'}</span>
              </div>
              <div class="gu-row">
                <button class="gu-pill-btn ${!isLocked ? 'active' : ''}" data-act="gu-set-lock" data-lock="false">Profile Roaming (Default)</button>
                <button class="gu-pill-btn ${isLocked ? 'active' : ''}" data-act="gu-set-lock" data-lock="true">Lock to Specific Player</button>
              </div>
              ${isLocked ? `
                <div style="margin-top:4px;">
                  <label style="font-size:13px; font-weight:600; color:#4a453e; display:block; margin-bottom:4px;">Assigned Bedside Speaker for this Screen:</label>
                  <select class="gu-select" data-act="gu-select-lock-player">
                    ${allPlayers.map(p => `
                      <option value="${Utils.esc(p.id)}" ${p.id === activeDev?.id ? 'selected' : ''}>
                        ${Utils.esc(p.name)} (${p.online ? 'Online' : 'Offline'})
                      </option>
                    `).join('')}
                  </select>
                </div>
              ` : ''}
              <div class="gu-info-text">
                ${isLocked 
                  ? '<b>Hardware Locked:</b> Binds this physical touch unit permanently to a specific Yoto Player or Mini. Changing profiles updates cards shown, but playback always targets this room.' 
                  : '<b>Profile Roaming:</b> Dynamically sends audio to whichever player is assigned to the currently active child profile (ideal for shared tables).'}
              </div>
            </div>

            <div class="gu-section">
              <div class="gu-sec-title">
                <span>Operational Mode</span>
                <span style="font-size:12px; font-weight:600; color:#8c8272;">${isStandalone ? 'Standalone Cloud' : 'StoryDeck Core Managed'}</span>
              </div>
              <div class="gu-row">
                <button class="gu-pill-btn ${!isStandalone ? 'active' : ''}" data-act="gu-set-mode" data-mode="core">Core Managed (Profiles)</button>
                <button class="gu-pill-btn ${isStandalone ? 'active' : ''}" data-act="gu-set-mode" data-mode="standalone">Standalone Cloud (Players)</button>
              </div>
              <div class="gu-info-text">
                ${isStandalone 
                  ? '<b>Standalone Mode:</b> Connects directly to Yoto Cloud without a local server. Single household account-wide card library.' 
                  : '<b>Core Mode:</b> Connects to StoryDeck Core hub with Audiobookshelf streaming, multi-child profiles, and local proxy caching.'}
              </div>
            </div>
          ` : ''}

          <!-- TAB 3: 💡 DISPLAY / DIM -->
          ${activeTab === 'display' ? `
            <div class="gu-section">
              <div class="gu-sec-title">
                <span>Daytime Display Brightness</span>
                <span style="font-size:12px; font-weight:600; color:#8c8272;" id="guBrightnessLabel">${brightness}%</span>
              </div>
              <div class="gu-slider-wrap">
                <input type="range" class="gu-slider" min="10" max="100" value="${brightness}" data-act="gu-set-brightness" id="guBrightnessSlider">
              </div>
              <div class="gu-info-text">Backlight PWM control (10% to 100%). Controls CH422G EXIO2 on Waveshare hardware.</div>
            </div>

            <div class="gu-section">
              <div class="gu-sec-title">
                <span>Night Clock Inactivity Timeout</span>
                <span style="font-size:12px; font-weight:600; color:#8c8272;">
                  ${state.parentSettings.screensaverMin ? `After ${state.parentSettings.screensaverMin} min` : 'Never (Off)'}
                </span>
              </div>
              <div class="gu-row">
                ${[0, 1, 2, 5, 15].map(m => `
                  <button class="gu-pill-btn ${state.parentSettings.screensaverMin === m ? 'active' : ''}" data-act="gu-set-saver" data-val="${m}">
                    ${m === 0 ? 'Never' : `${m}m`}
                  </button>
                `).join('')}
              </div>
              <div class="gu-info-text">Delay before screen transitions to dim bedside clock. Red from bedtime to wake; green after wake time.</div>
            </div>

            <div class="gu-section">
              <div class="gu-sec-title">
                <span>Bedside Clock Digit Size</span>
                <span style="font-size:12px; font-weight:600; color:#8c8272;">${clockSizeLabels[clockSize] || 'Large'}</span>
              </div>
              <div class="gu-row">
                ${[0, 1, 2].map(idx => `
                  <button class="gu-pill-btn ${clockSize === idx ? 'active' : ''}" data-act="gu-set-clock-size" data-val="${idx}">
                    ${clockSizeLabels[idx]}
                  </button>
                `).join('')}
              </div>
              <div class="gu-info-text">Configures 7-segment clock scale for bedside readability across the room.</div>
            </div>
          ` : ''}

          <!-- TAB 4: 📶 NETWORK & SYSTEM -->
          ${activeTab === 'network' ? `
            <div class="gu-section">
              <div class="gu-sec-title">
                <span>Network &amp; Proximity Diagnostics</span>
                <span style="font-size:12px; font-weight:600; color:#16a34a;">&#9679; HOME (Local Subnet)</span>
              </div>
              <div class="gu-info-text" style="font-family:monospace; font-size:12px; line-height:1.6; color:#554f46;">
                <b>Local Subnet:</b> 192.168.1.xxx &bull; Wi-Fi RSSI: -52 dBm<br>
                <b>Core Server Link:</b> ${window.location.origin} (Latency: &lt;4ms)<br>
                <b>Hardware Model:</b> Waveshare ESP32-S3-Touch-LCD-4.3B<br>
                <b>Resolution:</b> ${window.innerWidth} &times; ${window.innerHeight} px (${window.devicePixelRatio || 1}x DPI)
              </div>
            </div>

            <div class="gu-section">
              <div class="gu-sec-title">
                <span>Hardware Maintenance &amp; Actions</span>
              </div>
              <div class="gu-row">
                <button class="gu-pill-btn" data-act="gu-restart" style="color:var(--red,#e5352b); font-weight:700;">&#128260; Restart Device</button>
                <button class="gu-pill-btn" data-act="gu-re-pair">&#128279; Re-pair with Core</button>
                <a href="/settings" target="_blank" class="gu-pill-btn" style="text-decoration:none; display:inline-flex; align-items:center;">&#9881; Web Settings Portal</a>
              </div>
              <div class="gu-info-text">
                Rebooting clears temporary memory. Re-pairing broadcasts a fresh mDNS discovery handshake to StoryDeck Core.
              </div>
            </div>
          ` : ''}
        </div>
      </div>
    `;
  }
};
