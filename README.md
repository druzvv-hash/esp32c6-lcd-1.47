# ESP32-C6-LCD-1.47-M — phone-only first test

Minimal Arduino test for the Waveshare ESP32-C6-LCD-1.47-M.

## Hardware used

- ESP32-C6
- ST7789 172x320 LCD
- LCD MOSI GPIO6
- LCD SCLK GPIO7
- LCD CS GPIO14
- LCD DC GPIO15
- LCD RST GPIO21
- LCD BL GPIO22
- BOOT button GPIO9
- LCD controller RAM X offset: 34

The test intentionally keeps backlight at 20%, with BOOT toggling to 50%.

## What appears on screen

- ESP32-C6 / PHONE FLASH TEST
- LCD status
- chip model and revision
- CPU frequency
- actual flash size reported by the chip/core
- free heap
- uptime
- backlight level

This is useful because the OEM demo/Android flasher disagreement about 4 MB vs 8 MB can be checked from our own firmware.

## Build only from Android / GitHub

1. Create a new GitHub repository from the phone.
2. Upload the contents of this ZIP/repository.
3. Open **Actions** -> **Build ESP32-C6 firmware** -> **Run workflow**.
4. When it finishes, download artifact:
   `ESP32C6-LCD-1.47-phone-flash`
5. Extract it on Android.

The workflow installs:
- Arduino ESP32 core 3.3.2
- GFX Library for Arduino 1.6.6

## FIRST flash from ESPFlash Android

For the first test, write **only**:

- Address: `0x10000`
- File: `app_0x10000.bin`

Keep High-Speed Stub OFF if it caused connection failures during the dump.

**Do not erase the whole flash yet.**
The OEM bootloader, partition table, NVS and OTA data stay in place.

After writing, press RESET. If the screen shows the new test UI, the full phone-only workflow works.

## OEM-compatible partition map

Recovered from the original dump:

| Partition | Offset | Size |
|---|---:|---:|
| nvs | 0x9000 | 0x5000 |
| otadata | 0xE000 | 0x2000 |
| app0 | 0x10000 | 0x300000 |
| spiffs | 0x310000 | 0x0E0000 |
| coredump | 0x3F0000 | 0x010000 |

A matching `partitions.csv` is included in the sketch directory.

## Restore

Your full OEM dump should be kept unchanged. To restore the original state, flash that full dump from address `0x0`.

Do not overwrite the OEM backup.
