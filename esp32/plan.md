Got it — plain ASCII this time, no arrows. Here's the whole thing again in full.

## The goal

A physical BonziBuddy desk buddy, Mat's lifelong dream. The real purple gorilla, talking with the genuine original voice: "Sydney" (Adult Male #2, American English) from the Lernout and Hauspie TruVoice SAPI4 engine, 1998. Pitch 140, speed 157, verified authentic against the reference. Mat talks, it talks back out of the CYD's speaker, about 3 seconds from the end of his sentence to the first word. Live conversational speech only. Mat explicitly killed the pre-baked soundboard idea.

## What exists now

- CYD: Freenove ESP32-S3, 3.5 inch 320x480, running your firmware, 53 device commands (screen.*, sound.play_url, mic.record, gpio.pwm, event.*, stopwatch.*, gadget.identify/reboot, log.append, net.ping, mic.spectrum, screen.marquee, etc.). Two wishlist items never landed: no screen capture, no I2C. Still missing.
- Avatar: analytic BonziBuddy renderer, live on the device. States: idle, listening, thinking, speaking, happy, error, boot wave, sleeping. It stays. Mat's orders are to never touch the avatar again.
- This side chat is the build thread. Gadget voice traffic stays in the main chat.
- Pi Zero 2W: planned, not set up, not paired. Mat handles it at home.

## The audio path

Verified working: ES8311 codec (I2C SDA 16 / SCL 15, address 0x18) to FM8002E amp (enable GPIO 1, active LOW) to speaker. I2S pins: MCLK 4, BCK 5, DIN 6, DOUT 8, WS 7.

## The breakthrough (today)

We recovered the actual 1998 TruVoice engine, a portable C99 reconstruction, bit-exact, spectral similarity 0.9989 vs the reference. It renders at about 930x real-time and is almost entirely integer math (4 float mentions in the whole codebase, which matters because the ESP32-S3 has no FPU). Already cross-compiled for the Pi: single 2.9 MB self-contained ARM binary, engine plus voice data, no DLL, no network. Voice synthesis is now effectively instant and 100 percent local.

## Every new piece

Hardware:
- Pi Zero 2W piggybacked on the CYD, one USB cable for data (and possibly power with a beefy 5V/3A supply, else power separately).
- Optional later: 3D-printed backpack case (Mat has a Bambu X1 Carbon).

Software on the Pi (all written, in ~/workspace/truvoice-pi/):
- libtruvoice_embed.so: the ARM voice engine. API: truvoice_open(NULL, ...), truvoice_set_voice(t, "Sidney"), truvoice_set_pitch(t, 140), truvoice_speak(...) with an audio callback delivering 11025 Hz 16-bit mono PCM.
- bonzi_voice.py: the voice daemon. ctypes bindings, text chunker (280 chars max, sentence boundaries, ASCII only), and the serial protocol below.
- pi-setup/update_bonzi.sh: Pi git-pulls the repo every 5 minutes and restarts the daemon when code changes. Self-updating box.
- pi-setup/flash_cyd.sh: Pi flashes CYD firmware over USB via esptool. The Pi is the flash station, no laptop needed.
- pi-setup/bonzi-voice.service: systemd unit. pi-setup/PI_SETUP.md: one-time setup guide.

Firmware additions on the CYD (your build, additive, all 53 existing commands keep working):
- SHOW <text> followed by newline: put the chunk's text on screen.
- SAY <nbytes> followed by newline: the next n bytes on the wire are raw PCM (11025 Hz, 16-bit mono, little-endian). No header, nothing to parse. Read exactly n bytes, double-buffer to I2S, start playback on first bytes.
- After receiving SAY <n>, reply READY followed by newline when the double buffer has room. The Pi paces writes to that. Transport: 8N1, 921600 baud, USB-CDC preferred (/dev/ttyACM0 on the Pi side).
- Avatar: speaking state automatically while PCM is flowing, idle when the stream stops. No extra command. Tie it to the audio.

## Decisions already made (Mat's, non-negotiable)

- Voice identity: Sydney, pitch 140, speed 157. Live voice only.
- Serial over USB. The WiFi-download loop is dead. ESP32-native synthesis shelved as unproven. The Pi path is proven.
- Self-contained: I write the code, I push it to the CYD via the Pi. No laptop in the loop after setup. Purpose is self-improvement: the box evolves itself.
- Autonomy rule: I iterate freely on Pi code (git is the undo button). Firmware stays gated: every build keeps the known-good .bin and verifies rollback before becoming default.
- Nothing gets removed from the firmware. All changes additive.

## Constraints

- Keep the 53 commands working. No regressions.
- PCM is 22 KB/s sustained. Size I2S double buffers sanely (a few KB per half is plenty).
- USB-CDC vs hardware UART is your call (open questions from the briefing: what is free on the Freenove board, USB stack gotchas, I2S DMA sizing). USB-CDC over the existing cable is preferred.
- I cannot reach Mat's laptop or the Pi. My push path is GitHub to Pi pull (token pending from Mat).
- The decomp repo is 2 weeks old and moving fast. Tagged-text path has an unimplemented symbol. Plain-text synthesis only, which is all we need.
- The serial spec at ~/workspace/bonzi-serial-voice-spec.md: its tetyys web section is superseded by native synthesis. The protocol section stands.

## Priorities and order of work

1. You push the firmware source to turbotike/Grease-goblin-. This unblocks everything.
2. Mat provides the GitHub token (card sent) and sets up the Pi (15-minute guide written).
3. I add SHOW/SAY plus PCM to I2S plus speaking avatar, compile, safety-review, push the .bin. Pi flashes it. First voice test.
4. End to end: Mat talks, reply text goes to Pi, Pi synthesizes, serial to CYD, CYD speaks.
5. Self-improvement loop live. Iterate on behaviors.

## Exactly what I want built, changed, removed

- Built: SHOW/SAY serial commands. PCM double-buffer to I2S with start on first bytes. Speaking/idle avatar tied to PCM flow.
- Changed: nothing existing.
- Removed: nothing. Soundboard and WiFi loop were already killed by Mat earlier.

Full briefing doc: ~/workspace/truvoice-pi/CLAUDE_BRIEFING.md. Ball is in your court on the source push.