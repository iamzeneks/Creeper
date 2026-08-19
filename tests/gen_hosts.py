# -*- coding: utf-8 -*-
"""Generate all binary test hosts under assets/ deterministically.

    python tests/gen_hosts.py

Produces:
  assets/img.png   - 2048x2048 RGB gradient (PNG stego host)
  assets/msc.mp3   - synthetic MPEG1 Layer3 128kbps 44.1kHz stereo stream (MP3 stego host)
  assets/test.wav  - 44.1kHz/16bit/stereo 60s sine (WAV stego host, via gen_wav)

Any of the three may already exist; existing files are left untouched.
"""
import os
import struct
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
ASSETS = os.path.join(ROOT, "assets")


def gen_img():
    out = os.path.join(ASSETS, "img.png")
    if os.path.exists(out):
        print("img.png exists, skip")
        return True
    try:
        import numpy as np
        from PIL import Image
    except ImportError:
        print("ERROR: pillow/numpy required to generate img.png", file=sys.stderr)
        return False
    h = w = 2048
    y = np.linspace(0, 255, h, dtype=np.uint8).reshape(h, 1)
    x = np.linspace(0, 255, w, dtype=np.uint8).reshape(1, w)
    r = np.broadcast_to(y, (h, w)).astype(np.uint8)
    g = np.broadcast_to(x, (h, w)).astype(np.uint8)
    b = ((np.broadcast_to(y, (h, w)).astype(np.int16)
          + np.broadcast_to(x, (h, w)).astype(np.int16)) // 2).astype(np.uint8)
    img = np.stack([r, g, b], axis=2)
    Image.fromarray(img, "RGB").save(out, optimize=True)
    print("written %s" % out)
    return True


def gen_mp3():
    out = os.path.join(ASSETS, "msc.mp3")
    if os.path.exists(out):
        print("msc.mp3 exists, skip")
        return True
    # MPEG1 Layer3, 128kbps, 44.1kHz, stereo (no CRC, no padding):
    # frame header 0xFF FB 90 00, frame length = 144*128000/44100 = 417 bytes.
    header = bytes([0xFF, 0xFB, 0x90, 0x00])
    side = 32  # MPEG1 Layer3 stereo side-info size
    n_frames = 21000
    body = bytes((i * 7 + 3) & 0xFF for i in range(417 - 4 - side))
    frame = header + b"\x00" * side + body
    with open(out, "wb") as f:
        for _ in range(n_frames):
            f.write(frame)
    print("written %s (%d frames, %d bytes)" % (out, n_frames, n_frames * len(frame)))
    return True


def gen_wav():
    out = os.path.join(ASSETS, "test.wav")
    if os.path.exists(out):
        print("test.wav exists, skip")
        return True
    import subprocess
    gen = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gen_wav.py")
    return subprocess.call([sys.executable, gen]) == 0


def main():
    os.makedirs(ASSETS, exist_ok=True)
    ok = gen_img() and gen_mp3() and gen_wav()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())