import { state } from '../state.js';
import { Utils } from '../config.js';
import { PlayerScreen } from './player.js';

/**
 * Screen 2: Catalog Browser & Navigation Component
 */
export const CatalogScreen = {
  renderTopNav() {
    const p = state.activeProfile || state.profiles[0] || { display_name: 'Profiles', avatar_url: '' };
    const players = p.players || [];
    const activeDev = state.activeDevice || state.resolveActiveDevice(p);
    const hasMultiple = players.length > 1;

    const devStatusIcon = activeDev?.is_playing ? '▶' : (activeDev?.online ? '●' : '○');
    const devStatusClass = activeDev?.is_playing ? 'status-playing' : (activeDev?.online ? 'status-online' : 'status-offline');

    // Dropdown options if picker is open
    const pickerDropdown = (state.isDevicePickerOpen && hasMultiple) ? `
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
          <div class="prof-pill" data-act="switch-profile" title="Switch profile">
            ${p.avatar_url ? `<img src="${Utils.esc(p.avatar_url)}" onerror="this.src='https://api.dicebear.com/7.x/bottts/svg?seed=${Utils.esc(p.id||'anon')}'">` : '<span style="font-size:16px;">👤</span>'}
            <span>${Utils.esc(p.display_name)}</span>
            <span style="font-size:10px; color:#8a8276;">&#9660;</span>
          </div>

          <!-- Device Selector Button -->
          <div class="device-pill-wrap">
            <div class="device-pill ${hasMultiple ? 'clickable' : ''}" data-act="${hasMultiple ? 'toggle-device-picker' : ''}" title="${hasMultiple ? 'Choose playback device' : 'Active playback device'}">
              <span class="device-dot ${devStatusClass}">${devStatusIcon}</span>
              <span class="device-label">${Utils.esc(activeDev?.name || p.default_player_name || 'Yoto Player')}</span>
              ${hasMultiple ? '<span class="pill-chev">&#9660;</span>' : ''}
            </div>
            ${pickerDropdown}
          </div>
        </div>

        <div class="nav-tabs">
          <button class="tab-btn ${state.activeTab === 'all' ? 'active' : ''}" data-tab="all">All Books</button>
          <button class="tab-btn ${state.activeTab === 'series' ? 'active' : ''}" data-tab="series">Series / Groups</button>
          <button class="tab-btn ${state.activeTab === 'abs' ? 'active' : ''}" data-tab="abs">Audiobooks</button>
          <button class="tab-btn ${state.activeTab === 'yoto' ? 'active' : ''}" data-tab="yoto">Yoto Cards</button>
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
        const seenGroupNames = new Set();
        seriesEntries.forEach(entry => {
          const groupName = (entry.name || '').trim();
          if (groupName && !seenGroupNames.has(groupName.toLowerCase())) {
            seenGroupNames.add(groupName.toLowerCase());
            if (!groups.has(groupName)) {
              groups.set(groupName, []);
            }
            groups.get(groupName).push({
              ...item,
              groupSequence: entry.sequence != null ? entry.sequence : item.sequence,
              groupSequenceDisplay: entry.sequence_display || item.sequence_display || (entry.sequence != null ? String(entry.sequence) : (item.sequence != null ? String(item.sequence) : null))
            });
          }
        });
      }
    });

    const shouldExcludeSingle = state.deviceConfig?.exclude_single_card_series !== false;

    // Filter out groups that have only 1 item if the setting is enabled
    const eligibleGroupNames = Array.from(groups.keys()).filter(name => {
      const count = (groups.get(name) || []).length;
      if (shouldExcludeSingle && count <= 1) {
        return false;
      }
      return true;
    });

    // Sort series groups alphabetically ignoring 'The', 'A', 'An' (with standalone last)
    const sortedGroupNames = eligibleGroupNames.sort((a, b) => {
      if (a === 'Standalone & Singles') return 1;
      if (b === 'Standalone & Singles') return -1;
      return this.cleanTitle(a).localeCompare(this.cleanTitle(b));
    });

    if (!sortedGroupNames.length) {
      return '<div style="padding:40px; color:#888;">No multi-book series found in this library.</div>';
    }

    return sortedGroupNames.map(groupName => {
      const items = groups.get(groupName);

      // Sort books within series by group-specific sequence ascending, then clean title
      items.sort((a, b) => {
        const seqA = a.groupSequence != null ? a.groupSequence : 999999;
        const seqB = b.groupSequence != null ? b.groupSequence : 999999;
        if (seqA !== seqB) {
          return seqA - seqB;
        }
        return this.cleanTitle(a.title).localeCompare(this.cleanTitle(b.title));
      });

      const tiles = items.map(c => {
        let seqLabel = '';
        if (c.groupSequenceDisplay) {
          seqLabel = c.groupSequenceDisplay.startsWith('#') ? c.groupSequenceDisplay : `#${c.groupSequenceDisplay}`;
        } else if (c.groupSequence != null) {
          seqLabel = `#${c.groupSequence % 1 === 0 ? c.groupSequence : c.groupSequence.toFixed(1)}`;
        }

        return `
        <div class="tile" data-card="${Utils.esc(c.id || c.cardId)}" title="${Utils.esc(c.title)}">
          <img src="${Utils.esc(c.cover || c.img)}" onerror="this.src='https://placehold.co/122x193/2b2622/ffffff?text=Book'">
          ${c.is_highlight ? '<span class="badge-star" title="Highlighted">★</span>' : ''}
          ${seqLabel ? `<span class="badge-seq">${Utils.esc(seqLabel)}</span>` : ''}
          ${c.source === 'abs' ? '<span class="badge-abs">ABS</span>' : '<span class="badge-yoto">YOTO</span>'}
        </div>
      `;
      }).join('');

      return `
        <div class="series-section">
          <div class="series-header">
            <span class="series-title">${Utils.esc(groupName)}</span>
            <span class="series-count">(${items.length} ${items.length === 1 ? 'card' : 'cards'})</span>
          </div>
          <div class="series-row">
            ${tiles}
          </div>
        </div>
      `;
    }).join('');
  },

  render() {
    if (state.activeTab === 'series') {
      return `
        ${this.renderTopNav()}
        <div class="grid-series">
          ${this.renderSeriesView()}
        </div>
        ${PlayerScreen.renderNowbar()}
      `;
    }

    const filtered = state.catalog.filter(c => {
      if (state.activeTab === 'abs') return c.source === 'abs';
      if (state.activeTab === 'yoto') return c.source !== 'abs';
      return true;
    });

    // In flat lists, sort highlights first, then by title ignoring articles 'The', 'A', 'An'
    filtered.sort((a, b) => {
      if (!!b.is_highlight !== !!a.is_highlight) {
        return b.is_highlight ? 1 : -1;
      }
      return this.cleanTitle(a.title).localeCompare(this.cleanTitle(b.title));
    });

    const tiles = filtered.map(c => `
      <div class="tile" data-card="${Utils.esc(c.id || c.cardId)}" title="${Utils.esc(c.title)}">
        <img src="${Utils.esc(c.cover || c.img)}" onerror="this.src='https://placehold.co/122x193/2b2622/ffffff?text=Book'">
        ${c.is_highlight ? '<span class="badge-star" title="Highlighted">★</span>' : ''}
        ${c.source === 'abs' ? '<span class="badge-abs">ABS</span>' : '<span class="badge-yoto">YOTO</span>'}
      </div>
    `).join('');

    return `
      ${this.renderTopNav()}
      <div class="grid">
        ${tiles.length ? tiles : '<div style="padding:40px; color:#888;">No audiobooks found in this library.</div>'}
      </div>
      ${PlayerScreen.renderNowbar()}
    `;
  }
};
