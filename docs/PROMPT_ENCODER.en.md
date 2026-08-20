# Creeper — Encoding Agent Task Specification

**English** | [简体中文](PROMPT_ENCODER.md)

> You are the **encoding agent**. Task: implement a C++ steganography tool in the project root (disguised as format-conversion software).
> All specifications **must** be executed strictly according to this document; do not modify the file format on your own (the testing agent relies on these formats for cross-validation).

## 0. Working Directory and Existing Resources (already ready, do not touch)

- Project root: repository root (the directory containing `src/`, `tests/`, `docs/`, `res/`)
- `stb/stb_image.h`, `stb/stb_image_write.h` — already downloaded, **do not modify**
- `imgui/` (complete Dear ImGui 1.92+ master) + `imgui/backends/` (imgui_impl_win32 + imgui_impl_dx11) — already downloaded, **do not modify**
- Host test files: `img.png` (8.8MB), `msc.mp3` (20MB), `src.png` (235MB, payload source material)
- Compiler: `g++` (w64devkit 15.2.0), linkable libraries: `-lbcrypt -ld3d11 -ldxgi -lgdi32 -limm32 -luser32 -lshell32 -lole32 -luuid -lcomdlg32 -ldwmapi`
- Reference prototype: `tests/envelope.py` (Python version of the envelope format, **C++ must be byte-level compatible**, used for cross-validation)

## 1. Deliverables List

```
crypto.h / crypto.cpp      # BCrypt encryption envelope seal/open
png_steg.h / png_steg.cpp  # PNG LSB steganography
mp3_steg.h / mp3_steg.cpp  # MP3 ID3v2 GEOB steganography
common_ui.h / common_ui.cpp# shared GUI infrastructure (DX11 window, ImGui init, Chinese font, hotkeys, file dialog, UTF-8<->UTF-16 utilities)
img_app.cpp                # image-disguised converter -> creeper_img.exe
audio_app.cpp              # audio-disguised converter -> creeper_audio.exe
cli_main.cpp               # headless CLI (for testing) -> creeper_cli.exe (source filename avoids the word "creeper")
build.bat                  # one-click build of all three exes
```
(The manual will be written by Xiao Zan personally at the end; do not write it)

## 2. Encrypted Envelope Format (crypto.h/cpp) — must be byte-level compatible with envelope.py

```
offset  size  content
0       8     magic  "CREEPER1"
8       1     version 0x01
9       16    salt (random)
25      12    nonce (random)
37      4     ct_len (big-endian)
41      ct_len AES-256-GCM ciphertext (including 16B tag)
```

- KDF: **PBKDF2-HMAC-SHA256, 600,000 iterations**, deriving a 32-byte key
- Algorithm: **AES-256-GCM** (BCrypt: `BCRYPT_AES_ALGORITHM` + `BCRYPT_CHAINING_MODE_GCM`)
- Flow: seal = read file bytes -> PBKDF2 derive key -> AES-GCM encrypt (random salt/nonce) -> assemble envelope
- open = parse envelope -> PBKDF2 derive -> GCM decrypt (authentication failure = wrong password or tampering, throw exception)
- **String obfuscation**: the envelope header magic `"CREEPER1"` and envelope-related error messages exist in source code as XOR 0x55 byte arrays, restored by `xstr()` at runtime (each file keeps its own copy in an anonymous namespace); the exe must not contain `CREEPER1` plaintext (source filenames containing `creeper` are also not allowed — the CLI main file is called `cli_main.cpp`)
- Interface:
  ```cpp
  std::vector<uint8_t> crypto_seal(const std::vector<uint8_t>& payload, const std::string& password);
  std::vector<uint8_t> crypto_open(const std::vector<uint8_t>& envelope, const std::string& password); // throws std::runtime_error on failure
  ```
- BCrypt notes: GCM uses `BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO` (pbNonce=nonce, pbTag=16B); PBKDF2 uses `BCryptDeriveKeyPBKDF2` (SHA256 algorithm handle); `BCryptSetProperty(BCRYPT_CHAINING_MODE, BCRYPT_CHAINING_MODE_GCM)`.

## 3. PNG Steganography (png_steg.h/cpp)

