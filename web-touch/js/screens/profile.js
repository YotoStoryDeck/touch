import { state } from '../state.js';
import { Utils } from '../config.js';

/**
 * Screen 1: Profile Selection Screen Component
 * Renders the avatar grid with scrolling support and friendly player names.
 */
export const ProfileScreen = {
  getTimeGreeting() {
    const hour = new Date().getHours();
    if (hour >= 5 && hour < 12) {
      return 'WHO IS LISTENING THIS MORNING?';
    } else if (hour >= 12 && hour < 17) {
      return 'WHO IS LISTENING THIS AFTERNOON?';
    } else {
      return 'WHO IS LISTENING TONIGHT?';
    }
  },

  render() {
    const greeting = this.getTimeGreeting();
    const cards = state.profiles.map(p => `
      <div class="profile-card" data-profile="${Utils.esc(p.id)}">
        <img class="profile-avatar" src="${Utils.esc(p.avatar_url)}" onerror="this.src='https://api.dicebear.com/7.x/bottts/svg?seed=${Utils.esc(p.id)}'">
        <div class="profile-name">${Utils.esc(p.display_name)}</div>
      </div>
    `).join('');

    return `
      <div class="profile-screen">
        <div class="profile-header">
          <div class="profile-title">${greeting}</div>
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
