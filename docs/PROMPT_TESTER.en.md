# PROMPT_TESTER.md — Algorithm Testing Agent Task Brief

**English** | [简体中文](PROMPT_TESTER.md)

> You are the **testing agent**. Task: perform comprehensive verification of the built C++ steganography tool in the repository root.
> Prerequisite: the encoding agent has finished, and `creeper_cli.exe`, `creeper_img.exe`, `creeper_audio.exe` have been built in the project root. If the exes do not exist, first report "构建缺失" (build missing) — do not modify the code yourself.

## 0. Environment and Resources

- Project root: repository root directory
- CLI usage (see PROMPT_ENCODER.md §5):
  `creeper_cli seal <in> <out> <password>` / `open <env> <out> <password>` / `embed <host> <payload> <out> <password> [--cap N] [--depth N]` / `extract <host> <outdir> <password>` / `has <host> <password>` (outputs 1/0, wrong password or no payload both yield 0) / `split <payload> <password> <outdir> <host...> [--cap N] [--depth N]` / `unsplit <outdir> <password> <host...>`
- Carriers: `img.png` (8.8MB), `msc.mp3` (20MB); payload material: `src.png` (235MB)
- Python 3.14.4 + Pillow 12.2.0 + cryptography 49.0.0 + numpy (available)
- Reference envelope implementation (byte-level compatibility baseline): `tests/envelope.py` (local copy; `seal(payload, password)` / `open_seal(envelope, password)`)

## 1. Test Matrix (all must be run; results recorded in TEST_REPORT.md)

### A. Encryption round-trip (CLI seal/open)
- Random data files: 1B, 1KB, 1MB, 10MB (generated via `os.urandom`); passwords: plain ASCII + Chinese password + empty password (if the CLI accepts an empty password)
- Assertions: the `open` output matches the input `hashlib.sha256`
- Negative cases: wrong password → non-zero exit code and no output file; tampering any 1 bit of the envelope → non-zero exit

### B. Cross-language cross-validation (critical! verifies C++ and Python envelope byte-level compatibility)
- C++ `seal` output → Python `open_seal` must succeed and restore the original data
- Python `seal` output → C++ `open` must succeed and restore the original data
- Same input + same password, the two envelope headers (magic/version/salt/nonce layout) must be the same length (41-byte header + ciphertext)
- Note: salt/nonce are random, so different ciphertexts on the two sides are normal; only verify **mutual decryption**, not ciphertext equality

### C. PNG steganography round-trip
- Read `img.png` with PIL, report dimensions and theoretical capacity (w×h×3÷8 bytes)
- Construct small payloads (1KB, within 15% capacity such as 150KB/200KB) → `embed img.png payload steg.png password` → `has steg.png password`==1 → `extract` → byte-identical
- **Fill-rate cap (default 15%)**: construct a ~16%-of-capacity payload (e.g. 250KB) → embed reports `payload too large: exceeds 15%...` with non-zero exit and no output file; same payload with `--cap 100` → successful round-trip. Also test `--cap` boundary values 0/100
- **Carrier losslessness verification**: PIL can open steg.png; compare per-pixel with the original (numpy), single-channel max absolute difference must be ≤ 1 (±1 embedding), alpha unchanged; assert the mean absolute difference only for small-fill-rate payloads (at high fill rate the mean diff ≈0.5×fill rate >1/255 is normal)
- **Histogram anti-detection verification** (statistical signature after ±1 embedding + pairing compensation):
  - The RGB aggregate histogram L1 distance between original and steg.png must be ≤ modified-channel pixel count × 0.1 (after pairing compensation it should be far below this threshold, typical ratio ~0.01)
  - The count of equal adjacent bins (H'[v]==H'[v+1], the classic trace of forced-LSB embedding) must be ≤ modified-channel pixel count × 0.01