- Decode with stb_image (`stbi_load`, force RGB); **embed only 1 bit in each of the three RGB channels** (keep alpha as-is, if present); write back with stb_image_write (`stbi_write_png`, lossless)
- Bit-stream order: pixel row-major order, per-pixel R->G->B
- **Embedding method = ±1 (LSB matching, not forced LSB)**: target bit equals current LSB of the pixel -> no change; differs -> pixel value ±1 (random direction 50/50; v=0 can only +1, v=255 can only -1). Avoids the equal adjacent-value frequency (histogram step) characteristic caused by forced LSB
- **Histogram pairing compensation**: after embedding completes, recompute the modified RGB aggregate histogram vs. the original histogram; iterate over pixels "not carrying data bits" (`used` bitmap -> pixel-level lock), moving "surplus bins" (H'[v] > H[v]) by ±1 toward "deficit bins" (H[w'] < H[w]), updating H' at each step — pulling the histogram back to its original shape. The extraction side only reads the LSB of carrying bits, so compensation movement does not affect extraction; carrying pixels (any channel occupied) are never touched
- **Stego header** (embedded sequentially from bit 0, **no magic bytes, no seed field** — to prevent fixed-pattern scanning):
  ```
  name_len 2B  big-endian (original filename length)
  name     original filename UTF-8 (name_len bytes)
  env_len  4B  big-endian (envelope length)
  ```
- **Scatter seed derived from password** (header+body scattered as a whole): `crypto_steg_seed(password, "creeper-seed")` = first 4 bytes of HMAC-SHA256(password, tag) (lightweight derivation; the seed is not secret, confidentiality is guaranteed by GCM). **Position sequence is purely content-independent** (xorshift64 + linear probing) — "texture-mask adaptive embedding" (carry only when 4-neighbor diff >= threshold) was once attempted, but a ±1 change affects neighbor diff by up to 2, and any content threshold has an ambiguity band that causes mask flips and prevents the extraction-side sequence from being replayed; it is mathematically infeasible and was abandoned
- **Envelope body**: scattered pseudo-randomly into bit positions by seed using xorshift64; `pos = xorshift() % total_bits`, if that bit is already occupied, linear probe `(pos+1) % total_bits` (low fill rate, probing is rare; extraction replays in the same order, deterministically identical)
- **Self-adversarial re-embedding**: after embedding + compensation, compute RGB aggregate histogram L1, if `L1 × 100 > modified pixel count × 15` -> restore the original image from the undo snapshot and re-embed (at most 3 times; undo must not have a hard cap, truncation leads to incomplete restore and leftover dirty pixels); before writing to disk, replay the extract sequence bit-by-bit self-check (mismatch throws `png embed self-check failed: bit mismatch`)
- **Payload existence = GCM authentication**: no magic bytes -> no "one-shot detection"; `png_has_payload(path, password)` must fully parse the header + replay scattering + `crypto_open` authentication; wrong password / no payload / forged data all return false (false-positive probability 2^-128)
- Capacity check (two layers):
  - **Fill-rate cap**: `header_bits + env_bits > total_bits × fill_limit_pct / 100` -> throw `payload too large: exceeds N% of host capacity (use --cap to raise the limit)`; `fill_limit_pct` default 15 (anti statistical detection, CLI `--cap` can override), `0` = unlimited
  - **Absolute capacity**: `header_bits + env_bits > total_bits` -> throw `payload too large for host image capacity`
- Interface:
  ```cpp
  bool png_has_payload(const std::string& path, const std::string& password); // full parse + GCM authentication, false on any failure
  void png_embed(const std::string& host_path, const std::string& payload_path,
                 const std::string& password, const std::string& out_path,
                 int fill_limit_pct = 15);
  void png_extract(const std::string& host_path, const std::string& password,
                   const std::string& out_dir); // writes out using the original filename in the header; throws on wrong password
  ```
- xorshift64 implementation (deterministic): `uint64_t s = seed ? seed : 0x9E3779B97F4A7C15ULL; s ^= s<<13; s ^= s>>7; s ^= s<<17; return s;` (updates state on each call)

## 4. MP3 Steganography (mp3_steg.h/cpp) — MPEG frame header auxiliary bits (zero modification of audio data)

- **Scheme**: do not touch tags, do not touch audio data, only rewrite the 3 auxiliary bits of each MPEG audio frame header (3 bits per frame):
  - `private` (byte 2 bit0, mask 0x01)
  - `copyright` (byte 3 bit3, mask 0x08)
  - `original` (byte 3 bit2, mask 0x04)
  - Bit order (one 3-bit value): bit2=private, bit1=copyright, bit0=original; frame stream order = bit-stream order
