// mp3_steg.cpp — MP3 帧头辅助位隐写实现（无魔数）
// 原理：每帧 MPEG 音频帧头有 3 个"辅助位"（private / copyright / original），
//       修改它们不影响解码器（多数解码器忽略），音频数据字节零改动。
//       位流按帧排列：每帧贡献 3 位（private=最高位, copyright, original=最低位）。
// 布局（帧序列）：name_len(16bit) | name(8bit×n) | env_len(32bit) | env(8bit×m)，
//                 末尾填充 0 位到 3 的倍数（无魔数——GCM 认证即载荷真实性校验）
#include "mp3_steg.h"

#include "crypto.h"
#include "file_util.h"

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// XOR 0x55 混淆常量还原（运行时还原，exe 内无明文）
std::string xstr(const uint8_t* x, size_t n) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; i++) s[i] = (char)(x[i] ^ 0x55);
    return s;
}
// "creeper-ks"：位流 keystream 派生标签（随密码变化，消除明文头结构）
static const uint8_t kKsTagXor[] = {0x36, 0x27, 0x30, 0x30, 0x25, 0x30, 0x27, 0x78, 0x3E, 0x26};

// xorshift64：确定性伪随机（与 png_steg 同构；seed 为 0 时使用固定常数）
struct XorShift64 {
    uint64_t s;
    explicit XorShift64(uint32_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ULL) {}
    uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
};

// ---------- ID3v2 标签定位（沿用原逻辑，仅用于找到音频区起点） ----------
uint32_t syncsafe_decode(const uint8_t* p) {
    return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7) | (uint32_t)(p[3] & 0x7F);
}

// 返回音频区起始偏移（跳过 ID3v2 头 + 标签 + padding）；无标签返回 0
size_t audio_start(const std::vector<uint8_t>& f) {
    if (f.size() >= 10 && std::memcmp(f.data(), "ID3", 3) == 0) {
        int major = f[3];
        int flags = f[5];
        if (major >= 2 && major <= 4 && !(flags & 0x0F)) {
            size_t end = 10 + syncsafe_decode(f.data() + 6);
            if (end <= f.size()) {
                // 跳过 padding（全零）直到帧同步
                size_t p = end;
                while (p < f.size() && f[p] == 0) p++;
                return p;
            }
        }
    }
    return 0;
}

// ---------- MPEG 音频帧扫描 ----------
// 表：MPEG1 Layer III 比特率（kbps），索引 1..14
static const int kbps1[] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320};
// 表：MPEG2/2.5 Layer III 比特率（kbps），索引 1..14
static const int kbps2[] = {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160};
// 采样率表：index 0..2
static const int sr1[] = {44100,48000,32000};   // MPEG1
static const int sr2[] = {22050,24000,16000};   // MPEG2
static const int sr25[] = {11025,12000,8000};   // MPEG2.5

struct FrameScan {
    std::vector<size_t> offsets; // 每帧头偏移
    size_t total = 0;            // 扫描覆盖到文件末尾
};

// 逐帧扫描音频区；遇到坏帧向后逐字节找同步；无任何合法帧返回空
FrameScan scan_frames(const std::vector<uint8_t>& f, size_t start) {
    FrameScan sc;
    size_t pos = start;
    while (pos + 4 <= f.size()) {
        const uint8_t* h = f.data() + pos;
        if (h[0] == 0xFF && (h[1] & 0xE0) == 0xE0) {
            int version = (h[1] >> 3) & 3; // 0=2.5, 2=2, 3=1
            int layer = (h[1] >> 1) & 3;   // 1=III
            int bri = (h[2] >> 4) & 0x0F;
            int sri = (h[2] >> 2) & 3;
            if (version != 1 && layer == 1 && bri >= 1 && bri <= 14 && sri <= 2) {
                int bitrate, samplerate, nslot;
                if (version == 3) { // MPEG1
                    bitrate = kbps1[bri];
                    samplerate = sr1[sri];
                    nslot = 144;
                } else if (version == 2) { // MPEG2
                    bitrate = kbps2[bri];
                    samplerate = sr2[sri];
                    nslot = 72;
                } else { // MPEG2.5
                    bitrate = kbps2[bri];
                    samplerate = sr25[sri];
                    nslot = 72;
                }
                int padding = (h[2] >> 1) & 1;
                size_t fsize = (size_t)(nslot * bitrate * 1000 / samplerate) + (size_t)padding;
                if (fsize < 4 || pos + fsize > f.size()) { pos++; continue; }
                sc.offsets.push_back(pos);
                pos += fsize;
                continue;
            }
        }
        pos++; // 坏帧/不同步：逐字节找
    }
    return sc;
}

