import { state } from '../state.js';
import { Utils } from '../config.js';

/**
 * Screen 6: Parent PIN Keypad Component
 * Supports:
 *  - 'unlock': Gatekeeper for opening Grown-ups screen
 *  - 'setup': Initial 4-digit PIN setup on first launch
 *  - 'confirm': Confirming newly entered PIN
 *  - 'change': Changing existing PIN
 */
export const PinScreen = {
  render() {
    const mode = state.pinMode || 'unlock';
    let title = 'Parent PIN Required';
    let subtitle = 'Enter 4-digit PIN to access Grown-ups settings';

    if (mode === 'setup') {
      title = 'Set Grown-ups PIN';
      subtitle = 'Create a 4-digit PIN for parent settings';
    } else if (mode === 'confirm') {
      title = 'Confirm Your PIN';
      subtitle = 'Re-enter your 4-digit PIN to confirm';
    } else if (mode === 'change') {
      title = 'Change Grown-ups PIN';
      subtitle = 'Enter current 4-digit PIN first';
    }

    const enteredLen = (state.currentEnteredPin || '').length;

    return `
      <div class="pin-screen">
        <div class="pin-close-btn" data-act="pin-cancel" title="Cancel">&#10005;</div>
        
        <div class="pin-header">
          <div class="pin-title">${title}</div>
          <div class="pin-subtitle">${subtitle}</div>
        </div>

        <div class="pin-dots ${state.isPinError ? 'shake' : ''}" id="pinDots">
          <div class="pin-dot ${enteredLen >= 1 ? 'filled' : ''}"></div>
          <div class="pin-dot ${enteredLen >= 2 ? 'filled' : ''}"></div>
          <div class="pin-dot ${enteredLen >= 3 ? 'filled' : ''}"></div>
          <div class="pin-dot ${enteredLen >= 4 ? 'filled' : ''}"></div>
        </div>

        <div class="pin-keypad">
          <button class="pin-key" data-key="1">1</button>
          <button class="pin-key" data-key="2">2</button>
          <button class="pin-key" data-key="3">3</button>
          
          <button class="pin-key" data-key="4">4</button>
          <button class="pin-key" data-key="5">5</button>
          <button class="pin-key" data-key="6">6</button>
          
          <button class="pin-key" data-key="7">7</button>
          <button class="pin-key" data-key="8">8</button>
          <button class="pin-key" data-key="9">9</button>
          
          <button class="pin-key action-key" data-key="clear">Clear</button>
          <button class="pin-key" data-key="0">0</button>
          <button class="pin-key action-key" data-key="back">&#9003;</button>
        </div>
      </div>
    `;
  }
};

