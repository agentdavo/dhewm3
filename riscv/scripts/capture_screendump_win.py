#!/usr/bin/env python3
"""Windows visual-capture helper for the bare-metal dhewm3 runs.

run-win.sh exposes the QEMU HMP monitor on tcp:127.0.0.1:<MONITOR_PORT> (set
MONITOR_PORT and VIDEO=gpu). This connects to it, issues `screendump <ppm>`
against the live virtio-gpu scanout (a STOPPED scanout screendumps all-zero, so
the guest must be running), waits for the PPM to settle, and converts it to PNG
so the image tools can view it. The Linux path (capture_gameplay_mp4.sh) uses a
unix socket + nc, which does not exist on Windows; this is the TCP equivalent.

  python capture_screendump_win.py --port 4455 --out shot.png [--ppm shot.ppm]

Pure stdlib (socket + zlib + struct): no PIL, no nc, no ImageMagick.
"""
import argparse, os, socket, struct, sys, time, zlib


def send_screendump(port, ppm_path, settle=2.5, connect_timeout=10.0):
    # HMP greets with a banner ending in "(qemu) "; drain it, send the command,
    # give QEMU a moment to write the file, then confirm it stopped growing.
    deadline = time.time() + connect_timeout
    last_err = None
    while time.time() < deadline:
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=5.0)
            break
        except OSError as e:
            last_err = e
            time.sleep(0.3)
    else:
        raise SystemExit(f"could not reach monitor on 127.0.0.1:{port}: {last_err}")
    with s:
        s.settimeout(2.0)
        try:
            s.recv(4096)  # banner
        except OSError:
            pass
        # forward slashes are fine for QEMU on Windows; avoid backslash escaping
        cmd = "screendump %s\n" % ppm_path.replace("\\", "/")
        s.sendall(cmd.encode())
        time.sleep(settle)
        try:
            s.sendall(b"\n")
            s.recv(4096)
        except OSError:
            pass
    # wait for the PPM to exist and stop growing (QEMU writes it asynchronously)
    stable, last = 0, -1
    for _ in range(40):
        if os.path.exists(ppm_path):
            sz = os.path.getsize(ppm_path)
            stable = stable + 1 if sz == last and sz > 0 else 0
            last = sz
            if stable >= 3:
                return
        time.sleep(0.25)
    if not os.path.exists(ppm_path) or os.path.getsize(ppm_path) == 0:
        raise SystemExit(f"screendump produced no data at {ppm_path}")


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise SystemExit(f"{path}: not a binary PPM (P6)")
    # parse header: P6 <w> <h> <maxval>, whitespace/comment separated
    idx, fields = 2, []
    while len(fields) < 3:
        while idx < len(data) and data[idx:idx+1].isspace():
            idx += 1
        if data[idx:idx+1] == b"#":
            while idx < len(data) and data[idx:idx+1] != b"\n":
                idx += 1
            continue
        start = idx
        while idx < len(data) and not data[idx:idx+1].isspace():
            idx += 1
        fields.append(int(data[start:idx]))
    w, h, maxv = fields
    idx += 1  # single whitespace after maxval
    px = data[idx:idx + w * h * 3]
    if len(px) < w * h * 3:
        raise SystemExit(f"{path}: truncated pixel data")
    return w, h, px


def write_png(path, w, h, rgb):
    def chunk(tag, payload):
        c = tag + payload
        return struct.pack(">I", len(payload)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # filter type 0
        raw.extend(rgb[y * w * 3:(y + 1) * w * 3])
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=int(os.environ.get("MONITOR_PORT", "4455")))
    ap.add_argument("--out", required=True, help="output PNG path")
    ap.add_argument("--ppm", default=None, help="intermediate PPM (default: <out>.ppm)")
    ap.add_argument("--settle", type=float, default=2.5)
    ap.add_argument("--keep-ppm", action="store_true")
    a = ap.parse_args()
    ppm = a.ppm or (os.path.splitext(a.out)[0] + ".ppm")
    send_screendump(a.port, ppm, settle=a.settle)
    w, h, rgb = read_ppm(ppm)
    write_png(a.out, w, h, rgb)
    if not a.keep_ppm and not a.ppm:
        try:
            os.remove(ppm)
        except OSError:
            pass
    print(f"captured {w}x{h} -> {a.out}")


if __name__ == "__main__":
    main()
