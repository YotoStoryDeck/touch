import { CONFIG, Utils } from './config.js';
import { state } from './state.js';
import { BrokerApi } from './broker-api.js';
import { ProfileScreen } from './screens/profile.js';
import { CatalogScreen } from './screens/catalog.js';
import { DetailScreen } from './screens/detail.js';
import { PlayerScreen } from './screens/player.js';
import { GrownUpsScreen } from './screens/grownups.js';
import { PinScreen } from './screens/pin.js';

/**
 * Main Controller & Event Router
 */
const App = {
  container: null,
  statusBadge: null,

  init() {
    this.container = document.getElementById('app');
    this.statusBadge = document.getElementById('brokerStatus');

    // Check for Kiosk / Fullscreen parameter (?kiosk=1, ?fullscreen=true, or ?tablet=1)
    const params = new URLSearchParams(window.location.search);
    if (params.get('kiosk') === '1' || params.get('kiosk') === 'true' ||
        params.get('fullscreen') === '1' || params.get('fullscreen') === 'true' ||
        params.get('tablet') === '1') {
      document.body.classList.add('kiosk-mode');
    }

    this.bindEvents();
    this.bindGearHoldTimer();
    this.startSleepTimerTicker();
    this.bootstrap();
  },

  /**
   * Confirms playback has successfully initiated on the physical player before switching to Now Playing.
   */
  async initiatePlayback(card, chapterIndex = 0, triggerEl = null) {
    if (!card) return;

    // Safety Interlock: Away/Traveling Player Guard
    const activeDev = state.activeDevice;
    if (activeDev && activeDev.online && activeDev.is_home === false) {
      if (state.blockRemotePlayback) {
        alert(`${activeDev.name} is currently away from home (traveling).

Remote playback from this bedside controller is blocked to avoid disturbing someone traveling.`);
        return;
      } else {
        const proceed = confirm(`${activeDev.name} is currently away from home (traveling).

Are you sure you want to start audio playback on this remote player?`);
        if (!proceed) return;
      }
    }
    const originalText = triggerEl ? triggerEl.innerHTML : null;
    if (triggerEl) {
      triggerEl.style.opacity = '0.6';
      triggerEl.style.pointerEvents = 'none';
      if (triggerEl.classList.contains('playbtn')) {
        triggerEl.innerHTML = '&#9203; Starting...';
      }
    }

    try {
      const res = await BrokerApi.playMedia(card, chapterIndex);
      if (res && (res.ok || res.status === 'success')) {
        state.setCard(card);
        state.setPreviewCard(card);
        state.setChapter(chapterIndex);
        state.setPlaying(true);
        state.localSessionActive = true;
        
        // Immediately fetch fresh device status to verify live playback
        await BrokerApi.getDeviceConfig();
        this.syncActiveDevicePlayback();

        // Transition to Now Playing screen now that playback is confirmed
        state.setScreen('now');
        this.render();
      } else {
        const msg = res?.error || res?.message || 'Playback failed to start';
        alert(`Could not start playback: ${msg}`);
        if (triggerEl && originalText) {
          triggerEl.innerHTML = originalText;
          triggerEl.style.opacity = '1';
          triggerEl.style.pointerEvents = 'auto';
        }
      }
    } catch (err) {
      console.error('Playback initiation error:', err);
      alert(`Playback error: ${err.message || err}`);
      if (triggerEl && originalText) {
        triggerEl.innerHTML = originalText;
        triggerEl.style.opacity = '1';
        triggerEl.style.pointerEvents = 'auto';
      }
    }
  },

  render() {
    if (!this.container) return;

    // Preserve scroll positions for scrollable grids across renders
    const activeScrollEl = this.container.querySelector('.grid, .grid-series, .chapters');
    const savedScrollTop = activeScrollEl ? activeScrollEl.scrollTop : 0;
    const savedScrollLeft = activeScrollEl ? activeScrollEl.scrollLeft : 0;

    switch (state.screen) {
      case 'profile':
        this.container.innerHTML = ProfileScreen.render();
        break;
      case 'catalog':
        this.container.innerHTML = CatalogScreen.render();
        break;
      case 'detail':
        this.container.innerHTML = DetailScreen.render();
        break;
      case 'now':
        this.container.innerHTML = PlayerScreen.render();
        break;
      case 'pin':
        this.container.innerHTML = PinScreen.render();
        break;
      case 'grownups':
        this.container.innerHTML = GrownUpsScreen.render();
        this.bindGrownUpsEvents();
        break;
      case 'offline':
        this.container.innerHTML = `
          <div class="profile-screen" style="text-align:center; padding:40px;">
            <div style="font-size:48px; margin-bottom:16px;">🔌</div>
            <div class="profile-title">BROKER OFFLINE</div>
            <div class="profile-subtitle" style="margin-top:12px; max-width:480px; line-height:1.5;">
              Connection lost to <code>${Utils.esc(CONFIG.BROKER_BASE)}</code>.<br>
              Pinging broker for recovery... You will be returned to your screen as soon as connection restores.
            </div>
          </div>
        `;
        break;
      default:
        this.container.innerHTML = ProfileScreen.render();
    }

    // Restore scroll position
    if (savedScrollTop > 0 || savedScrollLeft > 0) {
      const restoredEl = this.container.querySelector('.grid, .grid-series, .chapters');
      if (restoredEl) {
        restoredEl.scrollTop = savedScrollTop;
        restoredEl.scrollLeft = savedScrollLeft;
      }
    }
  },

  async selectProfile(profileId) {
    const p = state.profiles.find(x => x.id === profileId);
    if (!p) return;

    state.setActiveProfile(p);
    state.setScreen('catalog');
    this.render();

    // Fetch dynamic live catalog for selected child
    try {
      await BrokerApi.fetchLiveCatalog(p.id);
      
      // Auto-bind active player's playback state to the Nowbar
      this.syncActiveDevicePlayback();
      this.render();
    } catch (e) {
      console.error('Failed to load profile catalog:', e);
    }
  },

  syncActiveDevicePlayback() {
    const dev = state.activeDevice;
    const hasCard = dev && (dev.has_card || dev.is_playing || dev.now_playing || dev.item_id);

    if (hasCard) {
      state.isPlaying = Boolean(dev.is_playing);
      if (dev.volume != null && state.volume === 50) {
        state.volume = Math.min(dev.volume_max != null ? dev.volume_max : 100, dev.volume);
      }

      // Find matching item in catalog by ID or Title
      const matched = state.catalog.find(c => 
        (dev.item_id && (c.id === dev.item_id || c.cardId === dev.item_id)) ||
        (dev.now_playing && c.title && c.title.toLowerCase().includes(dev.now_playing.toLowerCase())) ||
        (dev.now_playing && dev.now_playing.toLowerCase().includes(c.title ? c.title.toLowerCase() : '---'))
      );

      const coverUrl = (matched && (matched.cover || matched.img))
        ? (matched.cover || matched.img)
        : (dev.cover_url || (dev.item_id ? `${CONFIG.BROKER_BASE}/api/thumb?type=abs&id=${encodeURIComponent(dev.item_id)}&size=300` : null));

      const fullTitle = dev.now_playing || matched?.title || 'Audiobook';
      const chapterTitle = dev.chapter_title || (matched?.chapters?.[state.chapterIndex]?.title) || 'Playing Track';
      const author = dev.author || matched?.author || '';

      // Only overwrite state.card if user isn't currently browsing an alternate card detail view, or if state.card matches
      if (!state.card || state.screen !== 'detail' || (matched && (state.card.id === matched.id || state.card.cardId === matched.cardId))) {
        state.card = {
          id: dev.item_id || matched?.id || 'remote-play',
          title: fullTitle,
          chapter_title: chapterTitle,
          author: author,
          cover: coverUrl || 'https://placehold.co/122x193/2b2622/ffffff?text=Playing',
          duration: dev.duration || matched?.duration || 1800,
          chapters: matched?.chapters || [{ title: chapterTitle, duration: dev.duration || 1800 }]
        };
      }
    } else {
      // No card in player: clear live playing card & playing state;
      // return to catalog if currently on Now Playing (preview in Detail screen remains accessible)
      state.isPlaying = false;
      state.card = null;
      if (state.screen === 'now') {
        state.setScreen('catalog');
      }
    }
  },

  bindEvents() {
    this.container.addEventListener('change', (e) => {
      const guLockSelect = e.target.closest('[data-act="gu-select-lock-player"]');
      if (guLockSelect) {
        const did = guLockSelect.value;
        const allPlayers = state.getAllAvailablePlayers();
        const found = allPlayers.find(p => p.id === did);
        if (found) {
          state.setActiveDevice(found);
          state.lockedPlayerId = found.id;
          state.persistParentSettings();
          this.syncActiveDevicePlayback();
        }
      }
    });

    this.container.addEventListener('click', (e) => {
      // 1. Profile Card Selection
      const card = e.target.closest('[data-profile]');
      if (card) {
        this.selectProfile(card.dataset.profile);
        return;
      }

      // 2. Profile Switcher Pill
      const switcher = e.target.closest('[data-act="switch-profile"]');
      if (switcher) {
        state.setScreen('profile');
        this.render();
        return;
      }

      // 2b. Device Picker Toggle
      const devToggle = e.target.closest('[data-act="toggle-device-picker"]');
      if (devToggle) {
        state.toggleDevicePicker();
        this.render();
        return;
      }

      // 2c. Target Device Selection
      const devOpt = e.target.closest('[data-act="select-device"]');
      if (devOpt) {
        const did = devOpt.dataset.deviceId;
        const target = (state.activeProfile?.players || []).find(p => p.id === did);
        if (target) {
          state.setActiveDevice(target);
          this.syncActiveDevicePlayback();
          this.render();
        }
        return;
      }

      // Close device picker if clicking elsewhere
      if (state.isDevicePickerOpen && !e.target.closest('.device-pill-wrap')) {
        state.isDevicePickerOpen = false;
        this.render();
      }

      // 3. Tab Navigation (All / ABS / Yoto)
      const tab = e.target.closest('[data-tab]');
      if (tab) {
        state.setActiveTab(tab.dataset.tab);
        this.render();
        return;
      }

      // 4. Media Card Selection (Catalog Grid)
      const tile = e.target.closest('[data-card]');
      if (tile) {
        const id = tile.dataset.card;
        const found = state.catalog.find(c => (c.id || c.cardId) === id);
        if (found) {
          state.setPreviewCard(found);
          // If no active card is playing, also set state.card
          if (!state.card || !state.isPlaying) {
            state.setCard(found);
          }
          state.setScreen('detail');
          this.render();

          // Fetch real tracklist / chapters from Broker if not yet cached on the card
          if (!found.chapters || found.chapters.length === 0) {
            BrokerApi.fetchMediaTracks(id).then(tracks => {
              if (tracks && tracks.length > 0) {
                found.chapters = tracks.map(t => ({
                  title: t.title,
                  duration: t.duration_seconds || 0,
                  icon_url: t.icon_url
                }));
                found.duration = tracks.reduce((sum, t) => sum + (t.duration_seconds || 0), 0);
                if (state.card && (state.card.id || state.card.cardId) === id) {
                  state.card = { ...found };
                  this.render();
                }
              }
            });
          }
        }
        return;
      }

      // Change PIN button inside Grown-ups screen
      const guChangePin = e.target.closest('[data-act="gu-change-pin"]');
      if (guChangePin) {
        state.previousScreen = 'grownups';
        state.currentEnteredPin = '';
        state.isPinError = false;
        state.pinMode = 'change';
        state.setScreen('pin');
        this.render();
        return;
      }

      // PIN Cancel Button
      const pinCancel = e.target.closest('[data-act="pin-cancel"]');
      if (pinCancel) {
        const returnScreen = state.previousScreen || (state.mode === 'standalone' ? 'catalog' : 'profile');
        state.previousScreen = null;
        state.currentEnteredPin = '';
        state.isPinError = false;
        state.setScreen(returnScreen);
        this.render();
        return;
      }

      // PIN Keypad Tap
      const pinKeyBtn = e.target.closest('.pin-key');
      if (pinKeyBtn && state.screen === 'pin') {
        const key = pinKeyBtn.dataset.key;
        if (key === 'clear') {
          state.currentEnteredPin = '';
          state.isPinError = false;
          this.render();
          return;
        }
        if (key === 'back') {
          state.currentEnteredPin = (state.currentEnteredPin || '').slice(0, -1);
          state.isPinError = false;
          this.render();
          return;
        }
        if (key != null && (state.currentEnteredPin || '').length < 4) {
          state.currentEnteredPin = (state.currentEnteredPin || '') + key;
          state.isPinError = false;
          this.render();

          // Check if 4 digits reached
          if (state.currentEnteredPin.length === 4) {
            const entered = state.currentEnteredPin;
            setTimeout(() => {
              if (state.pinMode === 'unlock') {
                if (entered === state.parentPin) {
                  state.currentEnteredPin = '';
                  state.setScreen('grownups');
                  this.render();
                } else {
                  state.isPinError = true;
                  state.currentEnteredPin = '';
                  this.render();
                  setTimeout(() => {
                    state.isPinError = false;
                    this.render();
                  }, 400);
                }
              } else if (state.pinMode === 'setup') {
                state.tempSetupPin = entered;
                state.currentEnteredPin = '';
                state.pinMode = 'confirm';
                this.render();
              } else if (state.pinMode === 'confirm') {
                if (entered === state.tempSetupPin) {
                  state.parentPin = entered;
                  state.persistParentSettings();
                  state.tempSetupPin = '';
                  state.currentEnteredPin = '';
                  state.setScreen('grownups');
                  this.render();
                } else {
                  state.isPinError = true;
                  state.currentEnteredPin = '';
                  this.render();
                  setTimeout(() => {
                    state.pinMode = 'setup';
                    state.isPinError = false;
                    this.render();
                  }, 500);
                }
              } else if (state.pinMode === 'change') {
                if (entered === state.parentPin) {
                  state.currentEnteredPin = '';
                  state.pinMode = 'setup';
                  this.render();
                } else {
                  state.isPinError = true;
                  state.currentEnteredPin = '';
                  this.render();
                  setTimeout(() => {
                    state.isPinError = false;
                    this.render();
                  }, 400);
                }
              }
            }, 100);
          }
        }
        return;
      }

      // Grown-ups Close Button
      const guClose = e.target.closest('[data-act="gu-close"]');
      if (guClose) {
        const returnScreen = state.previousScreen || (state.mode === 'standalone' ? 'catalog' : 'profile');
        state.previousScreen = null;
        state.setScreen(returnScreen);
        this.render();
        return;
      }

      // Grown-ups Mode Switch
      const guModeBtn = e.target.closest('[data-act="gu-set-mode"]');
      if (guModeBtn) {
        const newMode = guModeBtn.dataset.mode;
        state.mode = newMode;
        state.persistParentSettings();
        if (newMode === 'standalone') {
          // Switch directly to catalog without profile requirement
          const allPlayers = state.getAllAvailablePlayers();
          if (!state.activeDevice && allPlayers.length > 0) {
            state.setActiveDevice(allPlayers[0]);
          }
          if (state.profiles.length > 0) {
            BrokerApi.fetchLiveCatalog(state.profiles[0].id).then(() => {
              this.syncActiveDevicePlayback();
            });
          }
        }
        this.render();
        return;
      }

      // Grown-ups Lock Player Toggle
      const guLockBtn = e.target.closest('[data-act="gu-set-lock"]');
      if (guLockBtn) {
        state.lockPlayer = (guLockBtn.dataset.lock === 'true');
        if (state.lockPlayer && state.activeDevice) {
          state.lockedPlayerId = state.activeDevice.id;
        }
        state.persistParentSettings();
        this.render();
        return;
      }

      // Grown-ups Lock Player Selection
      const guLockSelect = e.target.closest('[data-act="gu-select-lock-player"]');
      if (guLockSelect && e.type === 'change') {
        const did = guLockSelect.value;
        const allPlayers = state.getAllAvailablePlayers();
        const found = allPlayers.find(p => p.id === did);
        if (found) {
          state.setActiveDevice(found);
          state.lockedPlayerId = found.id;
          state.persistParentSettings();
          this.syncActiveDevicePlayback();
        }
        return;
      }

      // Grown-ups Sleep Timer Duration
      const guSleepBtn = e.target.closest('[data-act="gu-set-sleep"]');
      if (guSleepBtn) {
        state.parentSettings.sleepTimerMin = parseInt(guSleepBtn.dataset.val, 10);
        state.persistParentSettings();
        this.checkAndArmSleepTimer(true);
        this.render();
        return;
      }

      // Grown-ups Sleep Timer Scope (Bedtime vs Always)
      const guSleepScopeBtn = e.target.closest('[data-act="gu-set-sleep-scope"]');
      if (guSleepScopeBtn) {
        state.parentSettings.sleepTimerMode = guSleepScopeBtn.dataset.val;
        state.persistParentSettings();
        this.checkAndArmSleepTimer(true);
        this.render();
        return;
      }

      // Grown-ups Remote/Travel Block Toggle
      const guRemoteBlockBtn = e.target.closest('[data-act="gu-set-remote-block"]');
      if (guRemoteBlockBtn) {
        state.blockRemotePlayback = (guRemoteBlockBtn.dataset.val === 'true');
        state.persistParentSettings();
        this.render();
        return;
      }

      // Grown-ups Screensaver Delay
      const guSaverBtn = e.target.closest('[data-act="gu-set-saver"]');
      if (guSaverBtn) {
        state.parentSettings.screensaverMin = parseInt(guSaverBtn.dataset.val, 10);
        state.persistParentSettings();
        this.render();
        return;
      }

      // 4b. Inspect Active Card (Tap Cover or Title in Player Screen)
      const inspectCard = e.target.closest('[data-act="inspect-card"]');
      if (inspectCard) {
        if (state.card) {
          state.setPreviewCard(state.card);
          state.setScreen('detail');
          this.render();

          // Fetch tracks if needed
          const id = state.card.id || state.card.cardId;
          if (id && (!state.card.chapters || state.card.chapters.length === 0)) {
            BrokerApi.fetchMediaTracks(id).then(tracks => {
              if (tracks && tracks.length > 0) {
                state.card.chapters = tracks.map(t => ({
                  title: t.title,
                  duration: t.duration_seconds || 0,
                  icon_url: t.icon_url
                }));
                state.card.duration = tracks.reduce((sum, t) => sum + (t.duration_seconds || 0), 0);
                state.previewCard = { ...state.card };
                this.render();
              }
            });
          }
        }
        return;
      }

      // 5. Quick Action Button inside Nowbar (play/pause toggle should NOT navigate)
      const nbPlayBtn = e.target.closest('.nb-play');
      if (nbPlayBtn) {
        const nextState = !state.isPlaying;
        state.setPlaying(nextState);
        this.render();
        const targetAction = nextState ? 'resume' : 'pause';
        BrokerApi.sendPlayerControl(targetAction).then(res => {
          if (!res || !res.ok) {
            console.warn(`Failed to execute ${targetAction}, reverting UI state:`, res);
            state.setPlaying(!nextState);
            this.render();
          }
        });
        return;
      }

      // 5b. Back / Screen Navigation Links
      const go = e.target.closest('[data-go]');
      if (go) {
        state.setScreen(go.dataset.go);
        this.render();
        return;
      }

      // 6. Track / Chapter Row Tap -> Play
      const chrow = e.target.closest('[data-ch]');
      if (chrow) {
        const chIdx = +chrow.dataset.ch;
        const targetCard = state.previewCard || state.card;
        this.initiatePlayback(targetCard, chIdx, chrow);
        return;
      }

      // 7. Player Action Controls
      const actBtn = e.target.closest('[data-act]');
      const act = actBtn?.dataset.act;
      if (act) {
        // Prevent clicking nowbar play from opening the now screen
        if (act === 'toggle' && actBtn.classList.contains('nb-play')) {
          e.stopPropagation();
        }

        if (act === 'playstart') {
          const btn = document.getElementById('btn-play-book') || actBtn;
          const targetCard = state.previewCard || state.card;
          this.initiatePlayback(targetCard, 0, btn);
          return;
        }

        if (act === 'toggle') {
          const nextState = !state.isPlaying;
          state.setPlaying(nextState);
          this.render();
          const targetAction = nextState ? 'resume' : 'pause';
          BrokerApi.sendPlayerControl(targetAction).then(res => {
            if (!res || !res.ok) {
              console.warn(`Failed to execute ${targetAction}, reverting UI state:`, res);
              state.setPlaying(!nextState);
              this.render();
            }
          });
          return;
        }

        if (act === 'next') {
          BrokerApi.sendPlayerControl('next').then(res => {
            if (res && res.ok) {
              if (state.card && state.chapterIndex < (state.card.chapters || []).length - 1) {
                state.setChapter(state.chapterIndex + 1);
              }
              BrokerApi.getDeviceConfig().then(() => {
                this.syncActiveDevicePlayback();
                this.render();
              });
            } else {
              console.warn('Next track error:', res);
            }
          });
          return;
        }

        if (act === 'prev') {
          BrokerApi.sendPlayerControl('prev').then(res => {
            if (res && res.ok) {
              if (res.data?.track) {
                state.setChapter(Math.max(0, res.data.track - 1));
              } else if (state.chapterIndex > 0) {
                state.setChapter(state.chapterIndex - 1);
              }
              BrokerApi.getDeviceConfig().then(() => {
                this.syncActiveDevicePlayback();
                this.render();
              });
            } else {
              console.warn('Prev track error:', res);
            }
          });
          return;
        }

        if (act === 'vol-down') {
          const maxLimit = (state.activeDevice && state.activeDevice.volume_max != null) ? state.activeDevice.volume_max : 100;
          const cur = state.volume != null ? state.volume : (state.activeDevice?.volume != null ? state.activeDevice.volume : 50);
          const newVol = Math.max(0, Math.min(maxLimit, cur - 10));
          state.volume = newVol;
          this.render();
          BrokerApi.sendPlayerControl('volume', { volume: newVol });
          return;
        }

        if (act === 'vol-up') {
          const maxLimit = (state.activeDevice && state.activeDevice.volume_max != null) ? state.activeDevice.volume_max : 100;
          const cur = state.volume != null ? state.volume : (state.activeDevice?.volume != null ? state.activeDevice.volume : 50);
          const newVol = Math.min(maxLimit, cur + 10);
          state.volume = newVol;
          this.render();
          BrokerApi.sendPlayerControl('volume', { volume: newVol });
          return;
        }

        if (act === 'vol-set') {
          const maxLimit = (state.activeDevice && state.activeDevice.volume_max != null) ? state.activeDevice.volume_max : 100;
          const rect = actBtn.getBoundingClientRect();
          const clickX = e.clientX - rect.left;
          const ratio = Math.max(0, Math.min(1, clickX / rect.width));
          const rawVol = Math.round(ratio * 100);
          const newVol = Math.min(maxLimit, rawVol);
          state.volume = newVol;
          this.render();
          BrokerApi.sendPlayerControl('volume', { volume: newVol });
          return;
        }
      }
    });
  },

  bindGearHoldTimer() {
    let holdTimeout = null;

    const startHold = (e) => {
      const gearBtn = e.target.closest('[data-act="gear-button"]');
      if (!gearBtn) return;
      gearBtn.classList.add('pressing');
      if (holdTimeout) clearTimeout(holdTimeout);
      holdTimeout = setTimeout(() => {
        gearBtn.classList.remove('pressing');
        holdTimeout = null;
        state.previousScreen = state.screen;
        state.currentEnteredPin = '';
        state.isPinError = false;
        if (!state.parentPin) {
          state.pinMode = 'setup';
        } else {
          state.pinMode = 'unlock';
        }
        state.setScreen('pin');
        this.render();
      }, 2500);
    };

    const cancelHold = (e) => {
      const gearBtn = document.querySelector('.nav-gear-btn');
      if (gearBtn) gearBtn.classList.remove('pressing');
      if (holdTimeout) {
        clearTimeout(holdTimeout);
        holdTimeout = null;
      }
    };

    document.addEventListener('pointerdown', startHold);
    document.addEventListener('pointerup', cancelHold);
    document.addEventListener('pointercancel', cancelHold);
    document.addEventListener('pointerleave', cancelHold);
  },

  bindGrownUpsEvents() {
    // Parent volume ceiling slider removed - volume limits are dynamically queried per-device from Yoto

    const bedtimeInput = document.getElementById('guBedtimeInput');
    if (bedtimeInput) {
      bedtimeInput.addEventListener('change', (e) => {
        state.parentSettings.bedtime = e.target.value;
        state.persistParentSettings();
      });
    }

    const wakeInput = document.getElementById('guWakeInput');
    if (wakeInput) {
      wakeInput.addEventListener('change', (e) => {
        state.parentSettings.wakeTime = e.target.value;
        state.persistParentSettings();
      });
    }
  },

  async bootstrap() {
    try {
      // 1. Device Registration Handshake
      const regResult = await BrokerApi.registerDevice();
      if (regResult.status === 'pending_approval') {
        this.statusBadge.textContent = 'Device Quarantined (Pending Approval)';
        this.statusBadge.className = 'broker-badge connecting';
        this.container.innerHTML = `
          <div class="profile-screen" style="text-align:center; padding:40px;">
            <div style="font-size:48px; margin-bottom:16px;">⏳</div>
            <div class="profile-title">DEVICE PENDING APPROVAL</div>
            <div class="profile-subtitle" style="margin-top:12px; max-width:480px; line-height:1.5;">
              Device <code>${Utils.esc(CONFIG.DEVICE_ID)}</code> has announced itself to the broker.<br>
              Please approve this device in the Broker Web Admin under <b>Hardware</b> to enable access tokens.<br><br>
              <i>Checking approval status every 5 seconds...</i>
            </div>
          </div>
        `;
        setTimeout(() => this.bootstrap(), 5000);
        return;
      }

      // 2. Discover Hardware Config & Dynamic Profiles
      const conf = await BrokerApi.getDeviceConfig();
      this.statusBadge.textContent = `Connected to Broker (${state.profiles.length} Profiles)`;
      this.statusBadge.className = 'broker-badge';

      // 3. Launch Initial Screen
      if (state.mode === 'standalone') {
        const allPlayers = state.getAllAvailablePlayers();
        if (allPlayers.length > 0) {
          state.setActiveDevice(state.resolveActiveDevice(null));
        }
        if (state.profiles.length > 0) {
          await BrokerApi.fetchLiveCatalog(state.profiles[0].id);
          this.syncActiveDevicePlayback();
        }
        state.setScreen('catalog');
        this.render();
      } else if (conf.assigned_profile && state.profiles.some(p => p.id === conf.assigned_profile)) {
        await this.selectProfile(conf.assigned_profile);
      } else {
        state.setScreen('profile');
        this.render();
      }

      // 4. Start active health & ping monitor with recovery
      this.startHealthMonitor();
    } catch (err) {
      console.warn('Broker communication error:', err);
      this.enterOfflineState();
    }
  },

  /**
   * Transitions UI into offline overlay and remembers prior screen for instant recovery
   */
  enterOfflineState() {
    if (!state.isOffline) {
      state.isOffline = true;
      if (state.screen !== 'offline') {
        state.previousScreen = state.screen;
      }
      state.setScreen('offline');
      this.statusBadge.textContent = 'Broker Offline (Pinging...)';
      this.statusBadge.className = 'broker-badge error';
      this.render();
    }
    // Ping broker every 3 seconds to test for recovery
    if (!this._pingTimer) {
      this._pingTimer = setInterval(async () => {
        const ok = await BrokerApi.ping();
        if (ok) {
          clearInterval(this._pingTimer);
          this._pingTimer = null;
          await this.recoverFromOffline();
        }
      }, 3000);
    }
  },

  /**
   * Recovers from offline state, refreshes config/catalog, and restores previous screen
   */
  async recoverFromOffline() {
    console.log('Broker ping restored! Recovering controller state...');
    state.isOffline = false;
    this.statusBadge.textContent = 'Reconnecting...';
    this.statusBadge.className = 'broker-badge connecting';
    try {
      // Re-register if needed
      await BrokerApi.registerDevice();
      const conf = await BrokerApi.getDeviceConfig();
      if (state.activeProfile) {
        await BrokerApi.fetchLiveCatalog(state.activeProfile.id);
        this.syncActiveDevicePlayback();
      }
      this.statusBadge.textContent = `Connected to Broker (${state.profiles.length} Profiles)`;
      this.statusBadge.className = 'broker-badge';

      // Restore user's previous screen
      const returnTo = state.previousScreen || (state.activeProfile ? 'catalog' : 'profile');
      state.previousScreen = null;
      state.setScreen(returnTo);
      this.render();
      console.log(`Controller successfully restored to screen: ${returnTo}`);
    } catch (e) {
      console.warn('Error during recovery, re-entering offline wait:', e);
      this.enterOfflineState();
    }
  },

  /**
   * Returns true if current wall-clock time falls within the configured bedtime window.
   */
  isBedtimeNow() {
    const bedtimeStr = state.parentSettings.bedtime || '19:00';
    const wakeTimeStr = state.parentSettings.wakeTime || '07:00';
    const [bH, bM] = bedtimeStr.split(':').map(Number);
    const [wH, wM] = wakeTimeStr.split(':').map(Number);
    const bMinutes = bH * 60 + bM;
    const wMinutes = wH * 60 + wM;

    const now = new Date();
    const curMinutes = now.getHours() * 60 + now.getMinutes();

    if (bMinutes > wMinutes) {
      // Overnight schedule (e.g. 19:00 -> 07:00)
      return (curMinutes >= bMinutes || curMinutes < wMinutes);
    } else {
      // Same-day schedule
      return (curMinutes >= bMinutes && curMinutes < wMinutes);
    }
  },

  /**
   * Evaluates if sleep timer should arm or reset upon playback state change or setting modification.
   */
  checkAndArmSleepTimer(forceReset = false) {
    const mins = state.parentSettings.sleepTimerMin || 0;
    if (mins <= 0) {
      state.sleepTimerRemainingSec = 0;
      return;
    }

    const mode = state.parentSettings.sleepTimerMode || 'bedtime';
    const shouldArm = (mode === 'always') || this.isBedtimeNow();

    if (!shouldArm) {
      state.sleepTimerRemainingSec = 0;
      return;
    }

    const isPlaying = Boolean(state.isPlaying || state.activeDevice?.is_playing);
    if (isPlaying) {
      if (forceReset || state.sleepTimerRemainingSec <= 0) {
        state.sleepTimerRemainingSec = mins * 60;
        console.log(`[SleepTimer] Armed for ${mins} minutes (${state.sleepTimerRemainingSec}s)`);
      }
    } else if (!isPlaying && forceReset) {
      state.sleepTimerRemainingSec = 0;
    }
  },

  /**
   * Starts 1Hz sleep timer countdown ticker.
   */
  startSleepTimerTicker() {
    if (this._sleepTicker) return;
    this._sleepTicker = setInterval(() => {
      const isPlaying = Boolean(state.isPlaying || state.activeDevice?.is_playing);
      if (!isPlaying) {
        return;
      }

      // Safety Gate 1: Never arm or stop an away/traveling player
      if (state.activeDevice && state.activeDevice.is_home === false) {
        if (state.sleepTimerRemainingSec > 0) {
          console.warn(`[SleepTimer] Active player ${state.activeDevice.name} is AWAY from home. Sleep timer aborted.`);
          state.sleepTimerRemainingSec = 0;
        }
        return;
      }

      // Safety Gate 2: Never stop an external session not initiated on this controller
      if (!state.localSessionActive) {
        return;
      }

      const mins = state.parentSettings.sleepTimerMin || 0;
      if (mins <= 0) {
        state.sleepTimerRemainingSec = 0;
        return;
      }

      const mode = state.parentSettings.sleepTimerMode || 'bedtime';
      if (mode === 'bedtime' && !this.isBedtimeNow()) {
        state.sleepTimerRemainingSec = 0;
        return;
      }

      if (state.sleepTimerRemainingSec > 0) {
        state.sleepTimerRemainingSec--;
        if (state.sleepTimerRemainingSec === 0) {
          console.log('[SleepTimer] Timer reached 0! Stopping player playback...');
          state.setPlaying(false);
          state.localSessionActive = false;
          this.render();
          BrokerApi.sendPlayerControl('stop').then(res => {
            console.log('[SleepTimer] Stop command executed:', res);
          });
        }
      } else if (state.sleepTimerRemainingSec <= 0 && isPlaying) {
        // Arm if playback began during active window on this controller
        this.checkAndArmSleepTimer(false);
      }
    }, 1000);
  },

  /**
   * Continuous background health & telemetry monitor (every 5s)
   */
  startHealthMonitor() {
    if (this._healthTimer) return;
    this._healthTimer = setInterval(async () => {
      if (state.isOffline) return; // Managed by _pingTimer
      try {
        const isAlive = await BrokerApi.ping();
        if (!isAlive) {
          console.warn('Broker ping check failed! Triggering offline mode.');
          this.enterOfflineState();
          return;
        }

        // Active background sync (every 5-10s)
        await BrokerApi.getDeviceConfig();
        this.syncActiveDevicePlayback();
        if (state.screen === 'catalog' || state.screen === 'detail' || state.screen === 'now') {
          this.render();
        }
      } catch (err) {
        console.warn('Health monitor detected communication breakdown:', err);
        this.enterOfflineState();
      }
    }, 5000);
  }
};

// Start application when DOM is ready or immediately if already loaded
if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', () => App.init());
} else {
  App.init();
}

