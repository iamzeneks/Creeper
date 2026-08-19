// split_steg.cpp — 大文件拆分到多宿主实现（见 split_steg.h 协议说明）
#include "split_steg.h"

#include "crypto.h"
#include "file_util.h"
#include "mp3_steg.h"
#include "png_steg.h"
#include "wav_steg.h"

#include <cstring>
#include <map>
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
// "creeper-split"：分片块识别标识派生标签（密码派生 magic，无明文特征）
static const uint8_t kSplitTagXor[] = {0x36, 0x27, 0x30, 0x30, 0x25, 0x30, 0x27,
                                       0x78, 0x26, 0x25, 0x39, 0x3C, 0x21};

// 宿主类型（按扩展名分派）
enum class HostKind { PNG, WAV, MP3 };

std::string lower(const std::string& s) {
    std::string r = s;
    for (auto& c : r) c = (char)std::tolower((unsigned char)c);
    return r;
}

HostKind host_kind(const std::string& path) {
    std::string e = lower(path);
    size_t dot = e.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : e.substr(dot);
    if (ext == ".png") return HostKind::PNG;
    if (ext == ".wav") return HostKind::WAV;
    if (ext == ".mp3") return HostKind::MP3;
    throw std::runtime_error("unsupported host type (need .png/.wav/.mp3): " + path);
}

std::string host_ext(const std::string& path) {
    std::string e = lower(path);
    size_t dot = e.find_last_of('.');
    return dot == std::string::npos ? "" : e.substr(dot);
}

// 去掉路径与扩展名 → 纯主名
std::string strip_ext(const std::string& in) {
    std::string base = file_basename(in);
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    return base;
}

// 大端写/读
void put_u16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)(x & 0xFF));
}
void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x >> 24));
    v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)x);
}
uint16_t get_u16(const uint8_t* p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
uint32_t get_u32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// 宿主容量（100% 填充字节数）与 100% 位容量
size_t host_capacity(const std::string& path, HostKind kind, int depth) {
    switch (kind) {
        case HostKind::PNG: return png_capacity(path);
        case HostKind::WAV: return wav_capacity(path, depth);
        case HostKind::MP3: return mp3_capacity(path);
    }
    return 0;
}

// 读取宿主隐写流（head+env 原样字节；无载荷抛异常）
std::vector<uint8_t> host_read_stream(const std::string& path, HostKind kind,
                                      const std::string& password) {
    switch (kind) {
        case HostKind::PNG: return png_read_stream(path, password);
        case HostKind::WAV: return wav_read_stream(path, password);
        case HostKind::MP3: return mp3_read_stream(path, password);
    }
    return {};
}

// 嵌入隐写流到宿主
void host_embed_stream(const std::string& path, HostKind kind, const std::vector<uint8_t>& stream,
                       const std::string& password, const std::string& out_path,
                       int cap_pct, int depth) {
    switch (kind) {
        case HostKind::PNG: png_embed_stream(path, stream, password, out_path, cap_pct); break;
        case HostKind::WAV: wav_embed_stream(path, stream, password, out_path, cap_pct, depth); break;
        case HostKind::MP3: mp3_embed_stream(path, stream, password, out_path); break;
    }
}

// 解析隐写流 head → (name, env)。WAV 首字节为 depth（校验 1..3）。
void parse_stream_head(const std::vector<uint8_t>& stream, HostKind kind,
                       std::string& name, std::vector<uint8_t>& env) {
    size_t off = 0;
    if (kind == HostKind::WAV) {
        if (stream.size() < 1 + 6) throw std::runtime_error("malformed wav stream");
        if (stream[0] < 1 || stream[0] > 3) throw std::runtime_error("malformed wav stream");
        off = 1;
    }
    if (stream.size() < off + 6) throw std::runtime_error("malformed stream");
    uint16_t nl = get_u16(stream.data() + off);
    if (nl == 0 || nl > 512 || stream.size() < off + (size_t)6 + nl)
        throw std::runtime_error("malformed stream");
    name.assign((const char*)stream.data() + off + 2, nl);
    uint32_t el = get_u32(stream.data() + off + 2 + nl);
    if (el == 0 || stream.size() != off + (size_t)6 + nl + el)
        throw std::runtime_error("malformed stream");
    env.assign(stream.begin() + off + 6 + nl, stream.end());
}

} // namespace

