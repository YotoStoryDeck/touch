# StoryDeck Touch — Web Flasher Website

This branch (`website`) is dedicated exclusively to hosting the **StoryDeck Touch Web Installer** on GitHub Pages.

It contains only static web client files:
- **`index.html`**: Responsive landing page embedding the ESP Web Tools web component (`<esp-web-install-button>`).
- **`manifest.json`**: ESP Web Tools manifest targeting the ESP32-S3 microcontroller and 16MB flash offsets.
- **`.nojekyll`**: Tells GitHub Pages to serve static assets directly without Jekyll transformations.
- **`bin/`**: Destination directory for firmware binaries (`bootloader.bin`, `partitions.bin`, `boot_app0.bin`, `firmware.bin`).

All source code, firmware C++ files, and hardware design models reside separately on the `main` and `dev` branches.