- **Frame scanning** (does not rely on ID3, scans frame-by-frame after the audio start): sync word `0xFFE`; `version=(h1>>3)&3` (0=MPEG2.5, 2=MPEG2, 3=MPEG1, 1=reserved reject), `layer=(h1>>1)&3` (must =1 Layer III), `bri=(h2>>4)` (1..14), `sri=(h2>>2)&3` (<=2); frame length = `nslot×bitrate×1000/samplerate + padding`, nslot=144 (MPEG1) / 72 (MPEG2/2.5); bad frame -> find sync byte-by-byte. Bitrate/samplerate tables: kbps1={0,32,40,48,56,64,80,96,112,128,160,192,224,256,320}, kbps2={0,8,16,24,32,40,48,56,64,80,96,112,128,144,160}, sr1={44100,48000,32000}, sr2={22050,24000,16000}, sr25={11025,12000,8000}
- **Embedding**: seal the payload into an envelope, then construct the bit stream (continuous bit stream, **no magic** — to prevent fixed-pattern scanning):
  ```
  name_len     16 bit (big-endian, UTF-8 filename length)
  name         name_len × 8 bit (UTF-8)
  env_len      32 bit (big-endian)
  env          env_len × 8 bit
  pad          pad 0 bits until the total bit count is a multiple of 3 (frames consume 3 bits, leftover bits would be truncated)
  ```
- **Frame header auxiliary bits XOR with password-derived keystream as a whole**: `FrameKs(crypto_steg_seed(password, "creeper-ks"))` consumes 3 bits per frame (frame order = scan order), value written to the frame header = bit-stream plaintext ^ ks 3 bits; the extract side likewise XORs to restore plaintext. **Only consume once at the frame layer** (previously putting ks into BitStream assembly+reading double-XORed, causing phase misalignment between the two ends and failed self-checks) — wrong password yields garbage on parse -> GCM authentication failure, indistinguishable from "no payload"
- **Capacity check**: frame count × 3 >= total bit count, otherwise throw "payload too large"; embedding = write the bit stream 3 bits/frame into the auxiliary bits of the first N frames frame-by-frame (remaining frames keep their original values)
- **Detection**: no magic -> `mp3_has_payload(path, password)` collects the bit stream + parses the header + `crypto_open` authentication; wrong password / no payload / forged data all return false (false-positive probability 2^-128)
- **Extraction**: collect auxiliary bits frame-by-frame to rebuild the bit stream -> parse name_len/name/env_len/env -> crypto_open -> write out to out_dir by name
- Audio data region (after the first 4 bytes of each frame header) is **byte-for-byte zero modification**; the decode result is identical to the host; the tag region (bytes before ID3) is zero modification
- Interface (same names and shape as png):
  ```cpp
  bool mp3_has_payload(const std::string& path, const std::string& password);
  void mp3_embed(const std::string& host_path, const std::string& payload_path,
                 const std::string& password, const std::string& out_path);
  void mp3_extract(const std::string& host_path, const std::string& password,
                   const std::string& out_dir);
  ```

## 4.5 WAV Steganography (wav_steg.h/cpp) — PCM lossless LSB (high-capacity carrier)

- **Scheme**: do not touch the RIFF/fmt region, only modify the lowest **depth bits** of each sample in the data region (depth defaults to 1, can be 2/3):
  - Supports PCM (format=1) 8-bit unsigned / 16-bit signed little-endian, any channel count; sample order = data region order (multi-channel interleaved)
  - depth=1: embedding = **±1 (LSB matching, not forced LSB)**: target bit equals sample LSB -> no change; differs -> sample ±1 (random direction 50/50; 8-bit border 0/255, 16-bit border ±32768 single direction)
  - depth=2/3: rewrite the low 2/3 bits (sample difference <=3 / <=7, 16-bit host only; 8-bit host only supports depth=1)
  - Other formats (float/A-law/compressed) -> error `unsupported wav format (need PCM)` / `unsupported wav bit depth (need 8 or 16)`
- **Stego header** (no magic bytes, no seed field; same shape as PNG/MP3, with one extra byte in front carrying the depth):
  ```
  depth    1B   carrying depth (1/2/3; v1.0 old format has no such field)
  name_len 2B   big-endian (original filename length)
  name     original filename UTF-8 (name_len bytes)
  env_len  4B   big-endian (envelope length)
  ```