void split_embed(const std::string& payload_path, const std::string& password,
                 const std::vector<std::string>& hosts, const std::string& out_dir,
                 int cap_pct, int depth) {
    if (hosts.empty()) throw std::runtime_error("no host files given");
    if (cap_pct < 0 || cap_pct > 100) throw std::runtime_error("invalid cap (need 0..100)");
    if (depth < 1 || depth > 3) throw std::runtime_error("invalid depth (need 1..3)");

    std::vector<uint8_t> payload = read_file_bytes(payload_path);
    if (payload.empty()) throw std::runtime_error("payload file is empty");
    std::string name = file_basename(payload_path);
    if (name.empty() || name.size() > 512) throw std::runtime_error("payload filename too long");

    std::vector<uint8_t> env = crypto_seal(payload, password);
    if (env.size() == 0) throw std::runtime_error("failed to seal payload");

    uint32_t magic = crypto_steg_seed(password, xstr(kSplitTagXor, sizeof(kSplitTagXor)));

    // 每宿主可用字节：floor(容量×填充率) − 隐写头开销 − 分片块头(12B)。
    // MP3 无填充率概念（辅助位方案不破坏音频数据），按 100% 计。
    const size_t head_overhead = 2 + name.size() + 4; // name_len + name + env_len
    const size_t wav_extra = 1;                       // depth 首字节
    const size_t block_head = 12;                     // magic + index + count + chunk_len
    std::vector<size_t> avail;
    std::vector<HostKind> kinds;
    size_t total_avail = 0;
    for (const auto& h : hosts) {
        HostKind k = host_kind(h);
        kinds.push_back(k);
        size_t cap = host_capacity(h, k, depth);
        int pct = (k == HostKind::MP3) ? 100 : cap_pct;
        size_t overhead = head_overhead + (k == HostKind::WAV ? wav_extra : 0) + block_head;
        size_t a = (size_t)((uint64_t)cap * (uint64_t)pct / 100);
        if (a <= overhead) {
            avail.push_back(0);
        } else {
            a -= overhead;
            avail.push_back(a);
        }
        total_avail += avail.back();
    }
    if (total_avail < env.size())
        throw std::runtime_error("payload too large for combined host capacity (need " +
                                 std::to_string(env.size()) + " bytes, have " +
                                 std::to_string(total_avail) + ")");

    // 顺序分配：从列表开头依次装，最后一块可能不满
    size_t remaining = env.size();
    size_t offset = 0;
    int count = 0;
    for (size_t i = 0; i < hosts.size() && remaining > 0; i++) {
        if (avail[i] == 0) continue;
        size_t n = (remaining < avail[i]) ? remaining : avail[i];
        remaining -= n;
        count++;
    }
    if (count == 0 || count > 65535)
        throw std::runtime_error("payload too large for combined host capacity");

    // 组装各块并嵌入
    if (!out_dir.empty()) create_dirs(out_dir);
    size_t used = 0;
    for (size_t i = 0, idx = 0; i < hosts.size() && idx < (size_t)count; i++) {
        if (avail[i] == 0) continue;
HostKind k = kinds[i];
        size_t chunk_len = avail[i];
        if (offset + chunk_len > env.size()) chunk_len = env.size() - offset;

        // 组装隐写流：head(name_len+name+env_len) + env(magic+index+count+chunk_len+chunk)
        std::vector<uint8_t> stream;
        if (k == HostKind::WAV) stream.push_back((uint8_t)depth);
        put_u16(stream, (uint16_t)name.size());
        stream.insert(stream.end(), name.begin(), name.end());
        uint32_t el = (uint32_t)(block_head + chunk_len);
        put_u32(stream, el);
        put_u32(stream, magic);
        put_u16(stream, (uint16_t)idx);
        put_u16(stream, (uint16_t)count);
        put_u32(stream, (uint32_t)chunk_len);
        stream.insert(stream.end(), env.begin() + offset, env.begin() + offset + chunk_len);
        offset += chunk_len;

        std::string out_path = out_dir.empty()
                                   ? strip_ext(hosts[i]) + "_已转换" + host_ext(hosts[i])
                                   : out_dir + "/" + strip_ext(hosts[i]) + "_已转换" + host_ext(hosts[i]);
        host_embed_stream(hosts[i], k, stream, password, out_path, cap_pct, depth);
        idx++;
        used++;
    }
    (void)used;
}

void split_extract(const std::vector<std::string>& hosts, const std::string& password,
                   const std::string& out_dir) {
    if (hosts.empty()) throw std::runtime_error("no host files given");
    uint32_t magic = crypto_steg_seed(password, xstr(kSplitTagXor, sizeof(kSplitTagXor)));

    std::map<int, std::vector<uint8_t>> blocks;
    int count = -1;
    std::string name;
    for (const auto& h : hosts) {
        HostKind k;
        try {
            k = host_kind(h);
        } catch (const std::exception&) {
            continue; // 非受支持宿主跳过
        }
        std::vector<uint8_t> stream;
        try {
            stream = host_read_stream(h, k, password);
        } catch (const std::exception&) {
            continue; // 无载荷/密码错/解析失败 → 非分片宿主跳过
        }
        std::string n;
        std::vector<uint8_t> env;
        try {
            parse_stream_head(stream, k, n, env);
        } catch (const std::exception&) {
            continue;
        }
        if (env.size() < 12) continue;
        if (get_u32(env.data()) != magic) continue; // 非分片块（普通载荷或垃圾）
        int index = (int)get_u16(env.data() + 4);
        int c = (int)get_u16(env.data() + 6);
        uint32_t chunk_len = get_u32(env.data() + 8);
        if (c == 0 || c > 65535 || index < 0 || index >= c) continue;
        if ((size_t)chunk_len != env.size() - 12) continue; // 块长不符 → 损坏/伪造
        if (count < 0) {
            count = c;
            name = n;
        } else if (count != c) {
            continue; // 总块数不一致的块跳过（非同一次 split）
        }
        if (blocks.count(index)) throw std::runtime_error("corrupted split data (duplicate block)");
        blocks[index] = std::vector<uint8_t>(env.begin() + 12, env.end());
    }

    if (count < 0) throw std::runtime_error("no payload found");
    if ((int)blocks.size() != count)
        throw std::runtime_error("missing split blocks (need " + std::to_string(count) +
                                 ", have " + std::to_string(blocks.size()) + ")");

    // 拼接 → 整体信封 → GCM 认证还原
    std::vector<uint8_t> env;
    for (int i = 0; i < count; i++) {
        auto it = blocks.find(i);
        if (it == blocks.end()) throw std::runtime_error("missing split block " + std::to_string(i));
        env.insert(env.end(), it->second.begin(), it->second.end());
    }
    std::vector<uint8_t> payload = crypto_open(env, password);

    // 文件名清洗：仅保留 basename，防路径穿越
    std::string out_name = file_basename(name);
    if (out_name.empty() || out_name == "." || out_name == "..")
        throw std::runtime_error("invalid payload filename");
    if (!out_dir.empty()) create_dirs(out_dir);
    std::string out_path = out_dir.empty() ? out_name : out_dir + "/" + out_name;
    write_file_bytes(out_path, payload);
}