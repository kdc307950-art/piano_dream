#!/usr/bin/env python3
"""Generate and validate the CI screenshot matrix."""
import argparse, struct, subprocess
from pathlib import Path

SIZES = ((1280, 880), (1920, 1080), (2560, 1440))

def png_size(path: Path):
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n": raise ValueError("not a PNG")
    return struct.unpack(">II", data[16:24])

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default="./piano")
    ap.add_argument("--out", default="artifacts/screenshots")
    args = ap.parse_args()
    out = Path(args.out); out.mkdir(parents=True, exist_ok=True)
    for width, height in SIZES:
        path = out / f"ui-{width}x{height}.png"
        subprocess.run([args.exe, "--screenshot", str(path), "--size", f"{width}x{height}", "--frames", "5"], check=True, timeout=30)
        if path.stat().st_size < 1024 or png_size(path) != (width, height):
            raise SystemExit(f"invalid screenshot: {path}")
        print(f"{path}: {width}x{height}, {path.stat().st_size} bytes")

if __name__ == "__main__": main()
