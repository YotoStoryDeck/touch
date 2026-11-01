/**
 * Central State Store & Screen State Management
 */
export const state = {
  screen: 'profile', // 'profile' | 'catalog' | 'detail' | 'now' | 'grownups' | 'offline'
  previousScreen: null, // screen to return to after recovering from offline or grown-ups
  isOffline: false,
  mode: 'core', // 'core' (default, with child profiles) | 'standalone' (direct Yoto player list, no profiles)
  lockPlayer: false, // true = locked to assigned player; false = choose player on the fly
  lockedPlayerId: null,
  profiles: [],
  catalog: [],
  deviceConfig: { allow_profile_switch: true, assigned_profile: null },
  deviceToken: null,
  activeProfile: null,
  activeDevice: null,
  isDevicePickerOpen: false,
  activeTab: 'all',  // 'all' | 'abs' | 'yoto' | 'series'
  card: null,           // Card currently active / playing on player
  previewCard: null,    // Card currently being inspected in detail view
  chapterIndex: 0,
  isPlaying: false,
  volume: 50,
  parentSettings: {
    volumeMax: 100,
    sleepTimerMin: 0,
    screensaverMin: 10,
    bedtime: '19:00',
    wakeTime: '07:00'
  },
  parentPin: null, // 4-digit string
  pinMode: 'unlock', // 'unlock' | 'setup' | 'confirm' | 'change'
  currentEnteredPin: '',
  tempSetupPin: '',
  isPinError: false,

  init() {
    try {
      const savedMode = localStorage.getItem('storydeck_mode');
      if (savedMode === 'standalone' || savedMode === 'core') this.mode = savedMode;
      const savedLock = localStorage.getItem('storydeck_lock_player');
      if (savedLock !== null) this.lockPlayer = (savedLock === 'true');
      const savedLockId = localStorage.getItem('storydeck_locked_player_id');
      if (savedLockId) this.lockedPlayerId = savedLockId;
      const savedSettings = localStorage.getItem('storydeck_parent_settings');
      if (savedSettings) {
        this.parentSettings = Object.assign(this.parentSettings, JSON.parse(savedSettings));
      }
      const savedPin = localStorage.getItem('storydeck_parent_pin');
      if (savedPin) this.parentPin = savedPin;
    } catch (e) {
      console.warn('Could not read stored settings:', e);
    }
  },

  persistParentSettings() {
    try {
      localStorage.setItem('storydeck_mode', this.mode);
      localStorage.setItem('storydeck_lock_player', String(this.lockPlayer));
      if (this.lockedPlayerId) localStorage.setItem('storydeck_locked_player_id', this.lockedPlayerId);
      localStorage.setItem('storydeck_parent_settings', JSON.stringify(this.parentSettings));
      if (this.parentPin) localStorage.setItem('storydeck_parent_pin', this.parentPin);
    } catch (e) {
      console.warn('Could not persist settings:', e);
    }
  },

  setScreen(newScreen) {
    this.screen = newScreen;
  },

  setActiveProfile(profile) {
    this.activeProfile = profile;
    this.activeDevice = this.resolveActiveDevice(profile);
    this.isDevicePickerOpen = false;
  },

  setActiveDevice(device) {
    this.activeDevice = device;
    if (this.lockPlayer && device) {
      this.lockedPlayerId = device.id;
      this.persistParentSettings();
    }
    this.isDevicePickerOpen = false;
  },

  toggleDevicePicker() {
    this.isDevicePickerOpen = !this.isDevicePickerOpen;
  },

  getAllAvailablePlayers() {
    const map = new Map();
    (this.profiles || []).forEach(p => {
      (p.players || []).forEach(dev => {
        if (!map.has(dev.id)) map.set(dev.id, dev);
      });
    });
    return Array.from(map.values());
  },

  /**
   * Device Priority Resolution:
   * 1. If lockPlayer is enabled and lockedPlayerId exists, return that device.
   * 2. Any device in profile controlled list that's actively playing.
   * 3. A device that is currently online.
   * 4. The default device.
   */
  resolveActiveDevice(profile) {
    const allPlayers = this.getAllAvailablePlayers();
    if (this.lockPlayer && this.lockedPlayerId) {
      const lockedDev = allPlayers.find(p => p.id === this.lockedPlayerId);
      if (lockedDev) return lockedDev;
    }

    if (!profile) {
      return allPlayers.find(p => p.online) || allPlayers[0] || null;
    }

    const players = profile.players || [];
    if (!players.length) {
      return { id: profile.default_player_id, name: profile.default_player_name || 'Yoto Player', online: false };
    }

    // 1. Any device actively playing
    const playing = players.find(p => p.is_playing);
    if (playing) return playing;

    // 2. A device that is currently online
    const online = players.find(p => p.online);
    if (online) return online;

    // 3. The default device
    const def = players.find(p => p.id === profile.default_player_id);
    if (def) return def;

    return players[0];
  },

  setActiveTab(tab) {
    this.activeTab = tab;
  },

  setCard(card) {
    this.card = card;
  },
  setPreviewCard(card) {
    this.previewCard = card;
  },

  setChapter(idx) {
    this.chapterIndex = idx;
  },

  setPlaying(playing) {
    this.isPlaying = playing;
  }
};

state.init();
