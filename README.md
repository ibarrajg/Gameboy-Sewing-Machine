# ESP-GameBoy

A headless Game Boy (DMG) emulator on an **ESP32-S3**. The board is a Wi-Fi access point: your phone joins it, a browser opens a Game Boy UI, and video plus audio stream over a WebSocket. On-screen buttons (or a keyboard on desktop) send controller input back to the chip.

ROMs are stored on the ESP32’s flash. You upload them from the phone — you do not bake a cartridge into the firmware.

Use only legally obtained ROMs.

```
ESP32 Core 1          ESP32 Core 0              Phone browser
Peanut-GB  ──frame──► WebSocket binary  ──────► Canvas + Web Audio
           ◄─1-byte── controller bitmask ◄───── touch / keyboard
```

- Wi-Fi: open network **ESP-GameBoy** (channel 1, IP usually `192.168.4.1`)
- Video: 160×144, packed 2-bit (5760 bytes/frame)
- Audio: 8-bit mono PCM
- Flash library: about **14 MB** of LittleFS for `.gb` files (2 MB max each)

---

## What you need

**Hardware**

- ESP32-S3 **N16R8** (16 MB flash + 8 MB OPI PSRAM) — this is the default board
- USB-C cable that carries data (not charge-only)
- USB power: computer, phone charger, or power bank
- Phone or computer with Wi-Fi and a browser (Chrome on Android works well)

**Software**

