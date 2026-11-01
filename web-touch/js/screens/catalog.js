import { state } from '../state.js';
import { Utils } from '../config.js';
import { PlayerScreen } from './player.js';

/**
 * Screen 2: Catalog Browser & Navigation Component
 */
export const CatalogScreen = {
  renderTopNav() {
    const isStandalone = state.mode === 'standalone';
    const isLocked = state.lockPlayer;
    const p = state.activeProfile || state.profiles[0] || { display_name: 'Profiles', avatar_url: '' };
    const players = isStandalone ? state.getAllAvailablePlayers() : (p.players || []);
    const activeDev = state.activeDevice || state.resolveActiveDevice(isStandalone ? null : p);
    const hasMultiple = players.length > 1;

    const devStatusIcon = activeDev?.is_playing ? '▶' : (activeDev?.online ? '●' : '○');
    const devStatusClass = activeDev?.is_playing ? 'status-playing' : (activeDev?.online ? 'status-online' : 'status-offline');

    // Dropdown options if picker is open (and player is not locked)
    const pickerDropdown = (state.isDevicePickerOpen && hasMultiple && !isLocked) ? `
      <div class="device-dropdown">
        <div class="dropdown-header">Select Target Player</div>
        ${players.map(d => {
          const isSelected = d.id === activeDev?.id;
          const isPlay = d.is_playing;
          const isOnline = d.online;
          return `
            <div class="device-opt ${isSelected ? 'selected' : ''}" data-act="select-device" data-device-id="${Utils.esc(d.id)}">
              <span class="dev-dot ${isPlay ? 'dot-play' : (isOnline ? 'dot-on' : 'dot-off')}">
                ${isPlay ? '▶' : '●'}
              </span>
              <span class="dev-name">${Utils.esc(d.name)}</span>
              ${d.id === p.default_player_id ? '<span class="dev-tag">DEFAULT</span>' : ''}
              ${isPlay ? '<span class="dev-tag play">PLAYING</span>' : ''}
            </div>
          `;
        }).join('')}
      </div>
    ` : '';

    return `
      <div class="topnav">
        <div class="topnav-left">
          ${!isStandalone ? `
            <!-- Core Mode: Profile Selector Pill -->
            <div class="prof-pill" data-act="switch-profile" title="Switch profile">
              ${p.avatar_url ? `<img src="${Utils.esc(p.avatar_url)}" onerror="this.src='https://api.dicebear.com/7.x/bottts/svg?seed=${Utils.esc(p.id||'anon')}'">` : '<span style="font-size:16px;">👤</span>'}
              <span>${Utils.esc(p.display_name)}</span>
              <span style="font-size:10px; color:#8a8276;">&#9660;</span>
            </div>
          ` : ''}

          <!-- Device / Player Selector Button (Primary identity in Standalone mode) -->
          <div class="device-pill-wrap">
            <div class="device-pill ${(!isLocked && hasMultiple) ? 'clickable' : ''}" 
                 data-act="${(!isLocked && hasMultiple) ? 'toggle-device-picker' : ''}" 
                 title="${isLocked ? 'Controller is locked to this player' : (hasMultiple ? 'Choose playback device' : 'Active playback device')}">
              ${isLocked ? '<span style="font-size:11px;">🔒</span>' : ''}
              <span class="device-dot ${devStatusClass}">${devStatusIcon}</span>
              <span class="device-label">${Utils.esc(activeDev?.name || p.default_player_name || 'Yoto Player')}</span>
              ${(!isLocked && hasMultiple) ? '<span class="pill-chev">&#9660;</span>' : ''}
            </div>
            ${pickerDropdown}
          </div>
        </div>

        <div class="nav-tabs">
          <button class="tab-btn ${state.activeTab === 'all' ? 'active' : ''}" data-tab="all">All Books</button>
          <button class="tab-btn ${state.activeTab === 'series' ? 'active' : ''}" data-tab="series">Series / Groups</button>
          <button class="tab-btn ${state.activeTab === 'abs' ? 'active' : ''}" data-tab="abs">Audiobooks</button>
          <button class="tab-btn ${state.activeTab === 'yoto' ? 'active' : ''}" data-tab="yoto">Yoto Cards</button>
          
          <!-- Grown-Ups Screen Gear (Hold 2.5s) -->
          <button class="nav-gear-btn" data-act="gear-button" title="Hold for 2.5s to open Grown-ups settings">&#9881;</button>
        </div>
      </div>
    `;
  },

  cleanTitle(t) {
    return (t || '').replace(/^(the|a|an)\s+/i, '').trim().toLowerCase();
  },

  renderSeriesView() {
    // Group books by series name, supporting cards belonging to multiple series/groups
    const groups = new Map();

    state.catalog.forEach(item => {
      const seriesEntries = (item.series_list && item.series_list.length > 0)
        ? item.series_list
        : (item.series ? [{ name: item.series, sequence: item.sequence }] : []);

      if (seriesEntries.length === 0) {
        const groupName = 'Standalone & Singles';
        if (!groups.has(groupName)) {
          groups.set(groupName, []);
        }
        groups.get(groupName).push({ ...item, groupSequence: null });
      } else {
        // Place card into every series/group it belongs to
        seriesEntries.forEach(sEntry => {
          const groupName = sEntry.name ? sEntry.name.trim() : 'Standalone & Singles';
          if (!groups.has(groupName)) {
            groups.set(groupName, []);
          }
          groups.get(groupName).push({ ...item, groupSequence: sEntry.sequence });
        });
      }
    });

    const groupNames = Array.from(groups.keys()).sort((a, b) => {
      if (a === 'Standalone & Singles') return 1;
      if (b === 'Standalone & Singles') return -1;
      return a.localeCompare(b);
    });

    const seriesHtml = groupNames.map(groupName => {
      const items = groups.get(groupName);

      // Sort items within this series/group:
      // 1. Numerically by sequence ascending (e.g. Book 1, Book 2)
      // 2. Alphabetically by cleaned title (ignoring leading The/A/An)
      items.sort((a, b) => {
        const seqA = a.groupSequence != null ? Number(a.groupSequence) : Infinity;
        const seqB = b.groupSequence != null ? Number(b.groupSequence) : Infinity;

        if (seqA !== seqB && !isNaN(seqA) && !isNaN(seqB)) {
          return seqA - seqB;
        }

        const titleA = this.cleanTitle(a.title);
        const titleB = this.cleanTitle(b.title);
        return titleA.localeCompare(titleB);
      });

      const cardsHtml = items.map(c => `
        <div class="card series-card" data-card="${Utils.esc(c.id || c.cardId)}">
          <div class="card-thumb-wrap">
            <img class="card-thumb" src="${Utils.esc(c.cover || c.img)}" loading="lazy" onerror="this.src='https://placehold.co/122x193/2b2622/ffffff?text=No+Cover'">
            <div class="card-badge ${c.source === 'abs' || c.type === 'abs' ? 'badge-abs' : 'badge-yoto'}">
              ${c.source === 'abs' || c.type === 'abs' ? 'ABS' : 'YOTO'}
            </div>
            ${c.groupSequence != null ? `<div class="card-seq-badge">#${c.groupSequence}</div>` : ''}
          </div>
          <div class="card-name">${Utils.esc(c.title)}</div>
          <div class="card-author">${Utils.esc(c.author || '')}</div>
        </div>
      `).join('');

      return `
        <div class="series-group">
          <div class="series-group-header">
            <span class="series-group-title">${Utils.esc(groupName)}</span>
            <span class="series-group-count">${items.length} ${items.length === 1 ? 'book' : 'books'}</span>
          </div>
          <div class="series-row-scroll">
            <div class="series-row">
              ${cardsHtml}
            </div>
          </div>
        </div>
      `;
    }).join('');

    return `
      <div class="grid-series">
        ${seriesHtml || '<div class="muted" style="padding:20px; text-align:center;">No series collections available.</div>'}
      </div>
    `;
  },

  renderFlatView(items) {
    const cards = items.map(c => `
      <div class="card" data-card="${Utils.esc(c.id || c.cardId)}">
        <div class="card-thumb-wrap">
          <img class="card-thumb" src="${Utils.esc(c.cover || c.img)}" loading="lazy" onerror="this.src='https://placehold.co/122x193/2b2622/ffffff?text=No+Cover'">
          <div class="card-badge ${c.source === 'abs' || c.type === 'abs' ? 'badge-abs' : 'badge-yoto'}">
            ${c.source === 'abs' || c.type === 'abs' ? 'ABS' : 'YOTO'}
          </div>
        </div>
        <div class="card-name">${Utils.esc(c.title)}</div>
        <div class="card-author">${Utils.esc(c.author || '')}</div>
      </div>
    `).join('');

    return `
      <div class="grid">
        ${cards || '<div class="muted" style="padding:20px; text-align:center;">No cards found for this filter.</div>'}
      </div>
    `;
  },

  render() {
    let filtered = state.catalog;
    if (state.activeTab === 'abs') {
      filtered = state.catalog.filter(c => c.source === 'abs' || c.type === 'abs');
    } else if (state.activeTab === 'yoto') {
      filtered = state.catalog.filter(c => c.source === 'yoto' || c.type === 'yoto');
    }

    const bodyHtml = (state.activeTab === 'series')
      ? this.renderSeriesView()
      : this.renderFlatView(filtered);

    return `
      ${this.renderTopNav()}
      ${bodyHtml}
      ${PlayerScreen.renderNowbar()}
    `;
  }
};
