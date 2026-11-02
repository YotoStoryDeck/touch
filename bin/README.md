# StoryDeck Touch Pre-Compiled Binaries

This folder contains the firmware binaries flashed directly onto the ESP32-S3 via the browser's Web Serial API:

| File | Memory Offset | Description |
| :--- | :--- | :--- |
| `bootloader.bin` | `0x0000` (`0`) | 2nd-stage ESP32-S3 ROM bootloader |
| `partitions.bin` | `0x8000` (`32768`) | 16MB dual-OTA partition table |
| `boot_app0.bin` | `0xe000` (`57344`) | OTA boot data initialization selector |
| `firmware.bin` | `0x10000` (`65536`) | StoryDeck Touch core application binary |

Whenever a new release tag is published on GitHub, these binaries are refreshed by the release pipeline.
