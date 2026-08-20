// wav_steg.cpp — WAV（PCM）无损 LSB 隐写实现（无魔数）
// 位流顺序：data 区样本顺序（多通道交错，小端），每样本 depth 位（低 depth bit，
// 样本内 LSB 优先）；字节内 MSB 优先（bit0 = 字节最高位）。RIFF/fmt 区与样本高位零改动。
#ifdef _WIN32
#define _CRT_RAND_S // 启用 rand_s（CRT 级安全随机，导入表不含 bcrypt）
#include <windows.h>
#endif
#include "wav_steg.h"

#include "crypto.h"
#include "file_util.h"

#include <cstring>
#include <cstdlib>
#include <random>
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
// "creeper-uaz"：散布种子派生标签（随密码变化，消除明文固定位置；XOR 0x55 还原值
// 为 creeper-uaz——历史命名，勿改数组，否则所有已嵌入文件无法解析）
static const uint8_t kSeedTagXor[] = {0x36, 0x27, 0x30, 0x30, 0x25, 0x30, 0x27, 0x78, 0x20, 0x34, 0x2F};

// xorshift64：与 png/mp3 同构的确定性伪随机（seed 为 0 时使用固定常数）
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

// 位流字节取位（MSB 优先）
inline int get_bit(const uint8_t* data, size_t i) {
    return (data[i >> 3] >> (7 - (i & 7))) & 1;
}

// ---------- WAV 解析 ----------
struct WavInfo {
    size_t data_off = 0;   // data chunk 数据区起始
    size_t data_len = 0;   // 数据区字节数
    int bits = 16;         // 位深（8 / 16）
    size_t sample_count = 0; // 样本数（每样本承载 depth 位）
};

uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

WavInfo parse_wav(const std::vector<uint8_t>& f) {
    if (f.size() < 12) throw std::runtime_error("cannot decode wav file (too small)");
    if (std::memcmp(f.data(), "RIFF", 4) != 0 || std::memcmp(f.data() + 8, "WAVE", 4) != 0)
        throw std::runtime_error("not a RIFF/WAVE file");
    WavInfo info;
    bool have_fmt = false, have_data = false;
    size_t pos = 12;
    while (pos + 8 <= f.size()) {
        const uint8_t* h = f.data() + pos;
        uint32_t sz = rd32(h + 4);
        const uint8_t* body = h + 8;
        size_t body_len = std::min((size_t)sz, f.size() - (pos + 8));
        if (std::memcmp(h, "fmt ", 4) == 0) {
            if (body_len >= 16) {
                uint16_t fmt = (uint16_t)(body[0] | (body[1] << 8));
                uint16_t ch = (uint16_t)(body[2] | (body[3] << 8));
                uint16_t bps = (uint16_t)(body[14] | (body[15] << 8));
                if (fmt != 1) throw std::runtime_error("unsupported wav format (need PCM)");
                if (ch == 0) throw std::runtime_error("invalid wav channels");
                if (bps != 8 && bps != 16)
                    throw std::runtime_error("unsupported wav bit depth (need 8 or 16)");
                info.bits = bps;
                have_fmt = true;
            }
        } else if (std::memcmp(h, "data", 4) == 0) {
            info.data_off = pos + 8;
            info.data_len = body_len;
            have_data = true;
        }
        pos += 8 + sz + (sz & 1); // chunk 奇数大小补 1 字节 padding
        if (pos > f.size()) break;
    }
    if (!have_fmt) throw std::runtime_error("wav file missing fmt chunk");
    if (!have_data) throw std::runtime_error("wav file missing data chunk");
    if (info.data_len == 0) throw std::runtime_error("wav data chunk is empty");
    size_t bytes_per_sample = (size_t)info.bits / 8;
    if (info.data_len % bytes_per_sample != 0)
        throw std::runtime_error("wav data chunk size not aligned to sample size");
    info.sample_count = info.data_len / bytes_per_sample;
    return info;
}