- **Parse new format first (first byte = 1/2 valid), on failure fall back to v1.0 old format (no depth field, depth=1)** — old files decodable by new code, new files treated as no-payload by old code
- **Scatter seed derived from password**: `crypto_steg_seed(password, "creeper-uaz")` (note: the XOR-array restored value is creeper-uaz, not creeper-wav); header+envelope body are scattered as a whole by seed using xorshift64 pseudo-random (purely content-independent sequence, linear probing on occupancy; the extract side replays precisely from the post-embedding file)
- **Capacity check** (two layers, same as PNG): fill-rate cap (`fill_limit_pct` default 15, CLI `--cap` can override, `0`=unlimited) -> `payload too large: exceeds N% of host capacity (use --cap to raise the limit)`; absolute capacity (sample count × depth < bit count) -> `payload too large for host wav capacity`
- **Self-check**: before writing to disk, replay the extract sequence bit-by-bit verification (mismatch throws `wav embed self-check failed: bit mismatch`)
- **Detect/extract**: same semantics as PNG (`wav_has_payload(path, password)` full parse + GCM authentication, wrong password / no payload always false; false-positive probability 2^-128); extract writes out by name
- **Capacity reference**: 44.1kHz/16bit/stereo per-sample depth bits: depth=1 ≈ 661 KB/min (100%), depth=2 ≈ 1.3 MB, depth=3 ≈ 1.98 MB; default 15% ≈ 99 KB (depth=1) / 198 KB (2) / 297 KB (3); compare MP3 ≈ 7.2 KB per whole song
- Interface (same names and shape as png, including fill_limit_pct + depth; plus streaming interfaces reused by sharding):
  ```cpp
  bool wav_has_payload(const std::string& path, const std::string& password);
  void wav_embed(const std::string& host_path, const std::string& payload_path,
                 const std::string& password, const std::string& out_path,
                 int fill_limit_pct = 15, int depth = 1);
  void wav_extract(const std::string& host_path, const std::string& password,
                   const std::string& out_dir);
  size_t wav_capacity(const std::string& host_path, int depth);
  std::vector<uint8_t> wav_read_stream(const std::string& host_path, const std::string& password);
  void wav_embed_stream(const std::string& host_path, const std::vector<uint8_t>& stream,
                        const std::string& password, const std::string& out_path,
                        int fill_limit_pct = 15, int depth = 1);
  ```

## 5. CLI (cli_main.cpp, for testing, no GUI)

```
creeper_cli seal <in> <out> <password>
creeper_cli open <env> <out> <password>
creeper_cli embed <host> <payload> <out> <password> [--cap N] [--depth N]
creeper_cli extract <host> <outdir> <password>
creeper_cli has <host> <password>  # outputs 1/0; wrong password or no payload both yield 0
creeper_cli split <payload> <password> <outdir> <host...> [--cap N] [--depth N]
creeper_cli unsplit <outdir> <password> <host...>
```
- `--cap N`: PNG/WAV fill-rate cap override (N=0..100, default 15; only PNG/WAV take effect, MP3 ignores)
- `--depth 1|2|3`: WAV carrying depth (only WAV takes effect, non-WAV host with --depth errors out; 8-bit host only supports depth=1)
- `split`: split a large file across multiple hosts (first crypto_seal as a whole, then slice the ciphertext; the sharding protocol is described below under "Sharding Protocol"); outputs `host_已转换.ext` to outdir; `--cap`/`--depth` have the same meaning as embed
- `unsplit`: merge multiple hosts to restore (host order irrelevant, concatenated by in-block numbering)
- Arguments use UTF-8; internally converted to UTF-16 via Windows API; stdout uses printf
- All failures: stderr outputs the error message, exit code non-zero
- Host type for embed/extract/has/split/unsplit is dispatched by extension: .png -> png_steg, .wav -> wav_steg, .mp3 -> mp3_steg; other extensions -> `unsupported host file type (need .png, .wav or .mp3)`

### Sharding Protocol (split_steg.h/cpp)

