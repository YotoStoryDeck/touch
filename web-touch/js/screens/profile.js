import { state } from '../state.js';
import { Utils } from '../config.js';

/**
 * Screen 1: Profile Selection Screen Component
 * Renders the avatar grid with scrolling support and friendly player names.
 */
export const ProfileScreen = {
  render() {
    const cards = state.profiles.map(p => `
      <div class="profile-card" data-profile="${Utils.esc(p.id)}">
        <img class="profile-avatar" src="${Utils.esc(p.avatar_url)}" onerror="this.src='https://api.dicebear.com/7.x/bottts/svg?seed=${Utils.esc(p.id)}'">
        <div class="profile-name">${Utils.esc(p.display_name)}</div>
      </div>
    `).join('');

    return `
      <div class="profile-screen">
        <div class="profile-header">
          <div class="profile-title">WHO IS LISTENING TONIGHT?</div>
          <div class="profile-subtitle">Tap your profile avatar to explore your audiobooks</div>
        </div>
        <div class="profiles-scroll">
          <div class="profiles-row">
            ${cards}
          </div>
        </div>
      </div>
    `;
  }
};