// 提取帧头辅助 3 位（返回 0..7，bit2=private, bit1=copyright, bit0=original）
int aux_bits(const uint8_t* h) {
    int v = 0;
    v |= (h[2] & 0x01) << 2; // private
    v |= ((h[3] >> 3) & 0x01) << 1; // copyright
    v |= ((h[3] >> 2) & 0x01);      // original
    return v;
}
// 写入帧头辅助 3 位
void set_aux_bits(uint8_t* h, int v) {
    h[2] = (uint8_t)((h[2] & ~0x01) | ((v >> 2) & 0x01));
    h[3] = (uint8_t)((h[3] & ~0x08) | (((v >> 1) & 0x01) << 3));
    h[3] = (uint8_t)((h[3] & ~0x04) | ((v & 0x01) << 2));
}

// ---------- 位流（连续 bit 流；帧侧每帧消费/产出 3 位） ----------
// 位流本身是明文组装；密码派生 keystream 在"帧头辅助位"层单次 XOR：
// 帧 aux_i = plain_i ^ ks_i（嵌入端）；提取端同样 XOR 还原 plain。
// 只在一层消费 keystream，避免多重 XOR 导致的两端阶段错位。
struct BitStream {
    std::vector<uint8_t> bytes;
    size_t total_bits = 0; // 已写入位数
    size_t cursor = 0;     // 读取位置
    void write_bit(int b) {
        if (total_bits % 8 == 0) bytes.push_back(0);
        if (b) bytes[total_bits / 8] |= (uint8_t)(0x80 >> (total_bits % 8));
        total_bits++;
    }
    void write3(int v) {
        write_bit((v >> 2) & 1);
        write_bit((v >> 1) & 1);
        write_bit(v & 1);
    }
    void append_bytes(const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; i++) {
            for (int b = 7; b >= 0; b--) write_bit((p[i] >> b) & 1);
        }
    }
    int read_bit() {
        if (cursor >= total_bits) return -1;
        int v = (bytes[cursor / 8] >> (7 - (cursor % 8))) & 1;
        cursor++;
        return v;
    }
    int read3() {
        int v = 0;
        for (int i = 0; i < 3; i++) {
            int b = read_bit();
            if (b < 0) return -1;
            v = (v << 1) | b;
        }
        return v;
    }
};

// 帧级 keystream：每帧消费 3 位（与帧扫描序一一对应）
struct FrameKs {
    XorShift64 rng;
    uint64_t cur = 0;
    int cnt = 0;
    explicit FrameKs(uint32_t seed) : rng(seed) {}
    int bit() {
        if (cnt == 0) {
            cur = rng.next();
            cnt = 64;
        }
        int b = (int)(cur >> 63) & 1;
        cur <<= 1;
        cnt--;
        return b;
    }
    int next3() { return (bit() << 2) | (bit() << 1) | bit(); }
};

} // namespace

// 解析宿主 MP3 的载荷：收集辅助位 → 解析位流 → crypto_open；
// 返回 (载荷, 原始文件名)；密码错误/无载荷/格式损坏抛异常。
// write_to 非空时把载荷写出到该路径（extract 用）；为空时只解析不落盘（检测用）
std::vector<uint8_t> mp3_parse(const std::string& host_path, const std::string& password,
                               const std::string& write_to, std::string& out_name) {
    std::vector<uint8_t> f = read_file_bytes(host_path);
    FrameScan sc = scan_frames(f, audio_start(f));
    if (sc.offsets.size() < 8) throw std::runtime_error("no payload found in host audio");

    BitStream bs;
    FrameKs fks(crypto_steg_seed(password, xstr(kKsTagXor, sizeof(kKsTagXor))));
    for (size_t i = 0; i < sc.offsets.size(); i++)
        bs.write3(aux_bits(f.data() + sc.offsets[i]) ^ fks.next3());

    // name_len + name（逐位）
    uint16_t nl = 0;
    for (int i = 0; i < 16; i++) {
        int v = bs.read_bit();
        if (v < 0) throw std::runtime_error("no payload found in host audio");
        nl = (uint16_t)((nl << 1) | v);
    }
    if (nl == 0 || nl > 512) throw std::runtime_error("no payload found in host audio");
    std::string name;
    for (int i = 0; i < nl; i++) {
        int byte = 0;
        for (int b = 0; b < 8; b++) {
            int v = bs.read_bit();
            if (v < 0) throw std::runtime_error("no payload found in host audio");
            byte = (byte << 1) | v;
        }
        name.push_back((char)byte);
    }

    // env_len + env（逐位）
    uint32_t el = 0;
    for (int i = 0; i < 32; i++) {
        int v = bs.read_bit();
        if (v < 0) throw std::runtime_error("no payload found in host audio");
        el = (el << 1) | (uint32_t)v;
    }
    if (el == 0 || el > sc.offsets.size() * 3 / 8) throw std::runtime_error("no payload found in host audio");
    std::vector<uint8_t> env;
    env.reserve(el);
    for (uint32_t i = 0; i < el; i++) {
        int byte = 0;
        for (int b = 0; b < 8; b++) {
            int v = bs.read_bit();
            if (v < 0) throw std::runtime_error("no payload found in host audio");
            byte = (byte << 1) | v;
        }
        env.push_back((uint8_t)byte);
    }

    // 解信封（密码错误抛异常；GCM 认证 = 载荷真实性校验）
    std::vector<uint8_t> payload = crypto_open(env, password);

    // 文件名清洗：仅保留 basename，防路径穿越
    out_name = file_basename(name);
    if (out_name.empty() || out_name == "." || out_name == "..")
        throw std::runtime_error("invalid payload filename");
    if (!write_to.empty()) write_file_bytes(write_to, payload);
    return payload;
}