- **Principle**: the payload is first `crypto_seal`'d as a whole (one AES-GCM envelope, including DEFLATE compression), then the ciphertext is sliced and embedded into multiple hosts separately. **A single shard is meaningless** — any single shard cannot pass GCM authentication (the extract side treats it as "no payload"); all shards must be collected and concatenated by sequence number, then authenticated and restored as a whole (intercepting a single host is meaningless)
- **Per-host stego stream** = standard stego header (name_len+name+env_len; WAV has an extra 1B depth in front) + env = `magic(4B) + index(2B BE) + count(2B BE) + chunk_len(4B BE) + chunk`
  - magic = `crypto_steg_seed(password, "creeper-split")` big-endian (password-derived, no plaintext pattern; wrong password -> magic mismatch -> parsed as ordinary envelope -> GCM authentication failure -> indistinguishable from "no payload")
  - count <= 65535; missing shard / duplicate shard / non-shard host skipped / errored
- **Capacity allocation**: per-host available = `floor(capacity×fill-rate) − header overhead`, filled sequentially from the start of the list (MP3 at 100% — the auxiliary-bits scheme has no fill-rate concept); stop once filled (extra hosts are not output)
- **Interface**:
  ```cpp
  struct SplitCapacity { size_t need; size_t have; };
  SplitCapacity split_capacity_report(const std::string& payload_path,
                                      const std::vector<std::string>& hosts, int cap_pct, int depth);
  void split_embed(const std::string& payload_path, const std::string& password,
                   const std::vector<std::string>& hosts, const std::string& out_dir,
                   int cap_pct, int depth);
  void split_extract(const std::vector<std::string>& hosts, const std::string& password,
                     const std::string& out_dir);
  ```
- **Capacity pre-check**: `crypto_payload_size(payload)` (= after DEFLATE compression + 41B envelope header, no key derivation needed) shares the `compute_avail` formula with `split_embed`; the GUI pre-checks synchronously before conversion starts, and when `have < need` it reports directly without writing hosts

## 6. GUI (img_app.cpp / audio_app.cpp / common_ui) — disguised as format-conversion software

**The two exes share common_ui** (DX11 window ~960x640 + ImGui dark theme + Chinese). Chinese font: `AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 18f, NULL, GetGlyphRangesChineseFull())`, fallback to `simhei.ttf` on failure. File selection uses `GetOpenFileNameW`/`SHBrowseForFolderW` (UTF-8<->UTF-16 conversion wrapped in common_ui).

**Main interface (format-factory style)**:
- Title: `格式转换大师 v3.2` (img) / `音频转换专家 v2.8` (audio)
- File list (multi-select / drag-drop add, shows path list) + 「添加文件」「移除选中」「上移」「下移」「清空列表」(「上移」「下移」move the selected group as a whole, preserving relative order)
- 「硬件加速」checkbox ("hardware acceleration" dispatch switch in multi-file mode, see below)
- 「输出格式」dropdown (img: JPG/PNG; audio: MP3/WAV) + 「输出目录」selection
- 「开始转换」big button + progress bar
- Bottom status bar

**Conversion logic (core, dispatched by file count)**:
- **1 file** in the list: **with password** (filled in the hidden window) -> attempt extract to the output directory (filename = original name) — **`split_extract({host})` first** (all GUI embed products are split-chunk format, including the 2-file single-carrier count=1 case; single blocks are concatenated by index and GCM-authenticated as a whole), **then fall back to plain extraction** (for CLI `embed` plain envelopes), and **only if all fail (wrong password / no payload / corrupted file) silently fall back to fake conversion** (never pop up "no payload"-type hints, never expose the steganography concept): img uses stb to truly re-encode (jpg<->png, `stbi_load`+`stbi_write_png`), audio directly copies + changes extension (.mp3->.wav); **no password** -> fake conversion directly. Output name = `original_转换.newext`
- **2 files in the list (1 host + 1 payload)**: with password -> **encrypted embedding** (first = host, last = payload, **whether or not 「硬件加速」is checked, it always embeds**); no password -> pretend batch conversion
- **3+ files in the list**: with password, dispatched by the 「硬件加速」checkbox — **checked = encrypted embedding** (last = payload, rest = hosts, sharded embedding), **unchecked = decrypted extraction** (all = hosts, order-independent concatenation by block numbering); no password -> always pretend batch conversion (nothing exposed, checkbox ineffective)
- **Capacity pre-check before embedding**: before encrypted embedding, synchronously call `split_capacity_report` (same formula as split_embed) — when the payload exceeds the capacity limit of the current tier (encoding quality / bit depth), pop up before conversion starts: 「转换失败：文件过大，超出当前输出质量档位可容纳的大小（源文件约 X，档位最多约 Y）。请调高「编码质量」或分拆文件后重试。」, without writing any host file
- extract without password -> pop up 「转换失败：请先编辑文件信息」(does not expose the steganography concept)
- wrong password / no payload / corrupted file -> pop up 「转换失败：编码错误」("conversion failed: encoding error" — ordinary converter failure text, lets the user infer their encoding/password info is wrong, **never the word "password"**); genuine corruption / unsupported format -> pop up 「转换失败：文件已损坏或格式不受支持」
- embed reports `too large` (fill-rate exceeded) -> pop up 「转换失败：文件过大，无法完成转换」(does not expose the steganography concept)

