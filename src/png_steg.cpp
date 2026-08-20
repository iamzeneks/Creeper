// png_steg.cpp — PNG 隐写实现（±1 嵌入 + 直方图配对补偿，无魔数）
// 位流顺序：像素行主序，每像素 R→G→B；字节内 MSB 优先（bit0 = 字节最高位）。
// 本文件是唯一定义 STB_IMAGE/STB_IMAGE_WRITE 实现单元的翻译单元。
#ifdef _WIN32
#define _CRT_RAND_S // 启用 rand_s（CRT 级安全随机，导入表不含 bcrypt）
#include <windows.h>
#endif
#include "png_steg.h"

#include "crypto.h"
#include "file_util.h"

#include <cstring>
#include <cstdlib>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

namespace {

// XOR 0x55 混淆常量还原（运行时还原，exe 内无明文）
std::string xstr(const uint8_t* x, size_t n) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; i++) s[i] = (char)(x[i] ^ 0x55);
    return s;
}
// "creeper-seed"：散布种子派生标签（随密码变化，消除明文固定位置）
static const uint8_t kSeedTagXor[] = {0x36, 0x27, 0x30, 0x30, 0x25, 0x30, 0x27, 0x78, 0x26, 0x30, 0x30, 0x31};

// xorshift64：与规格一致的确定性伪随机（seed 为 0 时使用固定常数）
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

// 位位置 → 像素通道值：pos/3 = 像素，pos%3 = 通道（0=R 1=G 2=B）
inline uint8_t pixel_bit(const uint8_t* img, int req, size_t pos) {
    return img[(pos / 3) * req + (pos % 3)] & 1u;
}

// ---------- ±1 嵌入（LSB matching） + 直方图配对补偿 ----------
// 需要改位的像素 ±1（方向随机，边界单方向）而不是强制 LSB：避免相邻值频率相等
// （直方图阶跃）特征。嵌入完成后，用"未承载数据位"的像素做反向 ±1 移动，
// 把修改后直方图拉回原图形状（直方图配对平衡）；提取端只读承载位，不受补偿影响。

// 高频随机（±1 方向等）：一次性从系统 CSPRNG 播种的快速 PRNG（mt19937_64）。
// 单次播种（函数内 static 保证线程安全），避免逐像素构造 random_device 的开销。
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

static uint64_t HistRand() {
    return FastRand64();
}

// 随机 ±1 嵌入一位（LSB 相同不动）；返回是否实际修改
inline bool set_pixel_bit_rand(uint8_t* img, int req, size_t pos, uint8_t bit) {
    uint8_t& px = img[(pos / 3) * req + (pos % 3)];
    if ((px & 1u) == bit) return false;
    int d = (HistRand() & 1) ? -1 : 1;
    if (px == 0) d = 1;
    if (px == 255) d = -1;
    px = (uint8_t)((int)px + d);
    return true;
}

// 统计像素级"是否承载数据位"（像素任一通道被占用即锁定）
void mark_pixel_used(std::vector<uint8_t>& pixel_used, const std::vector<uint8_t>& used) {
    for (size_t i = 0; i < used.size(); i++) {
        if (used[i]) pixel_used[i / 3] = 1;
    }
}

// 直方图配对补偿：对未承载像素，把"过剩 bin"的值移向"亏缺 bin"（±1），
// 每步使直方图更接近原图；返回值 = 实际移动的像素数。
// undo 记录每次移动的 (像素索引, 原 RGB)，供自对抗重嵌时恢复原图
struct UndoEntry {
    uint32_t idx;
    uint8_t r, g, b;
};
size_t histogram_pair_compensate(uint8_t* img, int w, int h, int req,
                                 const std::vector<uint8_t>& pixel_used, const int* H,
                                 std::vector<UndoEntry>& undo) {
    int Hc[256];
    std::memset(Hc, 0, sizeof(Hc));
    const uint8_t* pc = img;
    for (int i = 0; i < w * h; i++, pc += req) {
        Hc[pc[0]]++;
        Hc[pc[1]]++;
        Hc[pc[2]]++;
    }
    size_t moved = 0;
    uint8_t* p = img;
    for (int i = 0; i < w * h; i++, p += req) {
        if (pixel_used[(size_t)i]) continue; // 承载像素不可动
        for (int c = 0; c < 3; c++) {
            int v = p[c];
            if (Hc[v] <= H[v]) continue; // 该值未过剩
            int best_d = 0;
            int best_need = 0x7FFFFFFF; // w 的亏缺量（>0 才好）
            for (int d = -1; d <= 1; d += 2) {
                int w2 = v + d;
                if (w2 < 0 || w2 > 255) continue;
                int need = H[w2] - Hc[w2];
                if (need < best_need) {
                    best_need = need;
                    best_d = d;
                } else if (need == best_need && (HistRand() & 1)) {
                    best_d = d;
                }
            }
            if (best_d == 0 || best_need <= 0) continue; // 无亏缺可补
            {
                uint8_t* q = img + (size_t)i * req;
                undo.push_back({(uint32_t)i, q[0], q[1], q[2]});
            }
            Hc[v]--;
            Hc[v + best_d]++;
            p[c] = (uint8_t)(v + best_d);
            moved++;
        }
    }
    return moved;
}

