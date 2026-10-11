#!/usr/bin/env python3
"""Packs assets/png/icon-{16,24,32,48,64,128,256}.png into assets/app.ico (PNG-compressed frames).
Standard library only. Re-run after replacing the PNGs, then rebuild the exe."""
import struct
from pathlib import Path
a = Path(__file__).resolve().parent.parent / "assets"
sizes = [16, 24, 32, 48, 64, 128, 256]
frames = [(a / "png" / f"icon-{n}.png").read_bytes() for n in sizes]
out = struct.pack("<HHH", 0, 1, len(sizes))
off = 6 + 16 * len(sizes)
body = b""
for n, d in zip(sizes, frames):
    out += struct.pack("<BBBBHHII", n % 256, n % 256, 0, 0, 1, 32, len(d), off + len(body))
    body += d
(a / "app.ico").write_bytes(out + body)
print("wrote", a / "app.ico")
