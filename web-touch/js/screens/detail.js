import { state } from '../state.js';
import { Utils } from '../config.js';
import { PlayerScreen } from './player.js';

/**
 * Screen 3: Detail Screen (Book Summary & Tracklist)
 */
export const DetailScreen = {
  render() {
    const c = state.card;
    if (!c) return '<div class="frame">No card selected.</div>';

    const total = (c.chapters || []).reduce((a, ch) => a + (ch.duration || 0), 0);
    const rows = (c.chapters || []).map((ch, i) => `
      <div class="chrow" data-ch="${i}">
        <div class="chnum">${i + 1}</div>
        <div class="chttl">${Utils.esc(ch.title || ('Chapter ' + (i + 1)))}</div>
        <div class="chdur">${Utils.mmss(ch.duration)}</div>
      </div>
    `).join('');

    return `
      <div class="back" data-go="catalog">&#8592;</div>
      <div class="d">
        <div class="dleft">
          <img class="dcover" src="${Utils.esc(c.cover || c.img)}">
          <button class="playbtn" data-act="playstart">&#9654; Play Book</button>
        </div>
        <div class="dright">
          <div class="dtitle">${Utils.esc(c.title)}</div>
          <div class="dsub">${Utils.esc(c.author || (c.source === 'abs' ? 'Audiobookshelf' : 'Yoto'))}</div>
          <div class="dsum">${(c.chapters && c.chapters.length > 0) ? `${c.chapters.length} chapters &middot; ${Utils.longdur(total || c.duration)}` : 'Loading chapters...'}</div>
          <div class="chapters">
            ${rows.length ? rows : '<div style="padding:20px; color:#888;">Loading tracklist...</div>'}
          </div>
        </div>
      </div>
      ${PlayerScreen.renderNowbar()}
    `;
  }
};
