# The serial voice link

A host on the gadget's USB cable (a Raspberry Pi running a local voice engine)
speaks through the gadget's speaker and gets every reply's text to synthesise.
Built with `CONFIG_MUSE_SERIAL_VOICE=y` (the Freenove 2.8" overlay sets it).
Everything below is additive: the console's existing `>` commands, `@chat`
frames and the Home Link commands keep working.

## The port

The gadget's own USB Serial/JTAG port, the same one `tools/muse/chat.py` uses.
On the Pi it shows up as `/dev/ttyACM0`. Baud rate is ignored (it is USB), 8N1.
Open it with DTR and RTS **low** before the first byte, and keep them there:
DTR low with RTS high resets the chip, DTR high with RTS low pulls the BOOT
pin (the talk button) low. In pyserial:

```python
ser = serial.Serial(None, 921600, timeout=0.1)
ser.port = "/dev/ttyACM0"
ser.dtr = False
ser.rts = False
ser.open()
```

The port also carries the firmware's log lines (`I (12345) tag: ...`) and the
console's JSON frames. Read it line by line and ignore what you don't know.

## Host to gadget

Each command is one line ending in `\n`, starting at column 0.

| Line | Meaning | Reply |
|---|---|---|
| `SHOW <text>` | The caption for what is about to be said: the face shows it while speaking. ASCII, one line, up to about 300 characters. | `OK` |
| `RATE <hz>` | Sample rate of the PCM that follows, 8000 to 48000. Default 11025. Set once per session. | `OK` or `ERR bad RATE` |
| `SAY <n>` | Announce `n` bytes of raw PCM: signed 16-bit little-endian, mono, no header. `n` is even, at most 1048576. **Wait for `READY` before sending the bytes.** | `READY`, then nothing, or `ERR buffer full` / `ERR timeout` |
| `END` | The utterance is over. The face goes idle once what is buffered has played. | `OK` |

`READY` means the gadget has room for the whole chunk, so you can write all
`n` bytes at once. The buffer holds 64 KB (about three seconds at 11025 Hz).
Keep chunks at 8 to 32 KB: a `SAY` is answered as soon as that much is free,
so the audio keeps flowing while you synthesise the next sentence. If no bytes
arrive for 3 s after `READY`, the gadget gives up on that chunk.

Playback starts with the first bytes; the avatar's mouth moves with the audio
and stops when `END` has been seen and the buffer is empty, or after 1.5 s of
silence mid-utterance, or 8 s after `SHOW` with no audio. A press of the talk
button cuts the stream off.

A typical utterance:

```
SHOW Right then, what are we building today?
SAY 22050
  ...wait for READY, write 22050 bytes...
SAY 18000
  ...wait for READY, write 18000 bytes...
END
```

## Gadget to host

When Muse answers a voice turn (the user pressed the talk button and spoke),
each assistant message's complete text comes out as a console frame:

```
@chat {"seq":41,"type":"reply","msg":0,"text":"Right then, what are we building today?"}
```

Standard JSON escaping. There may be several messages per turn (`msg` 0, 1,
...). Typed turns from `chat.py` keep their existing `text` / `final` /
`message_done` frames and do **not** produce `reply` frames.

The gadget's answer is: read `reply` frames, synthesise, `SHOW` + `SAY` + `END`.

## What the gadget does meanwhile

Nothing changes for the rest of the firmware. If Muse's own spoken reply
arrives (the board has a reply decoder), both would play; keep the speaker
setting as you like it. Timers, menus and messages from the Home Link
commands keep working; a `SHOW` caption is shown on the reply page like a
`screen.show_text` message and is cleared when the stream ends.

## Over the network

No cable needed: the gadget listens on **TCP port 7343** (CONFIG_MUSE_VOICE_LINK_PORT)
on your Wi-Fi and speaks exactly the same protocol. Connect, read the greeting
line `HELLO muse-voice 1 <board name>`, then send `RATE`, `SHOW`, `SAY` (wait for
`READY`, send the bytes) and `END` as above. Replies are the same lines. `PING`
answers `PONG`. One client at a time; the gadget drops the stream if the client
goes away.

The console's `@chat` frames, including the `reply` frames with the text to
speak, are sent on the same socket, so the network client needs nothing from
the USB port at all.

To find the gadget, listen on **UDP port 7343**: while no client is connected
it broadcasts `MUSEVOICE <ip> 7343 <board name>` every 5 seconds. Or ask Muse
for `gadget.status`, which includes the Wi-Fi IP.

```python
import socket, json
s = socket.create_connection(("192.168.1.108", 7343))
f = s.makefile("rwb", buffering=0)
print(f.readline())              # HELLO ...
f.write(b"RATE 11025\n"); f.readline()
f.write(b"SHOW Hello\n"); f.readline()
pcm = open("hello.raw", "rb").read()   # s16le mono 11025 Hz
f.write(b"SAY %d\n" % len(pcm))
assert f.readline().strip() == b"READY"
f.write(pcm)
f.write(b"END\n"); f.readline()
```

## Firmware updates without a cable

Muse's `device.ota` command takes an **https** URL of a `muse-gadget.bin` and
flashes it over the air, then reboots; pairing and Wi-Fi survive. A GitHub
release asset of this repository is a fine place to put a build. Plain
`http://` is refused by the OTA client, so a local web server on the Pi would
need a certificate; use GitHub.

## Flashing from the Pi

```sh
python -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 --before default-reset --after hard-reset \
    write-flash --flash-mode dio --flash-size keep --flash-freq 80m \
    0x0 bootloader.bin 0x10000 partition-table.bin 0x1d000 ota_data_initial.bin 0x20000 muse-gadget.bin
```

Those four files come out of `build-muse-freenove-s3-28/` (and its
`bootloader/` and `partition_table/` folders) after `idf.py build`. Reflashing
keeps the pairing and Wi-Fi settings. Close the voice daemon's port first.
