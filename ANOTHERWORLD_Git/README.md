# ANOTHERWORLD — native Another World engine player for ESP32-S3

*Luc Dion, 31-10-2026 — version 0.1.12*

ANOTHERWORLD runs **Another World** (Éric Chahi, Delphine Software, 1991) on an ESP32-S3. It was written for the **ESP32-CoCo3-PRO** board (a Tandy Color Computer 3 emulator board) and uses the board's VGA output, I2S PDM audio, SD card and USB keyboard.

This is **not an Amiga emulator**. The game was built as a bytecode virtual machine plus data files, so the engine was rewritten in C++ from public descriptions of the data format:
- a bytecode VM with 64 cooperative threads and 256 variables;
- a polygon renderer working on 4 pages of 320×200 in 16 colors;
- a 4-voice sample mixer and a music sequencer.

The original game files are read from the SD card.

> **No game data is included.** You need your own copy of the game (the Amiga or DOS version). *Another World* and *Out of This World* are trademarks of their respective owners. This project is not affiliated with them.

## Status

- Tested with the **Amiga French version (Delphine + U.S. Gold)**: the introduction and the game run. Speed, music and sound effects are correct, and in-game texts appear at the right time and place.
- The DOS version uses the same engine and file layout (`MEMLIST.BIN` + `BANKxx`), but it has not been tested.
- The copy-protection screen (part 16000) is skipped. The access-code screen (part 16009) is reached with the **C** key.

## Hardware (ESP32-CoCo3-PRO pinout)

These are the same GPIOs as the CoCo3-PRO firmware, so the board needs no change.

| Function | GPIO | Notes |
|---|---|---|
| VGA red | 8, 9 | 2 bits per channel, resistor DAC (64 colors) |
| VGA green | 6, 7 | |
| VGA blue | 4, 5 | |
| VGA HSYNC / VSYNC | 2 / 1 | driven by the ESP32-S3 LCD_CAM peripheral and DMA |
| Audio left (or mono) | 47 | I2S PDM, line 1 |
| Audio right | 12 | I2S PDM, line 2 (two-line DAC mode), stereo build only |
| SD card (1-bit SD_MMC) | CMD 38, CLK 39, D0 40 | |
| USB keyboard | native USB OTG port | via EspUsbHost |

**Audio: mono or stereo.** The CoCo3-PRO is designed for stereo, but the test PCB only has the left output (GPIO 47) wired. `AW_AUDIO_STEREO` in `aw_config.h` selects the mode:
- `0` (default): **mono** on GPIO 47, with the 4 voices mixed at equal level.
- `1`: **stereo** on GPIO 47 + 12. Voices 0 and 3 go left and voices 1 and 2 go right, as on the Amiga's Paula chip, with a 25 % cross-feed for headphones. If the right line cannot be initialised, the player falls back to mono.

Requirements:
- ESP32-S3 with **8 MB OPI PSRAM** and 8 MB flash. Resources are unpacked into PSRAM.
- An SD card formatted in FAT32.

## Building