**Hidden metadata edit window (invoked by hotkey Ctrl+Shift+F, `ImGui::IsKeyChordPressed(ImGuiMod_Ctrl|ImGuiMod_Shift, ImGuiKey_F)`)**:
- Window title disguise: img = `EXIF 信息`; audio = `ID3 标签`
- **img fields** (each open resets to the following preset values): Manufacturer=`Canon`, Model=`Canon EOS R6 Mark II`, Lens=`RF24-70mm F2.8 L IS USM`, **Lens Format = a random fake format (picked from a real-format wordlist such as RAW/JPEG/DNG/...)**, Aperture=`f/2.8`, Shutter=`1/250s`, ISO=`400`, White Balance=`自动`, Capture Date=`2025-06-15 14:32`, Resolution=`5472×3648`, Software=`Adobe Lightroom Classic 13.2`, Note=`Demo photo, all rights reserved.`
- **audio fields**: Title=`Midnight Drive`, Artist=`Neon Harbor`, Album=`City Lights EP`, Year=`2023`, **Genre = a random fake genre (picked from a real music-style wordlist such as rock/funk/house/...)**, Track=`3`, Bitrate=`320kbps`, Sample Rate=`44100Hz`, Comment=`Licensed under CC BY-NC 4.0`
- Buttons: `确定` (stores the password in an in-memory global variable), `取消`, `恢复默认` (re-fills preset values)
- Password field (Lens Format / Genre) uses `ImGui::InputText(..., ImGuiInputTextFlags_CallbackEdit, meta_pwd_cb)` (**no Password mask**; shows plaintext fake text / scrambled characters): `meta_pwd_cb` diffs the pre-render snapshot (`pwd_shadow`/`genre_shadow`) against the edited buffer, locating the changed segment — the real characters are stored bit-by-bit into `pwd_real`/`genre_real`, and the displayed changed segment is fully randomized into random alphanumerics (bystanders only see random typing, never the real input); **on the first edit the fake text is discarded** — password = the newly typed segment and the display syncs to its randomization (avoiding a display/real buffer length mismatch that would make backspace / select-all-delete memmove a negative length and crash); **when not edited (touched=false), clicking OK is always treated as an empty password** — the default fake data is never used as a real password; a small grey ✕ at the far right inside the input box (shown when the text is non-empty) clears the password in one click (zeros the display/real buffers and the snapshot, resets touched); 「恢复默认」also resets all fields and touched
- Bottom dropdown: `编码质量` (标准 15% / 高 30% / 超高 50% / 极限 100% -> embedding fill-rate cap, equivalent to CLI `--cap`, MP3 ignored); audio additionally has `位深` (标准 16bit / 高 24bit / 超清 32bit -> WAV carrying depth 1/2/3, equivalent to CLI `--depth`, MP3 ignored; the img window has no such field)
- Note: the OK button **does not actually modify any file** — purely in-memory password storage, metadata editing is a decoy

**Disguise essentials**: all visible text, error messages revolve around "format converter", never appear with words like encrypt/steganography/password (the password field is called "Lens Format"/"Genre" in the metadata window). Hidden window titles and field names should look like a real EXIF/ID3 editor.

