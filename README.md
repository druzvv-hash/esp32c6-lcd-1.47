# R1 Terminal for ESP32-C6-LCD-1.47

Compact wireless status terminal for the **R1-S3 Logger**, built on the
**Waveshare ESP32-C6-LCD-1.47-M**.

The terminal connects directly to the Wi-Fi access point created by R1-S3,
reads its HTTP API, and shows live measurements, recorder state, hardware
health, and R3 link status on a small 172×320 display.

The device is intentionally **read-only**. The 1.47" display has no
touchscreen, so it is best used as a pocket status monitor rather than as a
full control panel.

## Current status

**Working — v0.3.5**

Confirmed working:

- Wi-Fi connection to R1-S3
- live current, voltage, and power
- requested / actual measurement rate
- SD state
- recorder state
- recorded time / rows / file size
- accumulated Wh / Ah
- FIFO / overflow / missed / invalid counters
- INA228 state and temperature
- RTC state
- EEPROM state
- R3 BLE connection state
- R3 authentication / freshness
- R3 RTC synchronization
- Wi-Fi RSSI
- R1 uptime
- terminal firmware version + Git commit hash
- first-time setup portal
- Web OTA
- dual OTA partitions
- configuration stored in NVS

## Hardware

### Terminal

Waveshare **ESP32-C6-LCD-1.47-M**

- ESP32-C6
- 8 MB Flash detected
- ST7789 LCD
- 172×320 pixels
- Wi-Fi 6
- BLE 5
- IEEE 802.15.4
- USB-C
- BOOT button

### LCD pinout

| Function | GPIO |
|---|---:|
| MOSI | 6 |
| SCLK | 7 |
| CS | 14 |
| DC | 15 |
| RESET | 21 |
| Backlight | 22 |
| BOOT button | 9 |

LCD configuration:

```text
Controller: ST7789
Resolution: 172 x 320
X offset:   34
```

## Architecture

```text
┌─────────────────────┐
│       R1-S3         │
│                     │
│ INA228              │
│ SD / RTC / EEPROM   │
│ BLE link to R3      │
│                     │
│ Wi-Fi Access Point  │
│ HTTP API            │
└──────────┬──────────┘
           │ Wi-Fi
           │
┌──────────▼──────────┐
│  ESP32-C6 Terminal  │
│                     │
│ 1.47" ST7789 LCD    │
│ HTTP API client     │
│ NVS configuration   │
│ Web OTA             │
└─────────────────────┘
```

## Display pages

Short BOOT press cycles through three pages.

### 1/3 — Live

Shows:

- current
- voltage
- power
- recorder ON/OFF
- requested / actual measurement rate
- SD state
- R3 link state
- Wi-Fi RSSI
- R1 firmware version
- terminal firmware version
- terminal Git commit hash

### 2/3 — Recording

Shows:

- recording state
- recording duration
- row count
- file size
- accumulated energy in Wh
- accumulated charge in Ah
- FIFO usage
- FIFO high-water mark
- overflows
- missed samples
- invalid samples

### 3/3 — Status

Shows:

- INA228 status
- SD status
- RTC status
- EEPROM status
- INA228 temperature
- Wi-Fi RSSI
- R3 BLE state
- R3 authentication / freshness
- R3 RTC synchronization
- preview drops
- R1 uptime

## Button controls

```text
Short BOOT press
    Next display page

BOOT >= 2 seconds
    Enter OTA mode

BOOT >= 6 seconds
    Forget stored R1 Wi-Fi configuration
    and return to setup mode
```

BOOT is GPIO9.

## First-time configuration

If R1 credentials are not stored, the terminal starts its own setup access
point:

```text
SSID:     R1TERM-SETUP
Password: r1terminal
```

Connect a phone to this network and open:

```text
http://192.168.4.1
```

Enter the R1-S3 Wi-Fi SSID and password. The credentials are stored in NVS,
then the terminal reboots and connects directly to R1-S3.

## OTA firmware update

Hold BOOT for about 2 seconds.

The terminal creates:

```text
SSID:     C6-OTA
Password: C6update47
```

Connect with a phone and open:

```text
http://192.168.4.1
```

Upload:

```text
OTA_UPDATE.bin
```

After a successful update the ESP32-C6 reboots into the newly written OTA
slot.

## Flash layout

The project uses an 8 MB dual-OTA layout:

```text
0x000000  Bootloader
0x008000  Partition table
0x009000  NVS
0x00E000  OTA data

0x010000  app0 / OTA_0   3 MB
0x310000  app1 / OTA_1   3 MB

0x610000  SPIFFS
0x7F0000  Core dump
0x800000  End of Flash
```

The original factory demo was built for a 4 MB layout even though this board
reports 8 MB Flash.

## R1 API

The terminal currently uses:

```text
GET /api/state
GET /api/live?after=<cursor>
```

`/api/state` supplies system state and diagnostics.

`/api/live` supplies the latest measurement samples.

Typical live sample:

```text
[id, timestamp_us, voltage, current, quality, revision]
```

Power is calculated locally:

```text
power = voltage × current
```

## Phone-only development workflow

The entire development and update path has been tested using only an Android
phone.

```text
Android / Termux
      ↓
edit code
      ↓
git commit
      ↓
git push
      ↓
GitHub Actions
      ↓
OTA_UPDATE.bin
      ↓
C6-OTA / Web OTA
      ↓
reboot into new firmware
```

No local Arduino IDE or ESP-IDF installation is required for normal
development.

## Build

GitHub Actions installs:

- Arduino ESP32 core 3.3.2
- GFX Library for Arduino 1.6.6
- ArduinoJson 7.4.3

The build embeds a short Git commit hash into the firmware.

Generated artifact:

```text
R1-Terminal-OTA/
├── OTA_UPDATE.bin
├── app_0x10000.bin
├── bootloader_0x0.bin
├── partitions_0x8000.bin
├── SHA256SUMS.txt
└── README.txt
```

For normal updates use only `OTA_UPDATE.bin`.

## Repository structure

```text
.
├── .github/
│   └── workflows/
│       └── build.yml
├── R1_Terminal/
│   ├── R1_Terminal.ino
│   └── partitions.csv
├── ESP32C6_LCD_Phone_Test/
│   └── earlier hardware / OTA test firmware
└── README.md
```

## Design philosophy

The role of this board is simple:

> Glance at the logger and immediately know what it is doing.

It is not intended to replace the full R1-S3 web interface.

A future controller with START/STOP, profile selection, measurement-rate
control, and file management would make more sense on a larger touchscreen.

## Project milestone

This project demonstrated:

1. factory firmware backup from Android
2. ESP32-C6 flashing directly from Android
3. Git development from Termux
4. cloud compilation with GitHub Actions
5. 8 MB dual-slot OTA
6. Web OTA from Android
7. Wi-Fi connection to R1-S3
8. live R1 telemetry
9. recorder telemetry
10. R1/R3 diagnostic status

The ESP32-C6-LCD-1.47-M is now a functional wireless pocket terminal for the
R1-S3 Logger.

## Related project

R1-S3 Logger:

https://github.com/druzvv-hash/r1-s3-logger