- **Toolchain:** Arduino IDE or arduino-cli with the **esp32 core 3.2.1** (ESP-IDF 5.4). Other core versions were not tested.
- **Board:** `ESP32S3 Dev Module`, with:
  - PSRAM: **OPI**;
  - Flash: QIO 120 MHz, 8 MB, default 8 MB partitions;
  - USB mode: Hardware CDC;
  - Arduino loop and events on core 1.

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32s3:USBMode=hwcdc,CPUFreq=240,FlashMode=qio120,FlashSize=8M,PartitionScheme=default_8MB,PSRAM=opi,LoopCore=1,EventsCore=1" ANOTHERWORLD
```

The USB host library (EspUsbHost) is bundled in `src/`, so no extra library is needed.

## SD card

Create a folder `/AW` at the root of the card:

```
/AW/MEMLIST.BIN    resource table
/AW/BANK01 … BANK0D  game banks (BANK05 does not exist on the Amiga disks)
/AW/STRINGS.TXT    in-game texts (optional)
```

- **DOS version:** copy `MEMLIST.BIN` and the `BANKxx` files as they are.
- **Amiga version:** the two ADF disk images contain the `bankXX` files and the executable `another`. The Amiga version has no `MEMLIST.BIN`: the table is built into the executable, so it must be extracted. The scripts in [`tools/`](tools) do this. They are read-only and never modify your files.

```bash
python -I tools/adf_extract.py disk1.adf out/disk1
```
```bash
python -I tools/adf_extract.py disk2.adf out/disk2
```
```bash
python -I tools/check_res.py out/disk1/another 0x5E5A out/AW out/disk1 out/disk2
```
```bash
python -I tools/make_strings.py out/disk1/another 0x2E4C 0x3900 out/AW/STRINGS.TXT
```

The offsets `0x5E5A` (resource table, 146 entries) and `0x2E4C`–`0x3900` (text table) are those of the Delphine + U.S. Gold executable. `check_res.py` writes `MEMLIST.BIN`, then unpacks every entry and verifies its checksum. With another release, `make_memlist.py` helps locate the table. Finally, copy the `bankXX` files next to `MEMLIST.BIN`, named `BANK01`, `BANK02`, and so on.

`STRINGS.TXT` holds one text per line, as `id=text`. The id is hexadecimal and `\n` marks a line break.

## Keys

| USB keyboard | Serial (921600 baud) | Action |
|---|---|---|
| Arrows | w a s d | move |
| Space / Enter / Ctrl | space | action (run, shoot, jump) |
| P | p | pause |
| C | c | access-code screen |
| F2 | x | cycle color rendering modes |
| F3 | m | mute |
| F4 | i | sample interpolation on/off (off = Amiga-like) |
| F5 … F11 | 1 … 7 | jump to part 16001 … 16007 |
| `-` / `=` (two keys right of 0) | `-` / `=` | brightness (gamma) darker / lighter |
| `[` / `]` | `[` / `]` | VGA line length −1 / +1 (see below) |
| | g | dump palette and the 4 pages over serial (diagnostic) |
| | t | trace text drawing over serial |

The bracket and dash keys are read by physical position, so they work the same on any keyboard layout.

## How the picture is made

The game uses 16 colors per palette, taken from 4096 (12-bit RGB). The board's VGA has only 2 bits per channel, i.e. 64 colors, with no level between black and 33 %. Plain rounding gives wrong hues: brown turns red, blue-grey turns green-grey. The default rendering, **checker frc**, works like this:

1. **Dithering in linear light.** Each Amiga channel is placed between the two VGA levels that surround it (gamma 2.2). Close to a level, the channel is drawn solid; between two levels, it is drawn as a 50 % checkerboard. The average hue is right.
2. **480 real lines.** Each game line is output on 2 VGA lines that are dithered in opposite phase, which makes the pattern twice as fine.
3. **Temporal alternation (FRC).** Two buffer pairs are kept. On every vertical sync, an LCD_CAM interrupt swaps the displayed buffer for one holding the inverted checkerboard. Each dithered pixel therefore alternates between its two levels at 60 Hz, like the FRC of 6-bit LCD panels: the eye sees the mean color and no fixed pattern.
4. **Exact line length.** The horizontal total is 400 dots, the 640×480 standard, so an LCD monitor samples the signal without drifting. Without this, the checkerboard shows regular dark vertical bars. If your monitor still shows bars, press its *Auto adjust* button, then fine-tune with `[` / `]`.

F2 cycles through the other modes for comparison: `checker` (static), `bayer 4x4`, `oklab` (nearest color or checker pair in OKLab space), `oklab nearest` and `raw`.

## How the sound is made

- **Mixing.** 4 voices are mixed at 48 kHz from 8-bit signed samples. By default, each sample is held until the next one, as on the Amiga, which keeps the sound crisp. F4 switches to linear interpolation.
- **Music.** The sequencer runs inside the audio task, and the sync mark used by the bytecode is written to variable 0xF4.
- **Output.** I2S PDM, on one line (mono) or two lines (stereo). A PDM stream at 50 % density hisses at rest. To avoid that, a varying DC "floor" keeps the signal at or above 0, so silence is 0 % density and makes no noise. The floor looks 21 ms ahead and ramps up gently before a sound starts, so attacks are not masked by a click.

## Source layout

| File | Contents |
|---|---|
| `ANOTHERWORLD.ino` | setup, VGA mode, SD mount, error screens, main loop |
| `aw_config.h` | version, pins, constants |
| `aw_res.*` | MEMLIST, banks, ByteKiller unpacking, game parts |
| `aw_vm.*` | bytecode virtual machine |
| `aw_video.*` | pages, polygons, palettes, color mapping, VGA output, text |
| `aw_sound.*` | mixer, music sequencer, PDM output |
| `aw_input.*` | USB keyboard and serial keys |
| `VGA.*`, `Mode.*`, `PinConfig.h`, `DMAVideoBuffer.h` | VGA driver (bitluni's ESP32-S3 VGA, modified for the ESP32-CoCo3 project) |
| `src/EspUsbHost/` | USB host library (third party, see below) |
| `tools/` | SD card preparation scripts (Python 3) |

Comments in the source are in French. [`LISEZMOI.TXT`](LISEZMOI.TXT) is a French summary.

## Credits and licenses

- ANOTHERWORLD engine, tools and documentation: © 2026 Luc Dion, [MIT License](LICENSE).
- VGA driver (`VGA`, `Mode`, `PinConfig`, `DMAVideoBuffer`): bitluni's ESP32-S3 VGA library, modified by Luc Dion for the ESP32-CoCo3 project. The upstream code keeps its own license (check bitluni's repository before redistributing).
- [EspUsbHost](https://github.com/tanakamasayuki/EspUsbHost) 2.7.0 by TANAKA Masayuki, MIT License (`src/EspUsbHost/LICENSE`), with a small HID-interface filter patch (diffs included).
- `glcdfont.h`: the classic 5×7 font of Adafruit_GFX, BSD License.
- `spiram.h`: Espressif Systems, Apache License 2.0.
- Thanks to the people who documented the Another World engine and data formats publicly over the years.
- *Another World* © Éric Chahi / Delphine Software. No original code or data is distributed here.