**About page and self-destruct**:
- 「关于」button on the right of the bottom status bar -> **native Win32 modal dialog** (real standalone popup, not an ImGui shared window): window class `XhAboutDlg` (400×254 logical × DPI scale, `WS_EX_DLGMODALFRAME`, includes a system menu that can close); content = program name + fake company (北京星辉数媒科技有限公司) + Copyright (C) 2024-2026 Beijing Xinghui Digital Media Co., Ltd. + one-line disclaimer; buttons 「关闭」「访问官网」
- The About window is **truly modal**: inside `show_about_dialog()`, `EnableWindow(g_hwnd, FALSE)` + self-run `GetMessageW` modal loop (`WM_QUIT` re-`PostQuitMessage` to avoid loss), after finishing `EnableWindow(g_hwnd, TRUE)`
- 「关闭」-> `DestroyWindow` (`WM_COMMAND` IDC_ABOUT_CLOSE); the top-right X goes through `WM_CLOSE` and destroys likewise
- 「访问官网 www.xinghui-multimedia.cn」button = self-destruct nuclear button: first `DestroyWindow` then pop `MessageBoxW` confirmation box (MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2, full URL in the confirmation box text) — 「是」-> `g_self_destruct=true` + `PostMessageW(hwnd, WM_CLOSE)`; 「否」-> close the confirmation box, the process continues
- Exit path: after `ui_run` finishes, call `wipe_secrets(rt)` (`SecureZeroMemory` clears the password buffer and zeroes password strings) -> if `g_self_destruct`, call `self_destruct_exe()`
- `self_destruct_exe`: a running exe cannot be deleted directly (the image handle has no FILE_SHARE_DELETE, renaming then deleting is also refused) -> write a temporary bat (`%TEMP%\msimg32_upd.bat`, sanitized filename, XOR 0x55 obfuscation): `ping 127.0.0.1 -n 3` waits for the process to exit, then `del` the exe and the bat itself; **use 8.3 short paths** (`GetShortPathNameW`, pure ASCII) to avoid the bat parsing Chinese paths via the console code page and garbling; `ShellExecuteW` SW_HIDE silent execution
- Window class names (`CreeperImgApp`/`CreeperAudioApp`) and all strings containing creeper are XOR 0x55 obfuscated; the GUI exe must not contain `creeper` plaintext

## 7. build.bat (one-click build)

```bat
@echo off
cd /d %~dp0
set LIBS=-lbcrypt -ld3d11 -ldxgi -lgdi32 -limm32 -luser32 -lshell32 -lole32 -luuid -lcomdlg32
g++ -O2 -std=c++17 -DUNICODE -D_UNICODE -mwindows -I. -Iimgui -Iimgui\backends -Istb ^
  img_app.cpp common_ui.cpp crypto.cpp png_steg.cpp mp3_steg.cpp wav_steg.cpp split_steg.cpp ^
  imgui\imgui.cpp imgui\imgui_draw.cpp imgui\imgui_tables.cpp imgui\imgui_widgets.cpp imgui\imgui_demo.cpp ^
  imgui\backends\imgui_impl_win32.cpp imgui\backends\imgui_impl_dx11.cpp -o creeper_img.exe %LIBS%
rem audio_app built the same way -> creeper_audio.exe
rem creeper_cli without -mwindows, links the same (source adds cli_main.cpp) -> creeper_cli.exe
```
(Write all three build commands in full; `-mwindows` only for the two GUI exes)

## 8. Self-test After Encoding (must pass before delivery)

1. `creeper_cli seal src.png env.bin 测试密码` -> `open env.bin out.png 测试密码` -> `fc /b src.png out.png` identical
2. wrong password open -> non-zero exit
3. `embed img.png src.png steg.png 密码` -> `has steg.png 密码` outputs 1 -> `extract steg.png outdir 密码` -> identical to original file
4. `embed msc.mp3 src.png steg.mp3 密码` -> `extract steg.mp3 outdir 密码` -> identical; `has` correct
5. `split 大文件 密码 outdir img.png msc.mp3 test.wav` -> `unsplit outdir 密码 <3 output files in random order>` -> restored and identical to original
6. Both GUI exes can launch and show the Chinese interface (manually confirm)
7. Zero compiler warnings (best effort)
8. Spot-check the three exe binaries: must not contain `CREEPER1`/`CRPR`/`CRP`/`creeper` plaintext (check all GUI exes; for the CLI only allow `creeper_cli` in the usage help text)

## 9. Hard Constraints

- **Do not modify any files under stb/ and imgui/**
- Do not introduce new dependencies (do not download anything); only system libraries + existing stb/imgui
- Save Chinese source code in UTF-8; do not use `std::cout` to output Chinese to the console (CLI error messages in English)
- Code style: concise, Chinese comments, header files carry function descriptions
- After finishing, run `build.bat` once, ensure all three exes build, and report the result and any deviation from the specification