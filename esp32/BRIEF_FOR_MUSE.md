Paste this to Muse in the gadget chat (from Mat, written by Claude):

The Pi is ready and paired with you as homelink-858e53 (MuseGadget858E53): hostname bonzi, 192.168.1.64, user mat with sudo, Debian 13, Python 3.13, git. You can system.run, file.read and file.write on it now, so you do not need a GitHub token to get code onto it. Do this on the Pi yourself, now:

1. Read /home/mat/README_FOR_MUSE.md and /home/mat/bonzibuddymuse/esp32/devices/SERIAL_VOICE.md (a clone of turbotike/bonzibuddymuse).
2. Install your TruVoice engine (libtruvoice_embed.so, Sydney, pitch 140, speed 157) under /home/mat/truvoice-pi.
3. Install your voice daemon. The gadget is the 3.5 inch board at 192.168.1.162, TCP port 7343, over Wi-Fi. There is no USB cable and there never will be. Connect there, read the @chat frames of type "reply" (each voice turn's answer text), synthesise, and stream it back with SHOW, RATE 11025, SAY n (wait for READY), END, exactly as the document says. If the IP changes, the gadget broadcasts "MUSEVOICE <ip> 7343 <name>" on UDP 7343. Reference code for the protocol side is at /home/mat/bonzi-voice/bonzi_voice.py (a working client with an espeak placeholder, not running): reuse its Link class and plug your engine into tts().
4. Run the daemon as a systemd service that restarts on failure and starts at boot.
5. When it runs, say a test sentence through the gadget and tell Mat it is done.

Everything you need is on the Pi. Reply briefly with what you did and anything that blocked you.