// 解码图片：原图带 alpha（4 通道）→ 按 RGBA 载入（alpha 保持原样）；否则统一转 RGB
unsigned char* load_image(const std::string& path, int& w, int& h, int& req) {
    std::vector<uint8_t> bytes = read_file_bytes(path);
    int comp = 0;
    if (!stbi_info_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &comp)) {
        throw std::runtime_error("cannot decode image: " + path);
    }
    req = (comp == 4) ? 4 : 3;
    unsigned char* img = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &comp, req);
    if (!img) throw std::runtime_error("cannot decode image: " + path);
    return img;
}

// 取字节流中第 bit_idx 位（MSB 优先）
inline uint8_t get_bit(const uint8_t* bytes, size_t bit_idx) {
    return (bytes[bit_idx >> 3] >> (7 - (bit_idx & 7))) & 1u;
}

// stbi_write 回调：写入内存缓冲
struct WriteCtx {
    std::vector<uint8_t>* buf;
};
void write_cb(void* context, void* data, int size) {
    WriteCtx* ctx = static_cast<WriteCtx*>(context);
    const uint8_t* p = static_cast<const uint8_t*>(data);
    ctx->buf->insert(ctx->buf->end(), p, p + size);
}

} // namespace

// 解析宿主 PNG 的载荷：密码派生 seed → 散布重放（头+体同一散布流）→ crypto_open；
// 返回 (载荷, 原始文件名)；密码错误/无载荷/格式损坏抛异常。
// write_to 非空时把载荷写出到该路径（extract 用）；为空时只解析不落盘（检测用）
std::vector<uint8_t> png_read_stream(const std::string& host_path, const std::string& password) {
    int w = 0, h = 0, req = 0;
    unsigned char* img = load_image(host_path, w, h, req);
    try {
        size_t total_bits = (size_t)w * h * 3;
        if (total_bits < 64) throw std::runtime_error("no payload found in host image");

        // 散布读取器：seed 由密码派生（无明文 seed 字段），同序重放；
        // 位置序列纯内容无关（密码派生 xorshift + 线性探测）→ 嵌入后图可精确重放
        uint32_t seed = crypto_steg_seed(password, xstr(kSeedTagXor, sizeof(kSeedTagXor)));
        XorShift64 rng(seed);
        std::vector<uint8_t> used(total_bits, 0);
        auto next_pos = [&]() {
            size_t p = (size_t)(rng.next() % total_bits);
            while (used[p]) p = (p + 1) % total_bits;
            used[p] = 1;
            return p;
        };
        auto read_bytes = [&](size_t nbytes) {
            std::vector<uint8_t> out(nbytes, 0);
            for (size_t i = 0; i < nbytes * 8; i++) {
                if (pixel_bit(img, req, next_pos())) out[i >> 3] |= (uint8_t)(1u << (7 - (i & 7)));
            }
            return out;
        };

        std::vector<uint8_t> nl_b = read_bytes(2);
        uint16_t name_len = (uint16_t)(((uint16_t)nl_b[0] << 8) | nl_b[1]);
        if (name_len == 0 || name_len > 512) throw std::runtime_error("no payload found in host image");
        std::vector<uint8_t> name_b = read_bytes(name_len);
        std::vector<uint8_t> el_b = read_bytes(4);
        uint32_t env_len = ((uint32_t)el_b[0] << 24) | ((uint32_t)el_b[1] << 16) |
                           ((uint32_t)el_b[2] << 8) | (uint32_t)el_b[3];
        if (env_len == 0 || env_len > total_bits / 8)
            throw std::runtime_error("no payload found in host image");
        std::vector<uint8_t> env = read_bytes(env_len);

        // 拼回原样流（head+env）：调用方自行解析（分片合并按原样收集）
        std::vector<uint8_t> stream;
        stream.reserve(2 + name_len + 4 + env_len);
        stream.insert(stream.end(), nl_b.begin(), nl_b.end());
        stream.insert(stream.end(), name_b.begin(), name_b.end());
        stream.insert(stream.end(), el_b.begin(), el_b.end());
        stream.insert(stream.end(), env.begin(), env.end());
        return stream;
    } catch (...) {
        stbi_image_free(img);
        throw;
    }
    stbi_image_free(img);
}

