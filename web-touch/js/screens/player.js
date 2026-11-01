import { state } from '../state.js';
import { Utils } from '../config.js';

/**
 * Screen 3: Now Playing / Player Screen Component
 */
export const PlayerScreen = {
  renderNowbar() {
    const devName = state.activeDevice?.name || state.activeProfile?.display_name || 'Player';
    if (state.card && (state.isPlaying || state.activeDevice?.has_card)) {
      const ch = (state.card.chapters || [])[state.chapterIndex] || {};
      const trackTitle = state.card.chapter_title || ch.title || 'Playing';
      const author = state.card.author || '';
      const bookTitle = state.card.title || 'Now Playing';

      // Format bottom line as "Author • Chapter Title"
      let bottomLine = Utils.esc(trackTitle);
      if (author && trackTitle) {
        bottomLine = `${Utils.esc(author)} &bull; ${Utils.esc(trackTitle)}`;
      } else if (author) {
        bottomLine = Utils.esc(author);
      }

      return `
        <div class="nowbar" data-go="now">
          <div class="status"><span class="dot" style="${state.isPlaying ? '' : 'background:#eab308; box-shadow:0 0 8px #eab308;'}"></span>${Utils.esc(devName)}</div>
          <img class="nb-thumb" src="${Utils.esc(state.card.cover || state.card.img)}" onerror="this.src='https://placehold.co/122x193/2b2622/ffffff?text=Playing'">
          <div class="nb-txt">
            <div class="nb-title">${Utils.esc(bookTitle)}</div>
            <div class="nb-sub">${bottomLine}</div>
          </div>
          <div class="nb-play" data-act="toggle">${state.isPlaying ? '&#10074;&#10074;' : '&#9654;'}</div>
        </div>
      `;
    }
    return `
      <div class="nowbar">
        <div class="status"><span class="dot" style="background:#9ca3af; box-shadow:none;"></span>${Utils.esc(devName)}</div>
        <div class="nb-mini">&#9835;</div>
        <div class="nb-txt muted">Nothing playing &mdash; tap a book to listen</div>
      </div>
    `;
  },

  render() {
    const c = state.card;
    if (!c) {
      return `
        <div class="frame" style="display:flex; flex-direction:column; align-items:center; justify-content:center; gap:16px;">
          <div style="font-size:42px;">📻</div>
          <div style="font-size:18px; font-weight:600; color:#4a453e;">Nothing is currently playing.</div>
          <button class="gu-pill-btn active" data-go="catalog" style="padding:10px 20px;">&#8592; Back to Library</button>
        </div>
      `;
    }

    const ch = (c.chapters || [])[state.chapterIndex] || {};
    const dur = state.activeDevice?.duration || c.duration || ch.duration || 1800;
    const pos = Math.min(state.activeDevice?.position || 0, dur);
    const pct = dur > 0 ? Math.min(100, Math.max(0, (pos / dur) * 100)) : 0;
    const devMaxVol = (state.activeDevice && state.activeDevice.volume_max != null) ? state.activeDevice.volume_max : 100;
    const vol = Math.min(devMaxVol, state.volume != null ? state.volume : (state.activeDevice?.volume != null ? state.activeDevice.volume : 50));

    const trackTitle = c.chapter_title || ch.title || 'Playing';
    const author = c.author || '';

    // Format subtitle as "Author • Chapter Title"
    let subLine = Utils.esc(trackTitle);
    if (author && trackTitle) {
      subLine = `${Utils.esc(author)} &bull; ${Utils.esc(trackTitle)}`;
    } else if (author) {
      subLine = Utils.esc(author);
    }

    return `
      <div class="np">
        <div class="nptop">
          <div class="np-back-btn" data-go="catalog" title="Return to Library">&#8592; Back</div>
          <span class="label">Playing on ${Utils.esc(state.activeDevice?.name || state.activeProfile?.display_name || 'Yoto')}</span>
        </div>
        <div class="npmid">
          <img class="npcover clickable-card" data-act="inspect-card" src="${Utils.esc(c.cover || c.img)}" title="View book details and tracklist">
          <div class="npinfo">
            <div class="t clickable-card" data-act="inspect-card" title="View book details and tracklist">${Utils.esc(c.title)}</div>
            <div class="c">${subLine}</div>
            <div class="bar">
              <i style="width: ${pct}%;"></i>
              <b style="left: ${pct}%;"></b>
            </div>
            <div class="times"><span>${Utils.mmss(pos)}</span><span>-${Utils.mmss(dur - pos)}</span></div>
            <div class="ctrls">
              <span class="ic" data-act="prev" title="Previous Chapter">&#9198;</span>
              <div class="big" data-act="toggle" title="${state.isPlaying ? 'Pause' : 'Play'}">${state.isPlaying ? '&#10074;&#10074;' : '&#9654;'}</div>
              <span class="ic" data-act="next" title="Next Chapter">&#9197;</span>
              <span class="vol">
                <span data-act="vol-down" style="cursor:pointer;" title="Volume Down">&#128264;</span>
                <div class="volbar-wrap" style="position:relative; display:inline-block;">
                  <span class="volbar" data-act="vol-set" style="cursor:pointer;" title="Set Volume (Max: ${devMaxVol}%)">
                    <i style="width: ${vol}%;"></i>
                    ${devMaxVol < 100 ? `<div style="position:absolute; right:0; top:0; bottom:0; width:${100 - devMaxVol}%; background:rgba(239,68,68,0.3); border-radius:0 6px 6px 0; pointer-events:none;" title="Parent Limit: ${devMaxVol}%"></div>` : ''}
                  </span>
                </div>
                <span data-act="vol-up" style="cursor:pointer;" title="Volume Up">&#128266;</span>
              </span>
            </div>
          </div>
        </div>
      </div>
    `;
  }
};
