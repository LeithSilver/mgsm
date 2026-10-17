"""PS Vita test loop (README.md): talks to the vitacompanion plugin and to the game's log port.

  python tools/vita.py deploy [--no-launch]   quit the app, send build/eboot.bin over FTP, launch it
  python tools/vita.py log [out.txt]          print the game's log (port 17492), reconnecting; also saved to a file
  python tools/vita.py cmd <text>             send a vitacompanion command (quit <id>, launch <id>, press cross, reboot, screen on)
  python tools/vita.py put <local> <remote>   upload one file, e.g. put x.txt ux0:data/mgs_mobile/x.txt
  python tools/vita.py get <remote> <local>   download one file (crash dumps: ux0:data/*.psp2dmp)
  python tools/vita.py ls <remote dir>        list a directory
  python tools/vita.py press <buttons> [ms]   buttons held in the game (port 17493), e.g. press select, press cross+up 500
  python tools/vita.py shot [out.png]         screenshot: the game's own (port 17493) while it runs, else the system one
                                              (PS+START through vitacompanion; downloaded, then deleted on the Vita)

The Vita's IP comes from --ip or the VITA_IP environment variable. vitacompanion: FTP on 1337, commands on 1338.
The first install is the VPK (build/mgs_mobile.vpk) through VitaShell; deploy only replaces eboot.bin.
"""
import argparse
import ftplib
import os
import socket
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "mgs_native", "vita", "build")
TITLE_ID = "MGSM00001"
FTP_PORT, CMD_PORT, LOG_PORT, SHOT_PORT = 1337, 1338, 17492, 17493


def command(ip, text):
    with socket.create_connection((ip, CMD_PORT), timeout=5) as s:
        s.sendall((text + "\n").encode())
        time.sleep(0.2)


def ftp(ip):
    f = ftplib.FTP()
    f.connect(ip, FTP_PORT, timeout=10)
    f.login()
    return f


def remote(path):
    # vitacompanion's FTP server roots at the device list: "ux0:data/x" -> "/ux0:/data/x"
    dev, _, rest = path.partition(":")
    return "/" + dev + ":/" + rest.lstrip("/")


def put(ip, local, dest):
    with ftp(ip) as f, open(local, "rb") as fh:
        f.storbinary("STOR " + remote(dest), fh)
    print(f"sent {local} -> {dest} ({os.path.getsize(local)} bytes)")


def deploy(ip, launch):
    eboot = os.path.join(BUILD, "eboot.bin")
    if not os.path.exists(eboot):
        sys.exit(f"no {eboot}: run mgs_native\\vita\\build.bat")
    command(ip, f"quit {TITLE_ID}")
    time.sleep(1)
    put(ip, eboot, f"ux0:app/{TITLE_ID}/eboot.bin")
    if launch:
        command(ip, "screen on")   # (wakes a screen that went dark while idle)
        command(ip, f"launch {TITLE_ID}")
        print(f"launched {TITLE_ID}")


def screenshots(f):
    # Every file under ux0:picture/SCREENSHOT (one folder per app), as remote paths.
    base = remote("ux0:picture/SCREENSHOT")
    try:
        f.cwd(base)
    except ftplib.error_perm:
        return set()
    found = set()
    for d in [line.split()[-1] for line in _list(f) if line.startswith("d")]:
        f.cwd(base + "/" + d)
        found |= {base + "/" + d + "/" + line.split()[-1] for line in _list(f) if line.startswith("-")}
        f.cwd(base)
    return found


def _list(f):
    lines = []
    f.retrlines("LIST", lines.append)
    return lines


