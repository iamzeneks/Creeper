// split_steg.h — 大文件拆分到多宿主（distributed steganography）
// 原理：payload 先整体 crypto_seal（一个 AES-GCM 信封，含 DEFLATE 压缩），再切分
// 密文片段分别嵌入多个宿主。单块无意义——任何单块都过不了 GCM 认证（提取端
// 视为"无载荷"），必须收齐所有块按序号拼接后才能认证还原（"单宿主截获无意义"）。
//
// 每个宿主的隐写流 = 标准隐写头（name_len+name+env_len，WAV 前多 1B 承载深度）+ env，
// 其中 env 为分片块：
//   magic     4B  密码派生分片标识 = crypto_steg_seed(password, "creeper-split")（大端）
//                （无明文特征；密码错则 magic 不匹配 → 按普通信封解析 → GCM 认证失败
//                  → 与"无载荷"不可区分）
//   index     2B  大端，块序号（0..count-1）
//   count     2B  大端，总块数（≤65535）
//   chunk_len 4B  大端，本块密文片段字节数
//   chunk     chunk_len 字节
//
// 提取端所有宿主文件顺序无关（按块内 index 拼接）。嵌入端列表最后一个文件 = 载荷，
// 其余为宿主（从列表开头依次使用，容量装完即止）。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

// 容量预检报告：need = 载荷压缩加密后的精确字节数（crypto_payload_size），
// have = 给定档位（cap_pct/depth）下所有宿主可容纳的总字节（与 split_embed 同一公式）。
struct SplitCapacity {
    size_t need = 0;
    size_t have = 0;
};

// 嵌入前容量预检（不写任何文件、不派生密钥）：载荷超档位容量时 GUI/调用方
// 可在转换开始前事先告知。have < need 即会嵌入失败。
SplitCapacity split_capacity_report(const std::string& payload_path,
                                    const std::vector<std::string>& hosts,
                                    int cap_pct, int depth);

// 拆分嵌入：payload_path 拆入 hosts（按序使用）→ out_dir 下输出 "宿主_已转换.<ext>"。
// cap_pct 应用于 PNG/WAV 每宿主填充率上限（%）；depth 仅 WAV 生效。容量不足抛异常。
void split_embed(const std::string& payload_path, const std::string& password,
                 const std::vector<std::string>& hosts, const std::string& out_dir,
                 int cap_pct, int depth);

// 合并提取：hosts 全部视为宿主（顺序无关，按块内序号拼接）→ 还原到 out_dir。
// 密码错误/缺块/数据损坏/无任何分片块抛异常。
void split_extract(const std::vector<std::string>& hosts, const std::string& password,
                   const std::string& out_dir);