// 读/写样本值（小端；8-bit 无符号 / 16-bit 有符号）
inline int read_sample(const uint8_t* p, int bits) {
    if (bits == 8) return p[0];
    return (int)(int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
inline void write_sample(uint8_t* p, int bits, int v) {
    if (bits == 8) {
        p[0] = (uint8_t)v;
    } else {
        int16_t s = (int16_t)v;
        p[0] = (uint8_t)(s & 0xFF);
        p[1] = (uint8_t)((s >> 8) & 0xFF);
    }
}

// 随机 ±1 修改样本 LSB（LSB matching；边界单方向）；返回是否实际修改
// 高频随机（±1 方向等）：一次性从系统 CSPRNG 播种的快速 PRNG（mt19937_64）
static uint64_t FastRand64() {
    static std::mt19937_64 gen = [] {
        uint8_t b[8];
        secure_random_bytes(b, sizeof(b));
        uint64_t seed = 0;
        for (int i = 0; i < 8; i++) seed |= ((uint64_t)b[i]) << (8 * i);
        return std::mt19937_64(seed);
    }();
    return gen();
}

static uint64_t SampRand() {
    return FastRand64();
}
inline bool set_sample_bit_rand(uint8_t* data, int bits, size_t sample_idx, uint8_t bit) {
    uint8_t* p = data + sample_idx * ((size_t)bits / 8);
    int v = read_sample(p, bits);
    if ((v & 1) == bit) return false;
    int d = (SampRand() & 1) ? -1 : 1;
    int lo = (bits == 8) ? 0 : -32768;
    int hi = (bits == 8) ? 255 : 32767;
    if (v == lo) d = 1;
    if (v == hi) d = -1;
    write_sample(p, bits, v + d);
    return true;
}

// 重写样本低 k 位（k=2 时 ±3 以内；k 位内无溢出——目标值保留高位块，必在有效范围）
inline bool set_sample_lowbits(uint8_t* data, int bits, size_t sample_idx, int k, uint8_t val) {
    uint8_t* p = data + sample_idx * ((size_t)bits / 8);
    int v = read_sample(p, bits);
    int mask = (1 << k) - 1;
    int t = (v & ~mask) | (val & mask);
    if (t == v) return false;
    write_sample(p, bits, t);
    return true;
}

// 散布读取器：seed 由密码派生（无明文 seed 字段），同序重放；
// 位置序列纯内容无关（密码派生 xorshift + 线性探测）→ 嵌入后文件可精确重放
struct ScatterReader {
    const std::vector<uint8_t>& f;
    const WavInfo& info;
    int depth;
    std::vector<uint8_t> used;
    XorShift64 rng;

    ScatterReader(const std::vector<uint8_t>& file, const WavInfo& wi, uint32_t seed, int d)
        : f(file), info(wi), depth(d), used(wi.sample_count, 0), rng(seed) {}

    size_t next_pos() {
        size_t p = (size_t)(rng.next() % info.sample_count);
        while (used[p]) p = (p + 1) % info.sample_count;
        used[p] = 1;
        return p;
    }
    // 读 k 位（k ≤ depth；样本内 LSB 优先）
    uint8_t read_bits(int k) {
        size_t s = next_pos();
        int v = read_sample(f.data() + info.data_off + s * ((size_t)info.bits / 8), info.bits);
        return (uint8_t)(v & ((1 << k) - 1));
    }
    // 读 1 字节（字节内 MSB 优先，与嵌入侧 get_bit 位序一致）。
    // 逐位消费：样本切换按 next_pos 推进；depth 位内 LSB 优先（流位 → (样本, 样本内位)）。
    // depth=3 时字节边界与样本边界不对齐（3 样本=9 位>8），必须跨样本缓冲剩余位。
    size_t cur_pos = SIZE_MAX;
    int cur_consumed = 0;
    uint8_t cur_v = 0;

    uint8_t next_bit() {
        if (cur_pos == SIZE_MAX || cur_consumed == depth) {
            cur_pos = next_pos();
            cur_consumed = 0;
            cur_v = (uint8_t)read_sample(
                f.data() + info.data_off + cur_pos * ((size_t)info.bits / 8), info.bits);
        }
        return (uint8_t)((cur_v >> cur_consumed++) & 1);
    }

    uint8_t read_byte() {
        uint8_t b = 0;
        for (int i = 0; i < 8; i++)
            if (next_bit()) b |= (uint8_t)(1u << (7 - i));
        return b;
    }
};

// 按（depth, newfmt）尝试解析：newfmt=带 depth 头字段；两者散布起点一致。
// 成功返回载荷（已认证）；失败抛异常。
std::vector<uint8_t> try_parse(const std::vector<uint8_t>& f, const WavInfo& info,
                               const std::string& password, uint32_t seed, int depth,
                               const std::string& write_to, std::string& out_name) {
    ScatterReader r(f, info, seed, depth);
    size_t total_bits = info.sample_count * (size_t)depth;
    if (total_bits < 64) throw std::runtime_error("no payload found in host wav");

    // 新格式：depth(1B) + name_len(2B BE)；旧格式（v1.0 无 depth 字段）由调用方回退
    if (depth >= 1) {
        uint8_t d0 = r.read_byte();
        if (d0 != (uint8_t)depth) throw std::runtime_error("no payload found in host wav");
    }
    uint16_t name_len = (uint16_t)(((uint16_t)r.read_byte() << 8) | r.read_byte());
    if (name_len == 0 || name_len > 512) throw std::runtime_error("no payload found in host wav");
    std::string name;
    for (size_t i = 0; i < name_len; i++) name.push_back((char)r.read_byte());
    uint32_t env_len = ((uint32_t)r.read_byte() << 24) | ((uint32_t)r.read_byte() << 16) |
                       ((uint32_t)r.read_byte() << 8) | r.read_byte();
    if (env_len == 0 || env_len > total_bits / 8)
        throw std::runtime_error("no payload found in host wav");
    std::vector<uint8_t> env(env_len);
    for (size_t i = 0; i < env_len; i++) env[i] = r.read_byte();

    // 解信封（密码错误抛异常；GCM 认证 = 载荷真实性校验）
    std::vector<uint8_t> payload = crypto_open(env, password);

    // 文件名清洗：仅保留 basename，防路径穿越
    out_name = file_basename(name);
    if (out_name.empty() || out_name == "." || out_name == "..")
        throw std::runtime_error("invalid payload filename");
    if (!write_to.empty()) write_file_bytes(write_to, payload);
    return payload;
}

} // namespace

// 读取宿主隐写流：新格式（首字节 = depth 1..3）原样返回 depth 首字节 + head+env；
// 旧格式 v1.0（无 depth 字段）与无载荷统一抛异常（分片合并只认新格式）。
std::vector<uint8_t> wav_read_stream(const std::string& host_path, const std::string& password) {
    std::vector<uint8_t> f = read_file_bytes(host_path);
    WavInfo info = parse_wav(f);
    uint32_t seed = crypto_steg_seed(password, xstr(kSeedTagXor, sizeof(kSeedTagXor)));

    for (int depth : {1, 2, 3}) {
        try {
            ScatterReader r(f, info, seed, depth);
            size_t total_bits = info.sample_count * (size_t)depth;
            if (total_bits < 64) throw std::runtime_error("no payload found in host wav");
            uint8_t d0 = r.read_byte();
            if (d0 != (uint8_t)depth) throw std::runtime_error("no payload found in host wav");
            uint16_t name_len = (uint16_t)(((uint16_t)r.read_byte() << 8) | r.read_byte());
            if (name_len == 0 || name_len > 512) throw std::runtime_error("no payload found in host wav");
            std::vector<uint8_t> name_b(name_len);
            for (size_t i = 0; i < name_len; i++) name_b[i] = r.read_byte();
            uint32_t env_len = ((uint32_t)r.read_byte() << 24) | ((uint32_t)r.read_byte() << 16) |
                               ((uint32_t)r.read_byte() << 8) | r.read_byte();
            if (env_len == 0 || env_len > total_bits / 8)
                throw std::runtime_error("no payload found in host wav");
            std::vector<uint8_t> env(env_len);
            for (size_t i = 0; i < env_len; i++) env[i] = r.read_byte();

            std::vector<uint8_t> stream;
            stream.reserve(1 + 2 + name_len + 4 + env_len);
            stream.push_back(d0);
            stream.push_back((uint8_t)(name_len >> 8));
            stream.push_back((uint8_t)name_len);
            stream.insert(stream.end(), name_b.begin(), name_b.end());
            stream.push_back((uint8_t)(env_len >> 24));
            stream.push_back((uint8_t)(env_len >> 16));
            stream.push_back((uint8_t)(env_len >> 8));
            stream.push_back((uint8_t)env_len);
            stream.insert(stream.end(), env.begin(), env.end());
            return stream;
        } catch (const std::runtime_error&) {
        }
    }
    throw std::runtime_error("no payload found in host wav");
}

size_t wav_capacity(const std::string& host_path, int depth) {
    if (depth < 1 || depth > 3) throw std::runtime_error("invalid depth (need 1..3)");
    std::vector<uint8_t> f = read_file_bytes(host_path);
    WavInfo info = parse_wav(f);
    return info.sample_count * (size_t)depth / 8;
}

// 解析宿主 WAV 的载荷：密码派生 seed → 散布重放（头+体同一散布流）→ crypto_open；
// 返回 (载荷, 原始文件名)；密码错误/无载荷/格式损坏抛异常。
// write_to 非空时把载荷写出到该路径（extract 用）；为空时只解析不落盘（检测用）
std::vector<uint8_t> wav_parse(const std::string& host_path, const std::string& password,
                               const std::string& write_to, std::string& out_name) {
    std::vector<uint8_t> f = read_file_bytes(host_path);
    WavInfo info = parse_wav(f);

    uint32_t seed = crypto_steg_seed(password, xstr(kSeedTagXor, sizeof(kSeedTagXor)));

    // 新格式优先（首字节 = 承载深度 1/2）；GCM 认证失败后回退旧格式（无 depth 字段，深度=1）
    std::string parse_name;
    for (int depth : {1, 2, 3}) {
        try {
            std::vector<uint8_t> payload = try_parse(f, info, password, seed, depth,
                                                     write_to, parse_name);
            out_name = parse_name;
            return payload;
        } catch (const std::runtime_error&) {
        }
    }
    // 旧格式回退：无 depth 字段（深度=1，首 2B 直接是 name_len）
    try {
        ScatterReader r(f, info, seed, 1);
        size_t total_bits = info.sample_count;
        if (total_bits < 64) throw std::runtime_error("no payload found in host wav");
        uint16_t name_len = (uint16_t)(((uint16_t)r.read_byte() << 8) | r.read_byte());
        if (name_len == 0 || name_len > 512) throw std::runtime_error("no payload found in host wav");
        std::string name;
        for (size_t i = 0; i < name_len; i++) name.push_back((char)r.read_byte());
        uint32_t env_len = ((uint32_t)r.read_byte() << 24) | ((uint32_t)r.read_byte() << 16) |
                           ((uint32_t)r.read_byte() << 8) | r.read_byte();
        if (env_len == 0 || env_len > total_bits / 8)
            throw std::runtime_error("no payload found in host wav");
        std::vector<uint8_t> env(env_len);
        for (size_t i = 0; i < env_len; i++) env[i] = r.read_byte();
        std::vector<uint8_t> payload = crypto_open(env, password);
        out_name = file_basename(name);
        if (out_name.empty() || out_name == "." || out_name == "..")
            throw std::runtime_error("invalid payload filename");
        if (!write_to.empty()) write_file_bytes(write_to, payload);
        return payload;
    } catch (const std::runtime_error& e) {
        (void)e;
    }
    throw std::runtime_error("no payload found in host wav");
}

bool wav_has_payload(const std::string& path, const std::string& password) {
    try {
        std::string name;
        wav_parse(path, password, "", name);
        return true;
    } catch (...) {
        return false;
    }
}

void wav_embed_stream(const std::string& host_path, const std::vector<uint8_t>& stream,
                      const std::string& password, const std::string& out_path, int fill_limit_pct,
                      int depth) {
    if (depth < 1 || depth > 3) throw std::runtime_error("invalid depth (need 1..3)");
    if (stream.empty() || stream[0] != (uint8_t)depth)
        throw std::runtime_error("invalid wav stream (depth header mismatch)");
    std::vector<uint8_t> f = read_file_bytes(host_path);
    if (f.empty()) throw std::runtime_error("host file is empty");
    WavInfo info = parse_wav(f);
    if (depth > 1 && info.bits != 16)
        throw std::runtime_error("depth > 1 requires 16-bit wav");

    size_t stream_bits = stream.size() * 8;
    size_t total_bits = info.sample_count * (size_t)depth;

    // 容量检查：先按填充率上限（抗统计检测），再按绝对容量
    if (fill_limit_pct > 0 &&
        stream_bits > (size_t)((uint64_t)total_bits * (uint64_t)fill_limit_pct / 100))
        throw std::runtime_error("payload too large: exceeds " + std::to_string(fill_limit_pct) +
                                 "% of host capacity (use --cap to raise the limit)");
    if (stream_bits > total_bits)
        throw std::runtime_error("payload too large for host wav capacity");

    // 散布种子由密码派生：无明文 seed 字段，位置随密码变化
    uint32_t seed = crypto_steg_seed(password, xstr(kSeedTagXor, sizeof(kSeedTagXor)));

    // 单遍嵌入：散布写位（depth=1 随机 ±1；depth=2/3 低 k bit 重写）
    std::vector<uint8_t> used(info.sample_count, 0);
    XorShift64 rng(seed);
    for (size_t i = 0; i < stream_bits;) {
        size_t pos = (size_t)(rng.next() % info.sample_count);
        while (used[pos]) pos = (pos + 1) % info.sample_count;
        used[pos] = 1;
        if (depth == 1) {
            set_sample_bit_rand(f.data() + info.data_off, info.bits, pos,
                                (uint8_t)get_bit(stream.data(), i));
            i++;
        } else {
            uint8_t val = 0;
            int k = 0;
            for (; k < depth && i < stream_bits; k++, i++)
                if (get_bit(stream.data(), i)) val |= (uint8_t)(1u << k);
            set_sample_lowbits(f.data() + info.data_off, info.bits, pos, k, val);
        }
    }

    // 写盘前最终自检：重放提取序列逐位验证（防任何不一致）
    {
        ScatterReader chk(f, info, seed, depth);
        for (size_t i = 0; i < stream_bits;) {
            uint8_t want = 0;
            int k = 0;
            for (; k < depth && i < stream_bits; k++, i++)
                if (get_bit(stream.data(), i)) want |= (uint8_t)(1u << k);
            uint8_t got = chk.read_bits(k);
            if (got != want) throw std::runtime_error("wav embed self-check failed: bit mismatch");
        }
    }

    write_file_bytes(out_path, f);
}

void wav_embed(const std::string& host_path, const std::string& payload_path,
               const std::string& password, const std::string& out_path, int fill_limit_pct,
               int depth) {
    if (depth < 1 || depth > 3) throw std::runtime_error("invalid depth (need 1..3)");
    std::vector<uint8_t> payload = read_file_bytes(payload_path);
    if (payload.empty()) throw std::runtime_error("payload file is empty");

    std::vector<uint8_t> env = crypto_seal(payload, password);
    std::string name = file_basename(payload_path);
    if (name.size() > 512) throw std::runtime_error("payload filename too long");

    // 位流（无魔数、无 seed 字段：头+体整体按密码派生种子散布）
    std::vector<uint8_t> stream;
    stream.push_back((uint8_t)depth);
    uint16_t nl = (uint16_t)name.size();
    stream.push_back((uint8_t)(nl >> 8));
    stream.push_back((uint8_t)nl);
    stream.insert(stream.end(), name.begin(), name.end());
    uint32_t el = (uint32_t)env.size();
    stream.push_back((uint8_t)(el >> 24));
    stream.push_back((uint8_t)(el >> 16));
    stream.push_back((uint8_t)(el >> 8));
    stream.push_back((uint8_t)el);
    stream.insert(stream.end(), env.begin(), env.end());
    wav_embed_stream(host_path, stream, password, out_path, fill_limit_pct, depth);
}

void wav_extract(const std::string& host_path, const std::string& password, const std::string& out_dir) {
    create_dirs(out_dir);
    std::string name;
    std::vector<uint8_t> payload = wav_parse(host_path, password, "", name);
    std::string out_path = out_dir.empty() ? name : out_dir + "/" + name;
    write_file_bytes(out_path, payload);
}
