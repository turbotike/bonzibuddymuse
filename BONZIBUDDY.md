# BonziBuddy Muse gadget

> **2026-10-06: the gadget is now a pet.** The Pi voice (TruVoice) and the
> Bonzi sprites are retired: Meta's SDK does not speak gadget replies by
> itself, so the voice only ever came through the Pi, and the Pi's link to the
> board kept dying. The board now raises a creature of its own with Muse as
> its spirit; see [PET.md](PET.md). Everything below still describes the
> build, the board port and the tools, which the pet is built on. The Bonzi
> avatar is kept as `esp32/components/muse/avatar/muse_pixel.c.bonzi`.

Mat's fork of Meta's Muse Gadget SDK for the Freenove ESP32-S3 Display boards
(2.8" FNK0104AB and 3.5" FNK0104N): the stock Muse gadget plus

- the BonziBuddy avatar (esp32/components/muse/avatar/muse_pixel.c),
- 53 Home Link commands Muse can call on the device (esp32/main/gadget_tools.c,
  gadget_more.c, gadget_canvas.c; tappable menus in components/muse/muse_prompt.c),
- a side chat of its own (CONFIG_MUSE_CHAT_SESSION_ID),
- the Winamp-style settings skin (CONFIG_MUSE_SKIN_WINAMP),
- the serial voice link for a Raspberry Pi running the TruVoice "Sydney" engine:
  esp32/devices/SERIAL_VOICE.md (CONFIG_MUSE_SERIAL_VOICE).

The plan this serves is in esp32/plan.md.

## Build

ESP-IDF v6.0.1 only. Put your Muse SDK token in `esp32/sdkconfig.token`
(git-ignored):

    CONFIG_GADGET_SDK_TOKEN="mgst_..."

Windows:

    cd esp32
    .\build_freenove.ps1 build -Board 35        # or 28
    .\build_freenove.ps1 flash COM6 -Board 35

macOS / Linux:

    cd esp32
    tools/muse/board.sh freenove35 build
    tools/muse/board.sh freenove35 flash /dev/ttyACM0

The firmware lands in `esp32/build-muse-freenove-s3-35/muse-gadget.bin` with
its bootloader, partition table and OTA data beside it; SERIAL_VOICE.md has
the esptool line to flash all four from a Pi.

Board overlays: `esp32/devices/sdkconfig.muse-freenove-s3-35` and `-28`.
Board drivers: `esp32/components/muse/boards/board_freenove_s3_35.c` (ST77922
over QSPI with Freenove's init table, the in-cell touch in touch_freenove35.c)
and `board_freenove_s3_28.c` (ILI9341, FT6336U).

Serial console gotcha on Windows: open the port with DTR and RTS low before
the first byte or the chip resets (tools/muse/chat.py does this).

## Gotchas learned the hard way

- The ST77922 (3.5") only accepts flush windows whose x start is a multiple of
  4 and whose x end is 4n+3. Anything else draws as noise. The board driver
  registers an LVGL rounder (LV_EVENT_INVALIDATE_AREA) that widens every area;
  keep it if you touch the display code.
- Freenove's own ST77922 init table is sent verbatim as the vendor init; the
  driver warns that 3Ah and 36h get overwritten by it, which is intended.
- On Windows, opening the USB serial port with DTR high resets the chip and
  DTR high with RTS low holds BOOT (download mode). Open with both low.
- After editing a Kconfig, run a reconfigure before the build; the generated
  sdkconfig does not pick up new symbols on its own.
- The 320x480 panel uses the same compact Winamp layouts as the 240x320 one:
  the thresholds in muse_ui.c and muse_settings_ui.c are `width < 340`.
- While Bonzi answers on the 3.5", he stays full size under the status row and
  the whole reply scrolls in a box under his feet (muse_ui.c `build_transcript`,
  `update_transcript`): what he heard in dim type, then the reply as the chat
  session streams it (muse_state_set_transcript, pushed from
  muse_chat_session.cpp `push_transcript`). The box follows the line being
  spoken (Muse's caption page, or the Pi's SHOW sentence, found in the text by
  substring); a finger scroll pauses that for 8 s. The transcript stays up 45 s
  after the reply, longer with each scroll, until the next press. The 240x320
  layout (small Muse, paged text) is still there for screens with less room:
  the choice is made from `muse_pixel_blank_rows()`, which the generated sprite
  file defines (the figure is centred in its square canvas).