// head+env 拼接流 → 解信封认证；write_to 非空时落盘
std::vector<uint8_t> png_parse_stream(const std::vector<uint8_t>& stream,
                                      const std::string& password,
                                      const std::string& write_to, std::string& out_name) {
    if (stream.size() < 6) throw std::runtime_error("no payload found in host image");
    uint16_t name_len = (uint16_t)(((uint16_t)stream[0] << 8) | stream[1]);
    if (name_len == 0 || name_len > 512 || stream.size() < (size_t)6 + name_len)
        throw std::runtime_error("no payload found in host image");
    uint32_t env_len = ((uint32_t)stream[2 + name_len] << 24) | ((uint32_t)stream[3 + name_len] << 16) |
                       ((uint32_t)stream[4 + name_len] << 8) | (uint32_t)stream[5 + name_len];
    if (env_len == 0 || stream.size() != (size_t)6 + name_len + env_len)
        throw std::runtime_error("no payload found in host image");
    std::string name((const char*)stream.data() + 2, name_len);
    std::vector<uint8_t> env(stream.begin() + 6 + name_len, stream.end());

    // 解信封（密码错误抛异常；GCM 认证 = 载荷真实性校验）
    std::vector<uint8_t> payload = crypto_open(env, password);

    // 文件名清洗：仅保留 basename，防路径穿越
    out_name = file_basename(name);
    if (out_name.empty() || out_name == "." || out_name == "..")
        throw std::runtime_error("invalid payload filename");
    if (!write_to.empty()) write_file_bytes(write_to, payload);
    return payload;
}

std::vector<uint8_t> png_parse(const std::string& host_path, const std::string& password,
                               const std::string& write_to, std::string& out_name) {
    std::vector<uint8_t> stream = png_read_stream(host_path, password);
    return png_parse_stream(stream, password, write_to, out_name);
}

size_t png_capacity(const std::string& host_path) {
    int w = 0, h = 0, req = 0;
    unsigned char* img = load_image(host_path, w, h, req);
    stbi_image_free(img);
    return (size_t)w * h * 3 / 8;
}

bool png_has_payload(const std::string& path, const std::string& password) {
    try {
        std::string name;
        png_parse(path, password, "", name);
        return true;
    } catch (...) {
        return false;
    }
}

