/**
 * Central State Store & Screen State Management
 */
export const state = {
  screen: 'profile', // 'profile' | 'catalog' | 'detail' | 'now' | 'offline'
  previousScreen: null, // screen to return to after recovering from offline
  isOffline: false,
  profiles: [],
  catalog: [],
  deviceConfig: { allow_profile_switch: true, assigned_profile: null },
  deviceToken: null,
  activeProfile: null,
  activeDevice: null,
  isDevicePickerOpen: false,
  activeTab: 'all',  // 'all' | 'abs' | 'yoto' | 'series'
  card: null,
  chapterIndex: 0,
  isPlaying: false,

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
    this.isDevicePickerOpen = false;
  },

  toggleDevicePicker() {
    this.isDevicePickerOpen = !this.isDevicePickerOpen;
  },

  /**
   * Device Priority Resolution:
   * 1. Any device in profile controlled list that's actively playing
   * 2. A device that is currently online
   * 3. The default device
   */
  resolveActiveDevice(profile) {
    if (!profile) return null;
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

  setChapter(idx) {
    this.chapterIndex = idx;
  },

  setPlaying(playing) {
    this.isPlaying = playing;
  }
};
