# Creeper — Encrypted Steganography Suite

**English** | [简体中文](README.md)

> Encrypt files with **AES-256-GCM** and embed them losslessly into PNG / MP3 / WAV carriers that show no visible anomalies. The GUI masquerades as a "format converter". Security depends solely on the password (Kerckhoffs's principle).

## Features

- **End-to-end encryption**: AES-256-GCM + PBKDF2-HMAC-SHA256 (600,000 iterations); payload presence is decided by GCM authentication, false-positive rate 2⁻¹²⁸
- **Three carriers, zero damage to media data**:
  - PNG: ±1 on RGB channels (LSB matching) + histogram pairing compensation + self-adversarial re-embedding
  - WAV: lossless rewrite of the low 1–3 bits of PCM samples (configurable depth, up to 3× capacity)
  - MP3: 3 auxiliary bits per MPEG frame header; audio data and ID3 regions untouched byte-for-byte
- **No magic bytes**: headers carry no fixed signature; the scatter seed is derived from the password; presence = GCM authentication — a wrong password and no payload are indistinguishable
- **Anti-statistical-detection**: default 15% fill-rate cap (overridable with `--cap`)
- **Two frontends**: CLI (`creeper_cli`) + disguised GUI (`creeper_img` / `creeper_audio`, with hidden feature entry)
- **Self-verification**: the extraction sequence is replayed bit-by-bit before writing; embedded files are read back and verified

## Security Model

| Item | Design |
|---|---|
| Encryption | AES-256-GCM (pure software, no external deps) |
| Key derivation | PBKDF2-HMAC-SHA256, 600,000 iterations |
| Payload presence | GCM tag authentication (wrong password / no payload → same result) |
| Scattering | Password-derived xorshift64 sequence + linear probing, content-independent |
| Statistical exposure | 15% default fill cap + histogram compensation / low-bit uniformization |
| Interop | Byte-exact mutual decoding with the Python reference implementation (`tests/envelope.py`) |

## Carrier Capacity

| Carrier | Embedding | 100% capacity | Default 15% cap |
|---|---|---|---|
| PNG (2560×1600) | 1 bit per RGB channel, ±1 | ≈ 1.46 MB | ≈ 230 KB |
| WAV (44.1kHz stereo, per minute) | low 1 bit | ≈ 661 KB | ≈ 99 KB |
| WAV `--depth 2` | low 2 bits | ≈ 1.3 MB | ≈ 198 KB |
| WAV `--depth 3` | low 3 bits | ≈ 1.98 MB | ≈ 297 KB |
| MP3 (19,194 frames) | 3 header auxiliary bits/frame | ≈ 7.2 KB | — |

## Build

Requires [w64devkit](https://github.com/skeeto/w64devkit) (`g++` on PATH):

```bat
src\build.bat
```

Produces three executables into `res/` (release package, alongside md copies of the guides): `creeper_cli.exe` (console) and `creeper_img.exe` / `creeper_audio.exe` (GUI, `-mwindows`). Third-party deps `imgui/` and `stb/` ship in `third_party/`.

## Tests

```bat
tests\run_all.bat        :: full regression, all 9 suites, non-zero exit on failure
python tests\test_crypto.py   :: encryption round-trips + robustness
python tests\test_png.py      :: PNG stego round-trips
python tests\test_mp3.py      :: MP3 stego round-trips
python tests\test_wav.py      :: WAV stego round-trips (incl. v1.0 legacy-format compat)
python tests\test_cross.py    :: C++ ↔ Python envelope interop (critical)
```

Requires Python 3 + Pillow + numpy + cryptography. Binary test hosts (`assets/img.png` / `assets/msc.mp3` / `assets/test.wav`) are generated deterministically by `tests/gen_hosts.py` (each suite regenerates its host automatically when missing).

## Quick Start (CLI)

```bat
:: encrypt
creeper_cli seal secret.pdf secret.env your-password

:: decrypt
creeper_cli open secret.env out-dir your-password

:: embed (hide into carrier)
creeper_cli embed carrier.png secret.env output.png your-password

:: extract
creeper_cli extract output.png out-dir your-password

:: detect (1=present, 0=absent)
creeper_cli has output.png your-password

:: split a large file across multiple carriers (sealed first, then sharded;
:: a single shard alone is undecryptable)
creeper_cli split big.bin your-password out-dir carrier1.png carrier2.wav carrier3.mp3

:: reassemble from multiple carriers (order-independent, joined by shard id)
creeper_cli unsplit out-dir your-password carrier3.mp3 carrier1.png carrier2.wav
```

Options: `--cap N` (fill-rate cap 0–100, default 15, PNG/WAV); `--depth 1|2|3` (WAV embedding depth, up to 3× capacity).

## GUI (Disguised Mode)

`creeper_img.exe` ("image format conversion") and `creeper_audio.exe` ("audio format conversion") present as ordinary format converters; real features are reachable via a hidden entry (`Ctrl+Shift+F`).

- **Single file**: with a password, extraction is attempted, silently falling back to fake conversion on failure; without a password, fake conversion.
- **2 files (1 carrier + 1 payload)**: always embed, regardless of the "hardware acceleration" checkbox.
- **3+ files** (dispatched by the "hardware acceleration" checkbox):
  - checked = embed (last file is the payload, the rest are carriers, auto-sharded across them; use the "move up/down" buttons to put the payload last)
  - unchecked = extract (all files are carriers, joined out of order)
- **No password** (multi-file): always fakes a batch conversion, revealing nothing.
- **Hidden window**: also offers "encoding quality" (fill-rate cap) and "bit depth" (WAV depth 1/2/3).

## Layout

```
├─ src/               source (C++17, system libs only)
│  └─ build.bat       build script (w64devkit g++; outputs exes to res/)
├─ third_party/       imgui / stb (open-source deps, do not modify)
├─ docs/              specs, test report, product docs (markdown)
├─ res/               release package (gitignored: exes + md copies of the guides)
├─ assets/            test hosts (gitignored; generated by tests/gen_hosts.py)
├─ tests/             test suites (Python / PowerShell)
├─ .github/           GitHub Actions CI (build + non-GUI suites)
├─ README.md / README.en.md / LICENSE / THIRD_PARTY_LICENSES.md / AGENTS.md
```

## Documentation

- [User Guide](docs/使用说明书.en.md) / [使用说明书](docs/使用说明书.md) (product manual)
- [Technical Report](docs/技术报告.en.md) / [技术报告](docs/技术报告.md) (design & anti-detection rationale)
- [Encoder Spec](docs/PROMPT_ENCODER.en.md) / [编码任务书](docs/PROMPT_ENCODER.md) · [Tester Spec](docs/PROMPT_TESTER.en.md) / [测试任务书](docs/PROMPT_TESTER.md)
- [Test Report](docs/TEST_REPORT.en.md) / [测试报告](docs/TEST_REPORT.md) (defects & verification records)

## License

[BSD 3-Clause](LICENSE). For lawful purposes only (privacy protection, data backup, media metadata hiding, etc.); comply with the laws of your jurisdiction.

This software includes third-party open-source components: Dear ImGui (MIT) and stb_image / stb_image_write (MIT / Public Domain dual-licensed); see [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

Copyright © 2026, Creeper Project Authors. All rights reserved.