def shot(ip, out):
    # The game's own capture first (port 17493: the next frame, nothing pressed); the system screenshot (PS+START,
    # which the running app also sees as START) only when the game isn't running.
    try:
        s = socket.create_connection((ip, SHOT_PORT), timeout=3)
    except OSError:
        s = None   # the game isn't running
    reply = None
    if s:
        # Running but busy (a long load, the installer: no frame presented yet): wait, and never fall back to
        # PS+START then (the PS press makes the system suspend it mid-work, which ended the app once).
        with s:
            s.sendall(b"shot\n")
            s.settimeout(60)
            try:
                reply = s.recv(512).decode(errors="replace").strip()
            except OSError:
                sys.exit("the game is running but presented no frame for 60 s (busy or hung): no screenshot")
    if reply is not None:
        if not reply.startswith("ok "):
            sys.exit(f"game: {reply}")
        with ftp(ip) as f, open(out, "wb") as fh:
            f.retrbinary("RETR " + remote(reply[3:]), fh.write)
        print(f"saved {out} (from the game)")
        return
    with ftp(ip) as f:
        before = screenshots(f)
    command(ip, "press ps; press start; wait 300ms; release all")
    for _ in range(20):
        time.sleep(0.5)
        with ftp(ip) as f:
            new = sorted(screenshots(f) - before)
            if new:
                time.sleep(0.5)   # let the PNG finish writing
                with open(out, "wb") as fh:
                    f.retrbinary("RETR " + new[-1], fh.write)
                for path in new:
                    f.delete(path)
                print(f"saved {out}")
                return
    sys.exit("no new screenshot (screenshots disabled in this app, or the shell is busy)")


def press(ip, buttons, ms):
    # Through the game itself: vitacompanion's simulated buttons often never reached the app.
    with socket.create_connection((ip, SHOT_PORT), timeout=5) as s:
        s.sendall(f"press {buttons} {ms}\n".encode())
        reply = s.recv(256).decode(errors="replace").strip()
    if reply != "ok":
        sys.exit(f"game: {reply}")


def log(ip, out):
    # One client at a time: the game replays its backlog to each new connection, so the file restarts each time.
    while True:
        try:
            with socket.create_connection((ip, LOG_PORT), timeout=5) as s, open(out, "wb") as fh:
                s.settimeout(None)
                print(f"[connected to {ip}:{LOG_PORT}, saving to {out}]", flush=True)
                while data := s.recv(4096):
                    fh.write(data)
                    fh.flush()
                    sys.stdout.write(data.decode("utf-8", "replace"))
                    sys.stdout.flush()
            print("[disconnected]", flush=True)
        except OSError:
            pass
        time.sleep(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ip", default=os.environ.get("VITA_IP"))
    sub = ap.add_subparsers(dest="op", required=True)
    d = sub.add_parser("deploy")
    d.add_argument("--no-launch", action="store_true")
    lg = sub.add_parser("log")
    lg.add_argument("out", nargs="?", default=os.path.join(BUILD, "vita_log.txt"))
    c = sub.add_parser("cmd")
    c.add_argument("text", nargs="+")
    p = sub.add_parser("put")
    p.add_argument("local")
    p.add_argument("dest")
    g = sub.add_parser("get")
    g.add_argument("src")
    g.add_argument("local")
    ls = sub.add_parser("ls")
    ls.add_argument("dir")
    pr = sub.add_parser("press")
    pr.add_argument("buttons")
    pr.add_argument("ms", nargs="?", type=int, default=200)
    sh = sub.add_parser("shot")
    sh.add_argument("out", nargs="?", default=os.path.join(BUILD, "vita_shot.png"))
    a = ap.parse_args()
    if not a.ip:
        sys.exit("give the Vita's IP address: --ip <address> or the VITA_IP environment variable")

    if a.op == "deploy":
        deploy(a.ip, not a.no_launch)
    elif a.op == "log":
        log(a.ip, a.out)
    elif a.op == "cmd":
        command(a.ip, " ".join(a.text))
    elif a.op == "put":
        put(a.ip, a.local, a.dest)
    elif a.op == "get":
        with ftp(a.ip) as f, open(a.local, "wb") as fh:
            f.retrbinary("RETR " + remote(a.src), fh.write)
    elif a.op == "press":
        press(a.ip, a.buttons, a.ms)
    elif a.op == "shot":
        shot(a.ip, a.out)
    elif a.op == "ls":
        with ftp(a.ip) as f:
            f.cwd(remote(a.dir))  # this server ignores LIST's argument
            f.retrlines("LIST")


if __name__ == "__main__":
    main()
