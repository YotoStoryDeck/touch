import { state } from '../state.js';
import { Utils } from '../config.js';
import { BrokerApi } from '../broker-api.js';

/**
 * Screen 5: Grown-Ups Settings Screen Component
 * Opened by holding the gear icon in the home/catalog bar for 2.5 seconds (PIN-protected).
 */
export const GrownUpsScreen = {
  render() {
    const isStandalone = state.mode === 'standalone';
    const isLocked = state.lockPlayer;
    const allPlayers = state.getAllAvailablePlayers();
    const activeDev = state.activeDevice;

    return `
      <div class="grownups-screen">
        <div class="gu-header">
          <div class="gu-back" data-act="gu-close" title="Back">&#8592;</div>
          <div class="gu-title">&#9881; Grown-ups Settings</div>
          <button class="gu-pill-btn" style="margin-left:auto; font-size:12px; padding:6px 12px;" data-act="gu-change-pin">🔒 Change PIN</button>
        </div>

        <div class="gu-scroll">
          <!-- 1. Operating Mode & Profile / Player Control -->
          <div class="gu-section">
            <div class="gu-sec-title">
              <span>Operating Mode</span>
              <span style="font-size:12px; font-weight:600; color:#8c8272;">${isStandalone ? 'Standalone Cloud' : 'StoryDeck Core Managed'}</span>
            </div>
            <div class="gu-row">
              <button class="gu-pill-btn ${!isStandalone ? 'active' : ''}" data-act="gu-set-mode" data-mode="core">Core Managed (Profiles)</button>
              <button class="gu-pill-btn ${isStandalone ? 'active' : ''}" data-act="gu-set-mode" data-mode="standalone">Standalone Cloud (Players)</button>
            </div>
            <div class="gu-info-text">
              ${isStandalone 
                ? '<b>Standalone Mode:</b> Directly mirrors the Yoto account players without child profiles or avatar selection.' 
                : '<b>Core Mode:</b> Connects to StoryDeck Core with multi-child profiles, avatar switching, and curated libraries.'}
            </div>
          </div>

          <!-- 2. Target Player Assignment (ONLY VISIBLE IN STANDALONE MODE) -->
          ${isStandalone ? `
            <div class="gu-section">
              <div class="gu-sec-title">
                <span>Target Player Assignment</span>
                <span style="font-size:12px; font-weight:600; color:#8c8272;">${isLocked ? '🔒 Locked' : '🌐 Roaming'}</span>
              </div>
              <div class="gu-row">
                <button class="gu-pill-btn ${!isLocked ? 'active' : ''}" data-act="gu-set-lock" data-lock="false">Choose on the fly (Roaming)</button>
                <button class="gu-pill-btn ${isLocked ? 'active' : ''}" data-act="gu-set-lock" data-lock="true">Lock to Specific Player</button>
              </div>
              ${isLocked ? `
                <div style="margin-top:4px;">
                  <label style="font-size:13px; font-weight:600; color:#4a453e; display:block; margin-bottom:4px;">Assigned Player for this Screen:</label>
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
                When locked, this controller acts as a dedicated bedside remote for that specific Yoto player.
              </div>
            </div>
          ` : `
            <div class="gu-section" style="background:#faf8f3; border-style:dashed;">
              <div class="gu-sec-title" style="color:#71695f;">
                <span>Target Player Assignment</span>
                <span style="font-size:11px; font-weight:600; color:#16a34a;">Managed by Core Profile</span>
              </div>
              <div class="gu-info-text">
                In <b>Core Managed Mode</b>, active playback and device assignments are managed automatically by your StoryDeck Core child profile (<code>${Utils.esc(state.activeProfile?.display_name || 'Active Profile')}</code>) and Hardware settings.
              </div>
            </div>
          `}



          <!-- 4. Sleep Timer (Bedtime vs All Times) -->
          <div class="gu-section">
            <div class="gu-sec-title">
              <span>Sleep Timer</span>
              <span style="font-size:12px; font-weight:600; color:#8c8272;">
                ${state.parentSettings.sleepTimerMin ? `${state.parentSettings.sleepTimerMin} min (${state.parentSettings.sleepTimerMode === 'bedtime' ? 'Bedtime Only' : 'All the Time'})` : 'Off'}
              </span>
            </div>
            <div class="gu-row" style="margin-bottom: 8px;">
              ${[0, 15, 30, 45, 60].map(m => `
                <button class="gu-pill-btn ${state.parentSettings.sleepTimerMin === m ? 'active' : ''}" data-act="gu-set-sleep" data-val="${m}">
                  ${m === 0 ? 'Off' : `${m}m`}
                </button>
              `).join('')}
            </div>
            ${state.parentSettings.sleepTimerMin > 0 ? `
              <div class="gu-row" style="margin-bottom: 6px;">
                <button class="gu-pill-btn ${state.parentSettings.sleepTimerMode !== 'always' ? 'active' : ''}" data-act="gu-set-sleep-scope" data-val="bedtime">🌙 Bedtime Hours Only</button>
                <button class="gu-pill-btn ${state.parentSettings.sleepTimerMode === 'always' ? 'active' : ''}" data-act="gu-set-sleep-scope" data-val="always">☀️ All the Time (Day & Night)</button>
              </div>
            ` : ''}
            <div class="gu-info-text">
              ${state.parentSettings.sleepTimerMin === 0 
                ? 'Automatically stops playback on the player after the timer elapses.' 
                : (state.parentSettings.sleepTimerMode === 'bedtime' 
                    ? `Only arms between Bedtime (${state.parentSettings.bedtime}) and Wake Time (${state.parentSettings.wakeTime}). Day playback will continue uninterrupted.` 
                    : `Arms on every playback session all day long.`)}
            </div>
          </div>

          <!-- 4b. Remote / Travel Player Protection -->
          <div class="gu-section">
            <div class="gu-sec-title">
              <span>Travel & Away Player Protection</span>
              <span style="font-size:12px; font-weight:600; color:${state.blockRemotePlayback ? '#16a34a' : '#ea580c'};">
                ${state.blockRemotePlayback ? '🛡️ Protected (Blocked)' : '⚠️ Prompts on Away'}
              </span>
            </div>
            <div class="gu-row">
              <button class="gu-pill-btn ${state.blockRemotePlayback ? 'active' : ''}" data-act="gu-set-remote-block" data-val="true">Block Away Players</button>
              <button class="gu-pill-btn ${!state.blockRemotePlayback ? 'active' : ''}" data-act="gu-set-remote-block" data-val="false">Prompt Before Playing</button>
            </div>
            <div class="gu-info-text">
              Prevents bedtime sleep timers and accidental card taps from stopping or playing audio on Yoto players traveling away from home (in the car or at a hotel).
            </div>
          </div>

          <!-- 5. Night Clock & Screensaver Delay -->
          <div class="gu-section">
            <div class="gu-sec-title">
              <span>Night Clock / Screensaver</span>
              <span style="font-size:12px; font-weight:600; color:#8c8272;">
                ${state.parentSettings.screensaverMin ? `After ${state.parentSettings.screensaverMin} min` : 'Off'}
              </span>
            </div>
            <div class="gu-row">
              ${[0, 5, 10, 30].map(m => `
                <button class="gu-pill-btn ${state.parentSettings.screensaverMin === m ? 'active' : ''}" data-act="gu-set-saver" data-val="${m}">
                  ${m === 0 ? 'Off' : `${m} min`}
                </button>
              `).join('')}
            </div>
            <div class="gu-info-text">Shows dim clock digits when idle to avoid nighttime glare.</div>
          </div>

          <!-- 6. Bedtime & OK-to-Wake Schedules -->
          <div class="gu-section">
            <div class="gu-sec-title">
              <span>Bedtime & OK-to-Wake Times</span>
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
          </div>

          <!-- 7. Device & Web Portal Info -->
          <div class="gu-section">
            <div class="gu-sec-title">
              <span>System & Web Settings Portal</span>
            </div>
            <div class="gu-info-text" style="font-family:monospace; font-size:12px; line-height:1.6; color:#554f46;">
              <b>Web Settings Portal:</b> <a href="/settings" target="_blank" style="color:var(--red,#e5352b); text-decoration:none;">http://${window.location.host}/settings</a><br>
              <b>Active Target Player:</b> ${Utils.esc(activeDev?.name || 'None')} (${activeDev?.id || 'N/A'})<br>
              <b>Status:</b> ${activeDev?.online ? 'Online' : 'Offline'} &bull; ${activeDev?.is_playing ? 'Playing' : 'Idle'}<br>
              <b>Display Resolution:</b> ${window.innerWidth} &times; ${window.innerHeight} (Touch Screen)<br>
            </div>
          </div>
        </div>
      </div>
    `;
  }
};
