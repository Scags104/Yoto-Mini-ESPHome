# Yoto Mini – Open ESPHome Firmware

Custom [ESPHome](https://esphome.io) firmware that turns a **Yoto Mini** into a fully local,
Home Assistant–integrated kids' audio player: NFC cards, Music Assistant streaming,
Bluetooth from a phone, offline playback from the built-in storage, an on-device menu,
and a colour screen – with no Yoto cloud account involved.

> **Built with the help of Claude (Anthropic).** The firmware, custom components and this
> documentation were developed collaboratively with Claude. Every feature listed below has
> been tested and is working on real hardware.

---

## Contents

- [Features](#features)
- [Supported audio formats](#supported-audio-formats)
- [Hardware](#hardware)
- [Controls](#controls)
- [On-device menu](#on-device-menu)
- [Screen](#screen)
- [Home Assistant integration](#home-assistant-integration)
- [Built-in storage](#built-in-storage)
- [Installation](#installation)
- [Configuration notes](#configuration-notes)
- [Troubleshooting](#troubleshooting)
- [Restoring the stock firmware](#restoring-the-stock-firmware)
- [Credits](#credits)
- [Disclaimer](#disclaimer)

---

## Features

### Audio
- **Speaker** via the AW88194 smart amp (stock factory DSP setup ported from Open Yoto)
- **Headphones** via the ES8156 DAC, with **automatic speaker/headphone switching**
- **Music Assistant** playback over **Sendspin**, including per-source pause/resume
- **Bluetooth A2DP** – stream from a phone to the Yoto, with track info and phone controls (AVRCP)
- **Local playback** from the built-in eMMC storage (folders play as playlists)
- **Chimes** for startup, card scans, power-off, low battery, Bluetooth pairing and connection
- Volume applied in software, so the knob controls speaker and headphones alike
- Built-in mixer and resampler: chimes play over music; non-44.1 kHz audio is converted

### Cards (NFC)
- CR95HF reader driver (written for this project)
- Card scans become **Home Assistant tag events** (for Node-RED or automations)
- Card removal event, and an on-screen tick/cross from your automation (`show_result`)

### Controls
- Two rotary knobs decoded in **hardware (ESP32 PCNT)** with a glitch filter – no missed or
  reversed steps on fast spins
- Knob push buttons, power button, two-button **menu gesture**

### Screen (240×240 colour)
- Clock (12-hour, kept by the onboard RTC even without Wi-Fi)
- Now playing: title, artist/track number, cover art, paused state
- Volume overlay, card feedback, Bluetooth pairing screen
- Status bar: headphones, Wi-Fi signal, Bluetooth, battery %/icon
- Screen timeout after inactivity; brightness control
- **Themes:** Dark, Light, Night (dim red, sleep-friendly), Colourful
- **Clock styles:** Digital, Analog (clock face with hands), Minimal
- **Night mode:** automatically switches to the Night theme and dims the screen between set hours

### Power & battery
- Battery level and voltage (CW2215B fuel gauge), charging state and USB detection (SGM41513)
- Stock charger configuration (ported from Open Yoto)
- **Charge limit** (battery care) – hold the charge at e.g. 80 % while on USB
- **Auto power-off** after inactivity on battery
- **Low-battery** chime at 7 %, automatic power-off at 3 %
- **Sleep timer** (15–60 min)
- Hold the power button for a clean power-off (saves the audiobook position first)

### Built-in storage
- eMMC mounted as FAT with long UTF-8 file names
- **Web file manager**: browse, upload (with progress), download, delete (including whole
  folders), create folders, play
- **Resume** where you left off, per book folder
- **Cover art** from `cover.jpg` in the book folder
- Opened from Home Assistant's **"Visit"** link on the device page

### Connectivity
- Wi-Fi with Home Assistant API (encrypted) and encrypted OTA updates
- **Away mode**: Bluetooth turns on automatically after 2 minutes without home Wi-Fi, and off
  again once you're home and no phone is connected
- mDNS discovery for Home Assistant, Music Assistant (Sendspin) and the file manager

---

## Supported audio formats

### Built-in storage

| Format | Extension | Status | Notes |
| --- | --- | --- | --- |
| MP3 | `.mp3` | ✅ Recommended | 44.1 kHz plays without resampling. Supports mid-file resume. |
| FLAC | `.flac` | ✅ Supported | Resumes at the start of the saved track. |
| WAV | `.wav` | ✅ Supported | Resumes at the start of the saved track. |
| Opus | `.opus`, Ogg-Opus `.ogg` | ⚠️ May stutter | Always 48 kHz; decode + resample is heavy for this chip. |
| Ogg Vorbis | `.ogg` | ❌ Not supported | Not included in ESPHome. Skipped with a log message. |
| AAC | `.m4a`, `.m4b`, `.aac` | ❌ Not supported | Not included in ESPHome's decoder. Convert to MP3. |

**Recommended encoding for audiobooks** (mono, 64 kbps, ~28 MB per hour):

```bash
ffmpeg -i input.m4b -ac 1 -ar 44100 -c:a libmp3lame -b:a 64k output.mp3
```

For music, use `-ac 2 -b:a 128k`.

### Streaming sources

| Source | Format |
| --- | --- |
| Music Assistant (Sendspin) | FLAC preferred (PCM fallback), 44.1 kHz / 16-bit / stereo, decoded in internal RAM |
| Bluetooth from a phone | A2DP, received at 44.1 kHz / 16-bit |
| Home Assistant `play_media` URLs | MP3, FLAC, WAV over http(s) |

### Cover art

`cover.jpg` (also checked: `folder.jpg`, `cover.jpeg`, `cover.png`, `folder.png`) in the book
folder, shown at up to 120×120. The on-device JPEG decoder needs a **baseline, 4:2:0**
JPEG – make covers with:

```bash
convert input.png -resize 240x -sampling-factor 4:2:0 -strip -interlace none -quality 85 cover.jpg
```

(`magick` instead of `convert` on ImageMagick 7.)

---

## Hardware

Tested on **Yoto Mini, board configuration #04** (colour GC9306 screen, single IO expander).
Hardware details come from the Open Yoto project and Adafruit's Yoto work.

| Part | Function | Connection |
| --- | --- | --- |
| ESP32-WROVER-E (N8R8) | MCU, 8 MB flash, 8 MB PSRAM | – |
| ET6416 IO expander | Buttons, power hold, display CS/DC/RST, amp control | I2C `0x20` |
| ES8156 | Headphone DAC | I2C `0x08`, I2S |
| AW88194 | Speaker smart amp | I2C `0x34`, I2S |
| CR95HF | NFC reader | UART1 (GPIO32/33), 57600 8N2 |
| GC9306 | 240×240 TFT | SPI (GPIO23/22), backlight GPIO26 |
| CW2215B | Battery fuel gauge | I2C `0x64` |
| SGM41513 | Charger | I2C `0x1A` |
| PCF8563-compatible | Real-time clock | I2C `0x51` |
| eMMC | Storage | SDMMC 4-bit (GPIO14/15/2/4/12/13) |
| Rotary encoders | Knobs | GPIO39/35 (left), GPIO36/27 (right) |

I2C: SDA GPIO21, SCL GPIO25. I2S: MCLK GPIO0, BCLK GPIO5, LRCLK GPIO18, DOUT GPIO19.

---

## Controls

| Control | Action |
| --- | --- |
| Left knob – turn | Volume |
| Right knob – turn | Next / previous track (phone, Music Assistant or storage folder) |
| Either knob – click | Play / pause (per source) |
| Left knob – hold 0.8 s | **Full stop** (any source; also stops the phone over Bluetooth) |
| Right knob – hold 3 s | Bluetooth pairing (3-minute window) |
| Both knob buttons together | Open / close the menu |
| Power button – press | Screen on/off (closes the menu if open) |
| Power button – hold 2 s | Power off (on battery) |

**Play/pause by source:**
- **Bluetooth** – play/pause on the phone
- **Storage** – local pause/resume; when stopped, a click resumes the last book
- **Music Assistant** – pauses and resumes the Music Assistant queue

---

## On-device menu

Open with **both knob buttons together**. Turn either knob to move, **left click = select**,
**right click = back**. Closes after 30 s without input.

- **Bluetooth** – on/off, pair new device, Bluetooth when away
- **Storage** – browse folders; *Resume* / *Play from start* / *Play this folder*; play a file
- **Sleep timer** – Off, 15, 30, 45, 60 min
- **Settings** – screen timeout, auto power-off, brightness, theme, clock style, night mode
- **Info** – IP address, Wi-Fi signal, battery, charging, storage free, uptime, ESPHome version
- **Power** – restart, power off (with confirmation)

---

## Screen

| State | Shown |
| --- | --- |
| Idle | Large clock, AM/PM and date |
| Playing / paused | Cover art (if any), title, artist or "Track N of M", "Paused" |
| Volume change | Percentage and bar |
| Card scanned | Green tick; then your automation's tick/cross and message |
| Bluetooth pairing | Bluetooth symbol, countdown |
| Status bar | Headphones · Wi-Fi signal · Bluetooth · battery % · battery |

---

## Home Assistant integration

### Entities

| Entity | Type |
| --- | --- |
| Media player | Music Assistant (Sendspin), storage, Bluetooth, HA URLs |
| Battery, Battery voltage | Sensors |
| Charging, Charging state, USB power | Binary sensor / text / binary sensor |
| Charge limit | Number (60–100 %, 100 = off) |
| Auto power-off, Screen timeout | Numbers (minutes, 0 = never) |
| Screen | Light (backlight + brightness) |
| Theme, Clock style | Selects |
| Night mode, Night mode start/end, Night mode brightness | Switch / numbers |
| Bluetooth, Bluetooth when away | Switches |
| Bluetooth connected, Bluetooth source | Binary sensor / text |
| Headphones | Binary sensor |
| Last tag | Text sensor |
| IP address, Wi-Fi signal | Diagnostics |
| Storage size, Storage free | Diagnostics (MB) |
| Heap free, PSRAM free, Uptime | Diagnostics |
| Restart | Button |

### Events

| Event | Data |
| --- | --- |
| `tag_scanned` | Standard Home Assistant tag event (tag ID = card UID, e.g. `04-A1-B2-…`) |
| `esphome.yoto_tag_removed` | `tag_id` |
| `esphome.yoto_button` | `button`: `left`, `right`, `power`, `right_knob`; `action`: `press`, `long_press`, `next`, `previous` |
| `esphome.yoto_power` | `action: power_off` |

### Actions

| Action | Fields | Use |
| --- | --- | --- |
| `esphome.yoto_show_result` | `success` (bool), `message` (text) | Show a tick/cross after matching a card |
| `esphome.yoto_play_file` | `path` (e.g. `/Books/The Hobbit`) | Play a storage file or folder |

> Enable **"Allow the device to perform Home Assistant actions"** in the ESPHome integration
> options, or tag scans and events won't reach Home Assistant.

---

## Built-in storage

- **File manager:** `http://yoto.local:8080` (or the device IP) – also Home Assistant's
  **Visit** link on the device page. Port 8081 is used internally for audio streams.
- **Folders play as playlists** in file-name order – number chapters `01 …`, `02 …`.
- **Resume:** position saved every 30 s and on pause/stop/power-off, in a hidden `.resume`
  file per folder. Finishing a book clears it.
- **Limits:** listings show the first 300 items; folder playlists take up to 500 tracks.
  Huge folders (e.g. the stock Yoto icon cache) can be removed with *Delete* / *Delete all here*.
- Hidden files (starting with `.`) are not listed.

---

## Installation

### What you need

- A Yoto Mini (board config #04), opened to the main board
- A **3.3 V** USB-to-serial adapter, fine wire / pogo pin, soldering iron, multimeter
- ESPHome **2026.9** or newer, and `esptool` v5 (`pipx install esptool`)
- The two stock amp DSP files from Open Yoto (`firmware/components/aw88194/`):
  `factory_aw88194_fw.bin` (2036 bytes) and `factory_aw88194_cfg_rev04.bin` (924 bytes)

### 1. Connect serial and enter download mode

The 5-pad header at the top of the board is **GND, EN, TX, RX, GND**. Find each with a
continuity test against the ESP32 module pins. Power the player from its own USB-C port, not
from the adapter.

On this revision **GPIO0 is not on the header** – use a pogo pin or wire on the module's
GPIO0 pin (find it by continuity). **GPIO2 must also be held low** for a working download mode.

Enter download mode: hold GPIO0 and GPIO2 to GND → tap EN to GND → release. The serial
monitor shows `boot:0x27 (DOWNLOAD_BOOT…)`. Repeat before **every** esptool command.

### 2. Back up the stock firmware

```bash
esptool --chip esp32 --port /dev/ttyUSB0 --before no-reset --after no-reset flash-id
esptool --chip esp32 --port /dev/ttyUSB0 --baud 115200 --before no-reset --after no-reset \
  read-flash 0x0 0x800000 yoto-stock.bin
esptool --chip esp32 --port /dev/ttyUSB0 --baud 115200 --before no-reset --after no-reset \
  verify-flash 0x0 yoto-stock.bin
```

The file must be exactly 8 388 608 bytes. **Keep it safe** – it's your way back to stock.

### 3. Set up the ESPHome config folder

```
yoto.yaml                     the device configuration
secrets.yaml                  wifi_ssid, wifi_password, yoto_key
components/
  yoto_aw88194/               + the two amp .bin files
  yoto_battery/
  yoto_cr95hf/
  yoto_encoder/
  yoto_menu/
  yoto_storage/
sounds/                       chime WAV files
```

`yoto_key` is a 32-byte base64 key: `openssl rand -base64 32`.

### 4. First flash (serial)

In the ESPHome dashboard: **Install → Manual download → Factory format**. Then, entering
download mode before each command:

```bash
esptool --chip esp32 --port /dev/ttyUSB0 --before no-reset --after no-reset erase-flash
esptool --chip esp32 --port /dev/ttyUSB0 --baud 115200 --before no-reset --after no-reset \
  write-flash 0x0 firmware.factory.bin
```

Release GPIO0/GPIO2 and tap EN to boot.

### 5. Every update after that

Over Wi-Fi from the ESPHome dashboard (**Install → Wirelessly**). Bootloader rollback is
enabled, so a bad update reverts automatically.

### 6. Home Assistant & Music Assistant

- Adopt the device in Home Assistant's ESPHome integration and enable **"Allow the device to
  perform Home Assistant actions"**.
- Enable the **Sendspin** provider in Music Assistant – the Yoto appears as a player.

---

## Configuration notes

- **Bluetooth component** is pinned: `github://cociweb/a2dp-sink@54e644c1cc8d3acbb2b89432ab87a5cae30b68ac`.
  It needs `include_builtin_idf_components: [esp_coex]` under `esp32 → framework → advanced`.
- **`advertise_web_ui: true`** (on `yoto_storage`) powers the "Visit" link. Don't also declare
  an `mdns: services:` entry for port 8080.
- **Charge limit** only switches charging off/on (5-point hysteresis); it never changes the
  charge voltage.
- **Sendspin artwork** is configured and will display once Music Assistant sends artwork.
- Fonts are downloaded from Google Fonts at build time.

---

## Troubleshooting

| Symptom | Fix |
| --- | --- |
| `esptool` can't talk to the flash (`ff`/`00`) | Hold **GPIO2** low as well as GPIO0 when entering download mode. |
| Music Assistant can't find the player | `avahi-browse -art \| grep -i yoto` should list `_sendspin._tcp`. Clean Build Files and reinstall. |
| No events / tag scans in Home Assistant | Enable "Allow the device to perform Home Assistant actions". |
| A file won't play | Check the format table; convert to MP3 44.1 kHz. |
| Cover art fails (`Error while decoding`) | Re-make the cover with the ImageMagick command above. |
| Opus stutters | Known limitation – convert to MP3. |
| Knob turns the wrong way | Add `invert: true` to that knob under `yoto_encoder`. |
| Strange behaviour after config changes | ESPHome dashboard → **Clean Build Files**, then Install. |
| Playback glitchy right after an update | Reboot once; check Wi-Fi signal (better than about -70 dBm). |

---

## Restoring the stock firmware

Enter download mode and write your backup:

```bash
esptool --chip esp32 --port /dev/ttyUSB0 --baud 115200 --before no-reset --after no-reset \
  write-flash 0x0 yoto-stock.bin
```

The eMMC contents are not part of the flash backup.

---

## Credits

- **[Open Yoto](https://github.com/tensorfish/open-yoto)** – hardware research, pin maps,
  AW88194 amp, CR95HF and charger sequences
- **Adafruit** – Yoto Mini reverse engineering, ES8156 and display setup
- **[ESPHome](https://esphome.io)** – firmware framework, Sendspin, audio pipeline
- **[cociweb/a2dp-sink](https://github.com/cociweb/a2dp-sink)** – Bluetooth A2DP sink component
- **[Music Assistant](https://music-assistant.io)** – Sendspin streaming
- **Claude (Anthropic)** – co-developed the custom components, configuration and documentation

---

## Disclaimer

Not affiliated with or endorsed by Yoto. Opening the device and replacing its firmware voids
the warranty and is done at your own risk. The stock amp DSP files are Yoto's data and are
**not included** – obtain them yourself and don't redistribute them. Back up your stock
firmware before flashing.
