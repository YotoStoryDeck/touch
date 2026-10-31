/**
 * Application Configuration & Global Constants
 */
export const CONFIG = {
  BROKER_BASE: (typeof window !== "undefined" && window.location.origin) ? window.location.origin : "http://127.0.0.1:5055",
  DEVICE_ID: "test-browser-device",
  FRIENDLY_NAME: "Browser UI Prototype",
  FIRMWARE_VERSION: "1.0.0-web"
};

/**
 * Global Utility Functions
 */
export const Utils = {
  esc: (s) => (s == null ? '' : String(s)).replace(/[&<>"]/g, c => ({
    '&': '&amp;',
    '<': '&lt;',
    '>': '&gt;',
    '"': '&quot;'
  }[c])),

  mmss: (s) => {
    s = Math.max(0, Math.round(s || 0));
    return Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0');
  },

  longdur: (s) => {
    s = Math.round(s || 0);
    const h = Math.floor(s / 3600);
    const m = Math.floor((s % 3600) / 60);
    return h ? `${h}h ${m}m` : `${m}m`;
  }
};