- **Capacity overflow**: construct a payload exceeding the absolute capacity (e.g. 1.6MB) → embed must error with non-zero exit; binary-search the max embeddable byte count under the 15% cap (expected ≈ capacity ×15% × ~0.95, due to envelope expansion and header overhead)
- Try embedding `src.png` into `img.png` once, record the result (expected: error exceeding the 15% cap — record truthfully, this is manual material)
- Chinese filename payload: `测试载荷.bin` → filename restored after extract
- **No-magic verification**: after embedding, no fixed magic-byte signature may exist in the carrier file; `has` on a clean carrier →0 / after embedding + correct password →1 / after embedding + wrong password →0 / random binary renamed .png →0
- **Password-scattering verification (NO-MAGIC-2)**: same payload, same carrier, different passwords → the differing-bit distribution between two embed outputs of steg.png must be completely different (scatter positions change with password, no fixed positions); the stego header has no seed field (total bitstream length = 2 + name_len + 4 + env_len bytes; the position where a 16-bit length can no longer be read out from the scatter is no longer fixed)

### D. MP3 steganography round-trip
- `embed msc.mp3 payload steg.mp3 password` (payload ≤ capacity, ~6KB) → `has steg.mp3 password`==1 → `extract` → byte-identical
- **Carrier zero-intrusion verification** (manual MPEG frame parsing in Python):
  - File size completely unchanged
  - ID3v2 tag region (before the audio start) byte-for-byte identical
  - Byte-for-byte full-file comparison: the difference mask can only be one of {0x01, 0x04, 0x08, 0x0C} (private/copyright/original and their combinations), **no other mask allowed**
  - Frame data region (after each frame's 4-byte header to the next frame) byte-for-byte zero-change
  - The first 8 frames' auxiliary bits concatenated into 24 bits **must not** equal the old magic 0x435250 (no fixed signature); **and must vary with the password** (frame-header auxiliary bits XORed wholesale with a password-derived keystream, NO-MAGIC-2) — same carrier, same payload, different passwords → the first 8 frames' auxiliary bits differ
- Also test the case where the original msc.mp3 has no ID3v2: construct an untagged MP3 (copy msc.mp3 and strip the ID3 header) → embed → extract round-trip → frame data region zero-change
- Chinese filename payload likewise tested
- **Capacity overflow**: payload exceeding frames×3-bit capacity → embed must error with non-zero exit and produce no output file
- Decoding losslessness: steg.mp3 and msc.mp3 produce identical output under the same decoder (frame data untouched, semantically guaranteed)

### E. Detection function (has)
- Clean carrier (with password) → 0; after embedding + correct password → 1; after embedding + wrong password → 0 (no magic, authentication-based determination); random binary renamed .png/.mp3/.wav → 0 (should not false-positive)
- Missing password argument (`has <host>`) → non-zero exit (usage)

### W. WAV steganography round-trip (PCM lossless LSB, large-capacity carrier)
- Carrier `test.wav` (44.1kHz/16-bit/stereo 60s, theoretical capacity = samples/8 ≈ 661.5KB; 15% cap ≈ 99KB)
- Small payloads (1KB / 30KB / 45KB≈13.6%) → `embed test.wav payload steg.wav password` → `has steg.wav password`==1 → `extract` → byte-identical
- **Carrier zero-intrusion verification** (Python RIFF/fmt/data parsing):
  - File size completely unchanged; region before the data chunk (RIFF/fmt region) byte-for-byte identical
  - Per-sample max absolute difference ≤ 1 (±1 embedding); modified sample count ≈ payload bit-flip count
- **depth (carrier depth) 1/2/3** (`--depth 1|2|3`):
  - depth=2 round-trip: per-sample difference ≤3, capacity ×2 (15% cap 2× baseline); depth=3 round-trip: per-sample difference ≤7, capacity ×3
  - 8-bit carrier with depth=2/3 → error (16-bit only); non-WAV carrier with `--depth` → error; `--depth 4` → error
  - v1.0 legacy format (no depth field) fallback: new code can decode old files (new-format parse failure falls back automatically)
- **Fill-rate cap (default 15%)**: over-cap payload → report `payload too large: exceeds 15%...` with no output; same payload with `--cap 100` → successful round-trip; absolute capacity overflow (> samples×depth) → error
- Chinese filename payload round-trip; random binary renamed .wav → has 0
- Stability: 5 consecutive embed/extract cycles
- Construct an 8-bit WAV carrier (optional) to verify the bit-depth branch; a float WAV (optional) to verify `unsupported wav format`

### S. Large-file sharding across multiple carriers (split/unsplit)
- **Mixed-carrier round-trip**: `split` a payload larger than a single carrier's capacity across multiple carriers (PNG+WAV, PNG+WAV+MP3 mixed) → `unsplit` with out-of-order input → byte-identical to the original file
- **Order independence**: out-of-order input to `unsplit` → identical restore
- **Missing block**: omit one block → `unsplit` reports `missing split blocks (need N, have M)` with non-zero exit
- **Wrong password** → `no payload found`; **single-block extract/has** → no payload (a single block cannot pass GCM authentication); **non-shard plain carrier mixed in** → skipped or `no payload found`
- **Insufficient capacity**: payload exceeding the combined multi-carrier total → `split` reports `payload too large for combined host capacity` with non-zero exit and no output
- **depth=2 + --cap 30 sharding**: WAV high-capacity sharding round-trip
- **Empty-password split/unsplit** → successful round-trip

### F. GUI smoke test (PowerShell)
- Launch `creeper_img.exe` → wait 3 seconds → `Get-Process creeper_img` exists → kill the process; `creeper_audio.exe` likewise
- Record whether the window appears normally (if screenshots are impossible, record process survival)
- About page/self-destruct: can be verified automatically (PowerShell P/Invoke + `EnumWindows` to find class `XhAboutDlg`, note FindWindowW only finds top-level windows and `GetClassNameW` needs `CharSet=Unicode`): dialog opens → button `BM_CLICK` (use **`PostMessage` async, `SendMessage` will block on the MessageBox modal) → `#32770` confirm box appears → click "否" (No) process survives / click "是" (Yes) process exits and the exe is deleted (verified on a copy); the full chain has been truly verified this way, see TEST_REPORT §12
- **GUI dedicated scripts**: `test_gui_about.ps1` (About dialog real mouse-click chain + copy self-destruct); `test_gui_quality.ps1` (Ctrl+Shift+F opens the hidden window → Windows OCR verifies the "编码质量" ("encoding quality") dropdown exists); both scripts require `SetWindowPos(HWND_TOPMOST)` + window-center click activation to bypass foreground lockout (otherwise occlusion causes occasional failures)
- **Binary spot check**: the GUI exe must not contain plaintext `CREEPER1`/`CRPR`/`CRP`/`creeper`; the CLI exe may only contain `creeper_cli` in the usage help text and must not contain `CREEPER1`/`CRPR`/`CRP` or source filenames such as `creeper_cli.cpp`

### G. Robustness
- Missing arguments, nonexistent files, empty-file carrier, empty-file payload → all must error cleanly (non-zero exit, no crash, no garbage output written)

## 2. Deliverables (written to the project root)

- `tests/test_crypto.py`, `tests/test_png.py`, `tests/test_mp3.py`, `tests/test_wav.py`, `tests/test_split.py`, `tests/test_cross.py` (Python, directly re-runnable)
- `tests/test_gui.ps1`, `tests/test_gui_about.ps1`, `tests/test_gui_quality.ps1` (PowerShell)
- `tests/run_all.bat` (one-key serial full regression, any of the 9 suites failing exits non-zero)
- Test infrastructure: `tests/tester.py` (common utilities, ROOT already relative), `tests/envelope.py` (envelope reference implementation local copy, test_cross prefers local), `tests/gen_hosts.py` / `tests/gen_wav.py` (carrier auto-generation)
- `TEST_REPORT.md`: **table** recording every case → pass/fail/note; failed cases include reproduction commands and symptoms

## 3. Constraints

- **Forbidden to modify any production code** (crypto/png_steg/mp3_steg/common_ui/*_app.cpp, stb, imgui, build.bat); on finding a bug only record reproduction steps
- Temporary test files go in `tests/tmp/`, clean up large files after testing (keep the report)
- Use `fc /b` or Python byte comparison; note the method in the report
- Report in Chinese