- [Cursor](https://cursor.com/) or [VS Code](https://code.visualstudio.com/)
- [PlatformIO IDE](https://platformio.org/install/ide?install=vscode) extension (the project already recommends it)
- Python is installed automatically with PlatformIO

---

## 1. Get the project

```bash
git clone https://github.com/Artificial-Age/ESP32Gameboy.git
cd ESP32Gameboy
```

Or download the ZIP from GitHub and unzip it. Open the folder in Cursor / VS Code. When prompted, install the **PlatformIO IDE** extension and let it install the Espressif toolchain (first time can take several minutes).

---

## 2. Put the web UI on the filesystem image

LittleFS is built from the `data/` folder. Keep it in sync with `frontend/`:

| Source (edit here)     | Copy to (this is flashed) |
|------------------------|---------------------------|
| `frontend/index.html`  | `data/index.html`         |
| `frontend/style.css`   | `data/style.css`          |
| `frontend/app.js`      | `data/app.js`             |

A clone of this repo already has matching files in `data/`. Copy again whenever you change the UI.

Do **not** put copyrighted `.gb` files in `data/` if you plan to commit or push. ROM files are gitignored.

---

## 3. Plug in the board and find the serial port

1. Connect the ESP32-S3 over USB-C.
2. Windows: Device Manager → Ports (COM & LPT). Note the COM number (for example `COM14`).
3. If the port never appears, try another cable, another USB port, or hold **BOOT** while plugging in.

PlatformIO usually auto-detects the port. If upload fails, set it in `platformio.ini`:

```ini
upload_port = COM14
monitor_port = COM14
```

On macOS / Linux the port looks like `/dev/cu.usbmodem*` or `/dev/ttyACM0`.

The VS Code tasks in `.vscode/tasks.json` currently pin `COM14`. Change that to your port, or use the PlatformIO toolbar buttons instead (they auto-detect).

---

## 4. Build and upload firmware

This writes the emulator to flash. It does **not** copy the web UI or ROMs.

**PlatformIO toolbar:** click the checkmark (Build), then the right arrow (Upload).

**Terminal:**

```bash
pio run -e esp32-s3-n16r8 -t upload
```

If the serial port will not enter download mode, hold **BOOT**, start upload, then release **BOOT**.

Default environment is `esp32-s3-n16r8` (alias: `esp32-s3`). Partition table: 2 MB app + ~14 MB LittleFS (`partitions/n16r8_romlib.csv`).

---

## 5. Upload the LittleFS image (web UI)

This flashes `data/` — HTML, CSS, JS. You must do this at least once after a fresh board, and again after any UI change. **Firmware upload does not include these files.**

**PlatformIO toolbar:** cloud-upload icon — “Upload Filesystem (LittleFS)”.

**Terminal:**

```bash
pio run -e esp32-s3-n16r8 -t uploadfs
```

Optional serial log:

```bash
pio device monitor -e esp32-s3-n16r8
```

You should see the AP come up, for example:

```
[WiFi] AP SSID=ESP-GameBoy IP=192.168.4.1
```

---

## 6. Connect the phone and open the real UI

1. On the phone, join Wi-Fi **ESP-GameBoy** (open, no password). You will lose normal internet while connected — that is expected.
2. The captive-portal / sign-in sheet should appear. It is **not** the Game Boy. Tap the link to the ESP’s current address (usually `http://192.168.4.1/`), or **Open in Chrome** on Android.
3. The real page looks like a Game Boy: screen, D-pad, A/B, Select, Start.

If the portal does not open, type `http://192.168.4.1/` in Chrome yourself.

The sign-in window cannot run controls, WebSocket video, or audio. Always break out into a full browser.

Only one phone can join (the AP allows a single station).

---

## 7. Upload games (ROM library)

Games live on the ESP32, not on the phone.

1. Tap **⚙ Settings**.
2. Open **ROM library**. The flash storage meter shows used / free / total.
3. Tap **Add ROM to library** and pick a `.gb` file (max **2 MB** each).
4. Wait for the progress bar. You can add several games until flash is full.
5. Tap **Play** on a title. The device autosaves the current cartridge if needed, then reboots into that ROM.
6. Rejoin **ESP-GameBoy** if the phone drops during reboot, then open the UI again.
7. Tap the screen once to unlock audio, then play.

**Play** switches the active cartridge (tracked in `/current.txt`). **Delete** removes that ROM and its matching save.

---

## 8. Play

| Action | On-screen | Keyboard |
|--------|-----------|----------|
| D-Pad  | pad       | Arrow keys |
| A      | A         | Z or A |
| B      | B         | X or B |
| Select | SELECT    | Shift or Q |
| Start  | START     | Enter or W |

**Settings** also has:

- **Button haptics** — short vibrate on press (Android / supported browsers)
- **Stream buffer** — default 48 ms. Lower is snappier; higher is smoother on shaky Wi-Fi
- **Reboot** — flush save data and restart the SoftAP

SoftAP bandwidth can drop below 60 FPS on some phones. Firmware drops frames when the WebSocket queue is full instead of stalling the emulator.

---

## Save data

Cartridges with battery RAM autosave to `/saves/<name>.sav` (about every 15 seconds when dirty, and before reboot or ROM swap).

In **Settings → Save data**:

- Download `.sav`
- Upload a `.sav` (device reboots to apply it)
- Delete save (clears flash + RAM, then reboots)

---

## Troubleshooting

| Symptom | What to try |
|---------|-------------|
| Upload fails / port busy | Close Serial Monitor; unplug/replug; hold **BOOT** |
| Wrong COM port | Set `upload_port` in `platformio.ini`; change `.vscode/tasks.json` if you use those tasks |
| Wi-Fi appears but page is blank | You are still in the captive sheet — open the link in Chrome |
| No audio | Tap the screen once after Play; check phone silent/mute |
| “Idle — upload a ROM” in serial | Firmware is up but no cartridge is selected — use Settings → ROM library |
| UI looks old after a code change | You uploaded firmware but not LittleFS — run `uploadfs` again |
| Phone will not stay connected | Stay close; only one client is allowed; reboot the board from Settings or USB |

---

## Project layout

```
platformio.ini      Board, partitions, libraries
partitions/         16 MB layout (~14 MB ROM library)
src/main.cpp        Wi-Fi AP, captive portal, Peanut-GB, WebSocket
src/link_radio.cpp  ESP-NOW radio proof (not a Pokémon cable yet)
src/minigb_apu.c    Game Boy APU
include/            peanut_gb.h, minigb_apu.h, link_radio.h
frontend/           Web UI source
data/               LittleFS image (copy of the UI files)
```

Emulator core: [Peanut-GB](https://github.com/deltabeard/Peanut-GB) (MIT). Web stack: ESPAsyncWebServer + AsyncTCP.

Phase 2 (ESP-NOW link cable) is a **radio proof** only — Settings → Link radio proof. It is not a working Pokémon cable yet.
