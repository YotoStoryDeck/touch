import { CONFIG, Utils } from './config.js';
import { state } from './state.js';
import { BrokerApi } from './broker-api.js';
import { ProfileScreen } from './screens/profile.js';
import { CatalogScreen } from './screens/catalog.js';
import { DetailScreen } from './screens/detail.js';
import { PlayerScreen } from './screens/player.js';

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
    this.bootstrap();
  },

  render() {
    if (!this.container) return;

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
    if (dev && dev.is_playing) {
      state.isPlaying = true;

      // Find matching item in catalog by ID or Title
      const matched = state.catalog.find(c => 
        (dev.item_id && (c.id === dev.item_id || c.cardId === dev.item_id)) ||
        (dev.now_playing && c.title && c.title.toLowerCase().includes(dev.now_playing.toLowerCase())) ||
        (dev.now_playing && dev.now_playing.toLowerCase().includes(c.title ? c.title.toLowerCase() : '---'))
      );

      const coverUrl = (matched && (matched.cover || matched.img))
        ? (matched.cover || matched.img)
        : (dev.item_id ? `${CONFIG.BROKER_BASE}/api/thumb?type=abs&id=${encodeURIComponent(dev.item_id)}&size=300` : null);

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
      // If nothing is playing on this device, clear isPlaying state
      state.isPlaying = false;
    }
  },

  bindEvents() {
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
          state.setCard(found);
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

      // 5. Back / Screen Navigation Links
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
        state.setChapter(chIdx);
        state.setPlaying(true);
        state.setScreen('now');
        this.render();
        BrokerApi.playMedia(state.card, chIdx).then(res => {
          if (!res || !res.ok) {
            console.error('Playback error:', res?.error || 'Unknown error');
          }
        });
        return;
      }

      // 7. Player Action Controls
      const act = e.target.closest('[data-act]')?.dataset.act;
      if (act === 'playstart') {
        state.setChapter(0);
        state.setPlaying(true);
        state.setScreen('now');
        this.render();
        BrokerApi.playMedia(state.card, 0).then(res => {
          if (!res || !res.ok) {
            console.error('Playback error:', res?.error || 'Unknown error');
          }
        });
        return;
      }
      if (act === 'toggle') {
        state.setPlaying(!state.isPlaying);
        this.render();
        return;
      }
      if (act === 'next') {
        if (state.card && state.chapterIndex < (state.card.chapters || []).length - 1) {
          state.setChapter(state.chapterIndex + 1);
          this.render();
        }
        return;
      }
      if (act === 'prev') {
        if (state.chapterIndex > 0) {
          state.setChapter(state.chapterIndex - 1);
          this.render();
        }
        return;
      }
    });
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
      if (conf.assigned_profile && state.profiles.some(p => p.id === conf.assigned_profile)) {
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

