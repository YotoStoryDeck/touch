import { CONFIG } from './config.js';

import { state } from './state.js';



/**

 * Broker REST API Service

 * Handles device announcement, token rotation, profile discovery, and catalog fetching.

 */

export const BrokerApi = {

    /**
   * Fast health probe ping to check if broker is online
   */
  async ping() {
    try {
      const controller = new AbortController();
      const id = setTimeout(() => controller.abort(), 3000);
      const res = await fetch(`${CONFIG.BROKER_BASE}/health`, { signal: controller.signal });
      clearTimeout(id);
      return res.ok;
    } catch {
      return false;
    }
  },

/**

   * Registers the virtual device and acquires rotating JWT tokens

   */

  async registerDevice() {

    const res = await fetch(`${CONFIG.BROKER_BASE}/api/devices/register`, {

      method: 'POST',

      headers: { 'Content-Type': 'application/json' },

      body: JSON.stringify({

        mac: CONFIG.DEVICE_ID,

        friendly_name: CONFIG.FRIENDLY_NAME,

        firmware_version: CONFIG.FIRMWARE_VERSION

      })

    });



    if (res.status === 202) {

      return { status: 'pending_approval' };

    }



    if (res.ok) {

      const data = await res.json();

      if (data.tokens?.access_token) {

        state.deviceToken = data.tokens.access_token;

      }

      return { status: 'approved', data };

    }



    throw new Error(`Device registration failed with status ${res.status}`);

  },



  /**

   * Queries broker configuration & profile summaries

   */

  async getDeviceConfig() {

    const res = await fetch(`${CONFIG.BROKER_BASE}/api/device/config?device_id=${encodeURIComponent(CONFIG.DEVICE_ID)}`);

    if (!res.ok) throw new Error(`Config fetch failed: ${res.status}`);

    const conf = await res.json();



    state.deviceConfig = conf;

        if (conf.profiles && conf.profiles.length > 0) {
      state.profiles = conf.profiles.map(p => {
        const players = p.players || [];
        return {
          id: p.id,
          display_name: p.display_name,
          avatar_url: p.avatar_url && p.avatar_url.startsWith('/') 
            ? `${CONFIG.BROKER_BASE}${p.avatar_url}` 
            : (p.avatar_url || `https://api.dicebear.com/7.x/bottts/svg?seed=${p.id}`),
          default_player_id: p.default_player_id || '',
          default_player_name: p.default_player_name || 'Yoto Player',
          players: players
        };
      });

      // Keep activeProfile and activeDevice objects updated with live telemetry
      if (state.activeProfile) {
        const updatedProf = state.profiles.find(p => p.id === state.activeProfile.id);
        if (updatedProf) {
          state.activeProfile = updatedProf;
          if (state.activeDevice) {
            const updatedDev = (updatedProf.players || []).find(d => d.id === state.activeDevice.id);
            if (updatedDev) {
              state.activeDevice = updatedDev;
            } else {
              state.activeDevice = state.resolveActiveDevice(updatedProf);
            }
          } else {
            state.activeDevice = state.resolveActiveDevice(updatedProf);
          }
        }
      }
    }



    return conf;

  },



  /**

   * Fetches personalized media catalog (ABS + Yoto) for selected profile

   */

  /**
   * Fetches chapters / tracks breakdown for a specific card on demand
   */
  async fetchMediaTracks(mediaId) {
    const headers = state.deviceToken ? { 'Authorization': `Bearer ${state.deviceToken}` } : {};
    try {
      const res = await fetch(`${CONFIG.BROKER_BASE}/api/media/${encodeURIComponent(mediaId)}/tracks`, { headers });
      if (!res.ok) return null;
      const data = await res.json();
      return (data && data.tracks) ? data.tracks : [];
    } catch (e) {
      console.warn(`Failed to fetch tracks for ${mediaId}:`, e);
      return [];
    }
  },

  async fetchLiveCatalog(profileId) {

    const headers = state.deviceToken ? { 'Authorization': `Bearer ${state.deviceToken}` } : {};



    const absPromise = fetch(`${CONFIG.BROKER_BASE}/api/kids/${profileId}/abs`, { headers })

      .then(r => r.ok ? r.json() : [])

      .catch(() => []);



    const yotoPromise = fetch(`${CONFIG.BROKER_BASE}/api/kids/${profileId}/yoto`, { headers })

      .then(r => r.ok ? r.json() : [])

      .catch(() => []);



    const [absItems, yotoItems] = await Promise.all([absPromise, yotoPromise]);



    const combined = [];

    (absItems || []).forEach(b => {

      const thumb = b.thumb || b.cover_url || b.cover;

      const coverUrl = thumb ? (thumb.startsWith('http') ? thumb : `${CONFIG.BROKER_BASE}${thumb}`) : null;

      combined.push({

        id: b.id,

        title: b.title,

        author: b.author,

        cover: coverUrl,

        is_highlight: !!b.is_highlight,

        duration: b.duration,

        chapters: (b.chapters || []).map(ch => ({ title: ch.title, duration: (ch.end && ch.start) ? (ch.end - ch.start) : (ch.duration || 0) })),

        source: 'abs',

        series: b.series || '',

        sequence: b.sequence != null ? b.sequence : null,

        sequence_display: b.sequence_display || (b.sequence != null ? String(b.sequence) : null),

        series_list: b.series_list || (b.series ? [{ name: b.series, sequence: b.sequence, sequence_display: b.sequence_display }] : [])

      });

    });



    (yotoItems || []).forEach(y => {

      const thumb = y.thumb || y.cover_url || y.cover;

      const coverUrl = thumb ? (thumb.startsWith('http') ? thumb : `${CONFIG.BROKER_BASE}${thumb}`) : null;

      combined.push({

        id: y.card_id || y.id,

        title: y.title,

        author: y.author || 'Yoto',

        cover: coverUrl,

        is_highlight: !!y.is_highlight,

        duration: y.duration,

        chapters: (y.chapters || []).map(ch => ({ title: ch.title, duration: ch.duration || 0 })),

        source: 'yoto',

        series: y.series || '',

        sequence: y.sequence != null ? y.sequence : null,

        sequence_display: y.sequence_display || (y.sequence != null ? String(y.sequence) : null),

        groups: y.groups || [],

        series_list: y.series_list || (y.series ? [{ name: y.series, sequence: y.sequence, sequence_display: y.sequence_display }] : [])

      });

    });



    // Highlights first, then alphabetical sort ignoring 'The', 'A', 'An'

    combined.sort((a, b) => {

      if (!!b.is_highlight !== !!a.is_highlight) {

        return b.is_highlight ? 1 : -1;

      }

      const cleanA = (a.title || '').replace(/^(the|a|an)\s+/i, '').trim().toLowerCase();

      const cleanB = (b.title || '').replace(/^(the|a|an)\s+/i, '').trim().toLowerCase();

      return cleanA.localeCompare(cleanB);

    });



    state.catalog = combined;
    return combined;
  },

  /**
   * Dispatches playback command to the broker /api/play endpoint.
   * @param {Object} card - The media card object (ABS book or Yoto card).
   * @param {number} chapterIndex - 0-based chapter index from UI (converted to 1-based for broker).
   */
  async playMedia(card, chapterIndex = 0) {
    if (!card) return { ok: false, error: 'No media selected' };

    const headers = {
      'Content-Type': 'application/json'
    };
    if (state.deviceToken) {
      headers['Authorization'] = `Bearer ${state.deviceToken}`;
    }

    const payload = {
      child: state.activeProfile?.id || 'jackson',
      type: card.source || card.type || 'abs',
      id: card.id || card.cardId,
      device_id: state.activeDevice?.id || state.activeProfile?.default_player_id || '',
      chapter: (chapterIndex != null ? chapterIndex : 0) + 1
    };

    try {
      const res = await fetch(`${CONFIG.BROKER_BASE}/api/play`, {
        method: 'POST',
        headers,
        body: JSON.stringify(payload)
      });

      const data = await res.json().catch(() => ({ ok: false, error: `HTTP ${res.status}` }));
      if (!res.ok) {
        console.error('Playback request returned non-OK status:', res.status, data);
        return { ok: false, status: res.status, ...data };
      }
      return data;
    } catch (err) {
      console.error('Playback dispatch failed:', err);
      return { ok: false, error: err.message };
    }
  }
  /**
   * Dispatches transport / player commands (pause, resume, stop, next, prev, volume)
   * to the broker /api/player/control endpoint.
   */
  async sendPlayerControl(action, payload = {}) {
    const deviceId = state.activeDevice?.id || state.activeProfile?.default_player_id;
    if (!deviceId) return { ok: false, error: 'No active device selected' };

    const headers = { 'Content-Type': 'application/json' };
    if (state.deviceToken) {
      headers['Authorization'] = `Bearer ${state.deviceToken}`;
    }

    const body = {
      device_id: deviceId,
      action: action,
      ...payload
    };

    try {
      const res = await fetch(`${CONFIG.BROKER_BASE}/api/player/control`, {
        method: 'POST',
        headers,
        body: JSON.stringify(body)
      });
      const data = await res.json().catch(() => ({ ok: false, error: `HTTP ${res.status}` }));
      if (!res.ok) {
        console.warn('Player control command error:', res.status, data);
        return { ok: false, status: res.status, ...data };
      }
      return data;
    } catch (err) {
      console.error('Player control fetch failed:', err);
      return { ok: false, error: err.message };
    }
  }
};