bool mp3_has_payload(const std::string& path, const std::string& password) {
    try {
        std::string name;
        mp3_parse(path, password, "", name);
        return true;
    } catch (...) {
        return false;
    }
}

void mp3_embed(const std::string& host_path, const std::string& payload_path,
               const std::string& password, const std::string& out_path) {
    std::vector<uint8_t> f = read_file_bytes(host_path);
    if (f.empty()) throw std::runtime_error("host file is empty");
    std::vector<uint8_t> payload = read_file_bytes(payload_path);
    if (payload.empty()) throw std::runtime_error("payload file is empty");
    std::vector<uint8_t> env = crypto_seal(payload, password);
    std::string name = file_basename(payload_path);

    FrameScan sc = scan_frames(f, audio_start(f));
    if (sc.offsets.empty()) throw std::runtime_error("no MPEG audio frames found in host");

    // 位流：name_len | name | env_len | env（无魔数；帧头辅助位整体 XOR 密码派生
    // keystream，消除明文头结构——密码错则解析出垃圾 → GCM 认证失败，
    // 与"无载荷"不可区分）
    BitStream bs;
    uint16_t nl = (uint16_t)name.size();
    for (int i = 15; i >= 0; i--) bs.write_bit((nl >> i) & 1);
    bs.append_bytes((const uint8_t*)name.data(), name.size());
    uint32_t el = (uint32_t)env.size();
    for (int i = 31; i >= 0; i--) bs.write_bit((el >> i) & 1);
    bs.append_bytes(env.data(), env.size());
    // 填充到 3 的倍数：帧按 3 位消费，余位会被截断
    while (bs.total_bits % 3 != 0) bs.write_bit(0);

    // 容量检查：帧数 × 3 ≥ 总位数
    if (sc.offsets.size() * 3 < bs.total_bits)
        throw std::runtime_error("payload too large for host mp3 capacity (frames=" +
                                 std::to_string(sc.offsets.size()) + " bits=" + std::to_string(bs.total_bits) + ")");

    std::vector<uint8_t> out = f;
    FrameKs fks(crypto_steg_seed(password, xstr(kKsTagXor, sizeof(kKsTagXor))));
    for (size_t i = 0; i < sc.offsets.size(); i++) {
        int v = bs.read3();
        if (v < 0) break;
        set_aux_bits(out.data() + sc.offsets[i], v ^ fks.next3());
    }
    write_file_bytes(out_path, out);

    // 自检：回读文件，重建位流（同一 keystream 解回）逐位对比（防写入/偏移错位）
    {
        std::vector<uint8_t> rb = read_file_bytes(out_path);
        FrameScan rs = scan_frames(rb, audio_start(rb));
        BitStream rbs;
        FrameKs rks(crypto_steg_seed(password, xstr(kKsTagXor, sizeof(kKsTagXor))));
        for (size_t i = 0; i < rs.offsets.size() && i < sc.offsets.size(); i++)
            rbs.write3(aux_bits(rb.data() + rs.offsets[i]) ^ rks.next3());
        bool ok = rbs.total_bits >= bs.total_bits;
        if (ok) {
            for (size_t i = 0; i < bs.total_bits; i++) {
                int e = (bs.bytes[i / 8] >> (7 - (i % 8))) & 1;
                int g = (rbs.bytes[i / 8] >> (7 - (i % 8))) & 1;
                if (e != g) { ok = false; break; }
            }
        }
        if (!ok) throw std::runtime_error("mp3 embed self-check failed: bitstream mismatch");
    }
}

void mp3_extract(const std::string& host_path, const std::string& password, const std::string& out_dir) {
    create_dirs(out_dir);
    std::string name;
    std::vector<uint8_t> payload = mp3_parse(host_path, password, "", name);
    std::string out_path = out_dir.empty() ? name : out_dir + "/" + name;
    write_file_bytes(out_path, payload);
}