void png_embed_stream(const std::string& host_path, const std::vector<uint8_t>& stream,
                      const std::string& password, const std::string& out_path, int fill_limit_pct) {
    int w = 0, h = 0, req = 0;
    unsigned char* img = load_image(host_path, w, h, req);
    try {
        size_t total_bits = (size_t)w * h * 3;
        size_t stream_bits = stream.size() * 8;
        if (stream_bits == 0) throw std::runtime_error("payload stream is empty");

        // 容量检查：先按填充率上限（抗统计检测），再按绝对容量
        if (fill_limit_pct > 0 &&
            stream_bits > (size_t)((uint64_t)total_bits * (uint64_t)fill_limit_pct / 100))
            throw std::runtime_error("payload too large: exceeds " + std::to_string(fill_limit_pct) +
                                     "% of host capacity (use --cap to raise the limit)");
        if (stream_bits > total_bits)
            throw std::runtime_error("payload too large for host image capacity");

        // 原图直方图（RGB 汇总，用于配对补偿与自对抗验证）
        int H[256];
        std::memset(H, 0, sizeof(H));
        {
            const uint8_t* p = img;
            for (int i = 0; i < w * h; i++, p += req) {
                H[p[0]]++;
                H[p[1]]++;
                H[p[2]]++;
            }
        }

        // 散布种子由密码派生：无明文 seed 字段，位置随密码变化。
        // 位置序列纯内容无关（xorshift + 线性探测），任何像素至多被改一次（±1，
        // LSB 幂等）→ 嵌入后图可精确重放同一序列，提取端无需任何辅助信息。
        uint32_t seed = crypto_steg_seed(password, xstr(kSeedTagXor, sizeof(kSeedTagXor)));

        // 修改快照（供自对抗重嵌时恢复原图；记录量 ≈ 修改像素数，远小于整图。
        // 不可设硬上限——截断会导致重嵌时恢复不全、残留脏像素）
        std::vector<UndoEntry> undo;
        undo.reserve(stream_bits + (size_t)w * h / 2);

        // 单遍嵌入 pass：散布写位（随机 ±1）→ 配对补偿 → 返回统计
        auto embed_pass = [&](size_t& n_changed, size_t& l1) {
            std::vector<uint8_t> used(total_bits, 0);
            XorShift64 rng(seed);
            for (size_t i = 0; i < stream_bits; i++) {
                size_t pos = (size_t)(rng.next() % total_bits);
                while (used[pos]) pos = (pos + 1) % total_bits;
                used[pos] = 1;
                const uint8_t* q = img + (size_t)(pos / 3) * req;
                undo.push_back({(uint32_t)(pos / 3), q[0], q[1], q[2]});
                if (set_pixel_bit_rand(img, req, pos, get_bit(stream.data(), i))) n_changed++;
            }
            std::vector<uint8_t> pixel_used((size_t)w * h, 0);
            mark_pixel_used(pixel_used, used);
            n_changed += histogram_pair_compensate(img, w, h, req, pixel_used, H, undo);
            // 直方图 L1（RGB 汇总）与修改量对比
            int Hc[256];
            std::memset(Hc, 0, sizeof(Hc));
            const uint8_t* p = img;
            for (int i = 0; i < w * h; i++, p += req) {
                Hc[p[0]]++;
                Hc[p[1]]++;
                Hc[p[2]]++;
            }
            l1 = 0;
            for (int v = 0; v < 256; v++) l1 += (size_t)std::abs(Hc[v] - H[v]);
        };

        // 自对抗验证：直方图偏移过大（> 修改量的 15%）→ 恢复原图重嵌（最多 3 次）
        const int kMaxAttempts = 3;
        size_t n_changed = 0, l1 = 0;
        int attempt = 0;
        for (;;) {
            undo.clear();
            n_changed = l1 = 0;
            embed_pass(n_changed, l1);
            if (l1 * 100 <= n_changed * 15 || ++attempt >= kMaxAttempts) break;
            for (const auto& e : undo) {
                uint8_t* q = img + (size_t)e.idx * req;
                q[0] = e.r;
                q[1] = e.g;
                q[2] = e.b;
            }
        }

        // 写盘前最终自检：重放提取序列逐位验证（防任何不一致）
        {
            std::vector<uint8_t> chk(total_bits, 0);
            XorShift64 rng(seed);
            bool ok = true;
            for (size_t i = 0; i < stream_bits; i++) {
                size_t pos = (size_t)(rng.next() % total_bits);
                while (chk[pos]) pos = (pos + 1) % total_bits;
                chk[pos] = 1;
                if (pixel_bit(img, req, pos) != get_bit(stream.data(), i)) { ok = false; break; }
            }
            if (!ok) throw std::runtime_error("png embed self-check failed: bit mismatch");
        }

        // 无损写回 PNG
        std::vector<uint8_t> out_buf;
        WriteCtx ctx{&out_buf};
        if (!stbi_write_png_to_func(write_cb, &ctx, w, h, req, img, w * req))
            throw std::runtime_error("failed to write png output");
        write_file_bytes(out_path, out_buf);
    } catch (...) {
        stbi_image_free(img);
        throw;
    }
    stbi_image_free(img);
}

void png_embed(const std::string& host_path, const std::string& payload_path,
               const std::string& password, const std::string& out_path, int fill_limit_pct) {
    std::vector<uint8_t> payload = read_file_bytes(payload_path);
    if (payload.empty()) throw std::runtime_error("payload file is empty");
    std::string name = file_basename(payload_path);
    if (name.size() > 512) throw std::runtime_error("payload filename too long");
    std::vector<uint8_t> env = crypto_seal(payload, password);

    // 组装位流（无魔数、无 seed 字段：头+体整体按密码派生种子散布）
    std::vector<uint8_t> stream;
    uint16_t nl = (uint16_t)name.size();
    stream.push_back((uint8_t)(nl >> 8));
    stream.push_back((uint8_t)(nl & 0xFF));
    stream.insert(stream.end(), name.begin(), name.end());
    uint32_t el = (uint32_t)env.size();
    stream.push_back((uint8_t)(el >> 24));
    stream.push_back((uint8_t)(el >> 16));
    stream.push_back((uint8_t)(el >> 8));
    stream.push_back((uint8_t)el);
    stream.insert(stream.end(), env.begin(), env.end());
    png_embed_stream(host_path, stream, password, out_path, fill_limit_pct);
}

void png_extract(const std::string& host_path, const std::string& password, const std::string& out_dir) {
    create_dirs(out_dir);
    std::string name;
    std::vector<uint8_t> payload = png_parse(host_path, password, "", name);
    std::string out_path = out_dir.empty() ? name : out_dir + "/" + name;
    write_file_bytes(out_path, payload);
}
