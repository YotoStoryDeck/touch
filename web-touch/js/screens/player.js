import { state } from '../state.js';
import { Utils } from '../config.js';

/**
 * Screen 4: Now Playing & Persistent Nowbar Component
 */
export const PlayerScreen = {
  renderNowbar() {
    const devName = state.activeDevice?.name || state.activeProfile?.display_name || 'Player';
    if (state.card && (state.isPlaying || state.activeDevice?.has_card)) {
      const ch = (state.card.chapters || [])[state.chapterIndex] || {};
      const bookTitle = state.card.title || 'Audiobook';
      const trackTitle = state.card.chapter_title || ch.title || 'Playing';
      const author = state.card.author || '';

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
    if (!c) return '<div class="frame">Nothing is currently playing.</div>';

    const ch = (c.chapters || [])[state.chapterIndex] || {};
    const dur = state.activeDevice?.duration || c.duration || ch.duration || 1800;
    const pos = Math.min(state.activeDevice?.position || 0, dur);
    const pct = dur > 0 ? Math.min(100, Math.max(0, (pos / dur) * 100)) : 0;
    const vol = state.volume != null ? state.volume : 50;

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
          <span class="chev" data-go="catalog">&#8964;</span>
          <span class="label">Playing for ${Utils.esc(state.activeProfile?.display_name || 'Kiddo')}</span>
        </div>
        <div class="npmid">
          <img class="npcover" src="${Utils.esc(c.cover || c.img)}">
          <div class="npinfo">
            <div class="t">${Utils.esc(c.title)}</div>
            <div class="c">${subLine}</div>
            <div class="bar">
              <i style="width: ${pct}%;"></i>
              <b style="left: ${pct}%;"></b>
            </div>
            <div class="times"><span>${Utils.mmss(pos)}</span><span>-${Utils.mmss(dur - pos)}</span></div>
            <div class="ctrls">
              <span class="ic" data-act="prev">&#9198;</span>
              <div class="big" data-act="toggle">${state.isPlaying ? '&#10074;&#10074;' : '&#9654;'}</div>
              <span class="ic" data-act="next">&#9197;</span>
              <span class="vol">
                <span data-act="vol-down" style="cursor:pointer;" title="Volume Down">&#128264;</span>
                <span class="volbar" data-act="vol-set" style="cursor:pointer;" title="Set Volume">
                  <i style="width: ${vol}%;"></i>
                </span>
                <span data-act="vol-up" style="cursor:pointer;" title="Volume Up">&#128266;</span>
              </span>
            </div>
          </div>
        </div>
      </div>
    `;
  }
};
