import { state } from '../state.js';
import { Utils } from '../config.js';
import { PlayerScreen } from './player.js';

/**
 * Screen 3: Detail Screen (Book Summary & Tracklist)
 */
export const DetailScreen = {
  render() {
    // Prefer explicitly inspected previewCard, fallback to active playing card
    const c = state.previewCard || state.card;
    if (!c) {
      return `
        <div class="back" data-go="catalog" style="z-index:10;">&#8592;</div>
        <div class="frame" style="display:flex; flex-direction:column; align-items:center; justify-content:center; height:100%; text-align:center;">
          <div style="font-size:48px; margin-bottom:12px;">📖</div>
          <div style="font-size:22px; font-weight:700; color:var(--ink);">No Card Selected</div>
          <div style="font-size:14px; color:#888; margin-top:6px; margin-bottom:20px;">Choose a book from the catalog to listen or view details.</div>
          <button class="playbtn" data-go="catalog" style="max-width:200px; padding:12px 24px;">Return to Catalog</button>
        </div>
      `;
    }

    const isCurrentlyPlayingThis = Boolean(
      state.isPlaying && state.card && (
        (c.id && (state.card.id === c.id || state.card.cardId === c.id)) ||
        (c.cardId && (state.card.cardId === c.cardId || state.card.id === c.cardId)) ||
        (c.title && state.card.title === c.title)
      )
    );

    const total = (c.chapters || []).reduce((a, ch) => a + (ch.duration || 0), 0);
    const rows = (c.chapters || []).map((ch, i) => {
      const isChActive = isCurrentlyPlayingThis && state.chapterIndex === i;
      return `
        <div class="chrow ${isChActive ? 'active-track' : ''}" data-ch="${i}" style="${isChActive ? 'background:#e0ede7; font-weight:600;' : ''}">
          <div class="chnum">${isChActive ? '▶' : (i + 1)}</div>
          <div class="chttl">${Utils.esc(ch.title || ('Chapter ' + (i + 1)))}</div>
          <div class="chdur">${Utils.mmss(ch.duration)}</div>
        </div>
      `;
    }).join('');

    return `
      <div class="back" data-go="catalog">&#8592;</div>
      <div class="d">
        <div class="dleft">
          <img class="dcover" src="${Utils.esc(c.cover || c.img)}">
          <button class="playbtn" data-act="playstart" id="btn-play-book">${isCurrentlyPlayingThis ? '&#9646;&#9646; Now Playing' : '&#9654; Play Book'}</button>
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
