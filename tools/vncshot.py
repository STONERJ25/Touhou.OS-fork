#!/usr/bin/env python3
"""Takes a picture of a TouhouOS desktop through vncd (docs/VNC.md) and saves it as a PNG: Hatate's camera.

  python tools/vncshot.py --tunnel 192.168.1.185 --out build/dell.png        ssh tunnel to the machine's vncd (bounded), one picture
  python tools/vncshot.py --port 5900 --out build/shot.png                 a vncd already reachable on localhost:5900
  VNC_PASSWORD=secret python tools/vncshot.py ...                          when vncd has a password (`sudo vncd -passwd`)
  python tools/vncshot.py --tunnel 192.168.1.185 --keep                       no picture: keep the tunnel open on localhost:5900 until Ctrl+C, for a viewer of your own
                                                                           (TigerVNC, RealVNC, Remmina: "localhost:5900")

With --tunnel the script starts `ssh -N -L PORT:127.0.0.1:5900 marisa@HOST` with the dev key and the pinned host key
(assets/local/dell_known_hosts), uses it for the one picture and stops it again, so nothing stays open. The viewer is
tests/rfbclient.py, the strict client of the vncd tests (pure Python: the first full 2560x1440 frame takes a few seconds).
"""
import argparse
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tests"))
from rfbclient import RFBClient  # noqa: E402


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def open_tunnel(host, user, key, known_hosts, remote_port, local_port=0):
    port = local_port or free_port()
    cmd = ["ssh", "-N", "-i", str(Path(key).expanduser()), "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes",
           "-o", f"UserKnownHostsFile={known_hosts}", "-o", "ExitOnForwardFailure=yes", "-o", "ConnectTimeout=10", "-q",
           "-L", f"{port}:127.0.0.1:{remote_port}", f"{user}@{host}"]
    proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    deadline = time.time() + 20
    while time.time() < deadline:
        if proc.poll() is not None:
            raise SystemExit("vncshot: the ssh tunnel ended: " + (proc.stderr.read().decode(errors="replace").strip() or f"exit {proc.returncode}"))
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
            return proc, port
        except OSError:
            time.sleep(0.3)
    proc.kill()
    raise SystemExit("vncshot: the ssh tunnel did not come up in 20 s")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tunnel", metavar="HOST", help="start an ssh tunnel to the vncd of this machine for the picture")
    ap.add_argument("--user", default="marisa")
    ap.add_argument("--key", default="~/.ssh/touhou_ed25519")
    ap.add_argument("--known-hosts", default=str(ROOT / "assets" / "local" / "dell_known_hosts"))
    ap.add_argument("--remote-port", type=int, default=5900, help="vncd's port on the machine (vnc.port=)")
    ap.add_argument("--port", type=int, default=5900, help="the local port of a vncd (without --tunnel)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--out", default="build/vncshot.png")
    ap.add_argument("--keep", action="store_true", help="with --tunnel: take no picture, keep the tunnel open until Ctrl+C (a viewer connects to localhost:--port)")
    ap.add_argument("--timeout", type=int, default=60)
    args = ap.parse_args()

    proc = None
    host, port = args.host, args.port
    if args.tunnel:
        proc, port = open_tunnel(args.tunnel, args.user, args.key, args.known_hosts, args.remote_port, args.port if args.keep else 0)
        host = "127.0.0.1"
        if args.keep:
            print(f"vncshot: the tunnel to {args.tunnel} is open: point a VNC viewer at localhost:{port} (Ctrl+C here closes it)", flush=True)
            try:
                while proc.poll() is None:
                    time.sleep(1)
                print("vncshot: the ssh tunnel ended: " + (proc.stderr.read().decode(errors="replace").strip() or f"exit {proc.returncode}"))
            except KeyboardInterrupt:
                pass
            finally:
                if proc.poll() is None:
                    proc.terminate()
            return
    try:
        t0 = time.time()
        c = RFBClient(host, port, password=os.environ.get("VNC_PASSWORD"), timeout=args.timeout)
        print(f"vncshot: connected, desktop '{c.name}' {c.width}x{c.height}, pixel format {c.pf}", flush=True)
        c.set_encodings(["zrle", "copyrect", "cursor", "desktopsize"])
        c.update(incremental=False, timeout=args.timeout)
        t1 = time.time()
        from PIL import Image
        img = Image.frombytes("RGB", (c.width, c.height), c.rgb_bytes())
        out = Path(args.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        img.save(out)
        print(f"vncshot: {out} {c.width}x{c.height}, first frame in {t1 - t0:.1f} s, {c.bytes_in} bytes received, rectangles: "
              + ", ".join(f"{n}" for n in sorted({r[4] for r in c.rect_log})), flush=True)
        c.close()
    finally:
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(5)
            except subprocess.TimeoutExpired:
                proc.kill()


if __name__ == "__main__":
    main()
