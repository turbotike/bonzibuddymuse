"""avatar.py for Windows: ask your Muse, through the board, to redraw its avatar as muse_pixel.c,
save it, then build and flash with build_freenove.ps1 (the repo's board.sh is bash/macOS, and
the host-compiler check needs a `cc` this PC doesn't have, so that step is skipped: the ESP-IDF
build is the check).

  python tools\muse\avatar_win.py                     ask, save, build, flash
  python tools\muse\avatar_win.py --edit "bigger eyes"
  python tools\muse\avatar_win.py --reply components\muse\avatar\last_reply.md   reuse a saved reply
  python tools\muse\avatar_win.py --no-flash
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import avatar  # noqa: E402
import chat  # noqa: E402

ESP32 = os.path.dirname(os.path.dirname(HERE))
BUILD_PS1 = os.path.join(ESP32, "build_freenove.ps1")


def ps(*args):
    cmd = ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", BUILD_PS1, *args]
    return subprocess.run(cmd, cwd=ESP32).returncode == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--edit", metavar="CHANGE")
    ap.add_argument("--reply", metavar="FILE")
    ap.add_argument("--no-flash", action="store_true")
    a = ap.parse_args()

    port = a.port
    if a.reply:
        with open(a.reply, encoding="utf-8") as f:
            reply = f.read()
    else:
        port = port or chat.pick_port()
        avatar.say(f"Board on {port}")
        board, st = avatar.open_board(port)
        avatar.say("  " + avatar.summary(st))
        avatar.check_board(st)
        avatar.say("Asking your Muse for your avatar (this takes a few minutes)")
        try:
            reply = avatar.ask(board, avatar.request(a.edit), "reply")
        finally:
            board.close()
    if reply.strip().startswith("NO AVATAR"):
        avatar.save(None, reply)
        raise avatar.Stop("Muse couldn't find your avatar: " + reply.strip()[len("NO AVATAR"):].lstrip(": "))
    code = avatar.extract_c(reply)
    avatar.save(code, reply)
    if code is None:
        raise avatar.Stop(f"Muse's reply has no muse_pixel.c in it. Saved in {avatar.rel(avatar.LAST_REPLY)}.")
    avatar.say(f"Saved {avatar.rel(avatar.AVATAR_SRC)} ({len(code.encode())} bytes)")
    d = avatar.description(code)
    if d:
        avatar.say(f"  Muse drew: {d[:400]}")
    avatar.say("Building (the ESP-IDF compile is the check)")
    if not ps("build"):
        raise avatar.Stop(f"The firmware doesn't build with this avatar. It is at {avatar.rel(avatar.AVATAR_SRC)}; "
                          "the previous one is muse_pixel.c.prev beside it. Delete the file to go back to the default.")
    if a.no_flash:
        return
    port = port or chat.pick_port()
    avatar.say(f"Flashing {port}")
    if not ps("flash", port):
        raise avatar.Stop("Flashing failed. Hold BOOT, tap RESET, release BOOT and run: build_freenove.ps1 flash " + port)
    avatar.say("Done.")


if __name__ == "__main__":
    try:
        main()
    except avatar.Stop as e:
        avatar.say(str(e))
        sys.exit(e.code)
