"""creeper 加密信封：压缩 → AES-256-GCM → 带魔数的二进制信封

信封布局（固定 41 字节头）:
  magic     8B   b"CREEPER1"
  version   1B   0x01
  salt     16B   PBKDF2 盐（每次随机）
  nonce    12B   AES-GCM 随机 nonce
  ct_len    4B   大端，密文长度（含 16B GCM tag）
  ct      ct_len 密文

设计要点：
- 先 zlib 压缩再加密：最小化嵌入量（藏得越少越难被统计检测发现）
- PBKDF2-HMAC-SHA256 派生密钥：密码错误/数据被篡改 → GCM 认证失败直接报错
- GCM 自带完整性校验：载体被人动过手脚会立刻暴露
"""

import hashlib
import os
import struct
import zlib

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.pbkdf2 import PBKDF2HMAC

MAGIC = b"CREEPER1"
VERSION = 1
PBKDF2_ITER = 600_000  # OWASP 推荐量级；笔记本上约 0.3s
SALT_LEN = 16
NONCE_LEN = 12
TAG_LEN = 16
HEADER_LEN = len(MAGIC) + 1 + SALT_LEN + NONCE_LEN + 4


def _derive_key(password: bytes, salt: bytes) -> bytes:
    kdf = PBKDF2HMAC(
        algorithm=hashes.SHA256(),
        length=32,
        salt=salt,
        iterations=PBKDF2_ITER,
    )
    return kdf.derive(password)


def seal(payload: bytes, password: str) -> bytes:
    """任意字节 → 加密信封字节"""
    pwd = password.encode("utf-8")
    salt = os.urandom(SALT_LEN)
    nonce = os.urandom(NONCE_LEN)
    key = _derive_key(pwd, salt)

    compressed = zlib.compress(payload, level=9)
    ct = AESGCM(key).encrypt(nonce, compressed, None)

    header = MAGIC + bytes([VERSION]) + salt + nonce + struct.pack(">I", len(ct))
    return header + ct


def open_seal(envelope: bytes, password: str) -> bytes:
    """加密信封 → 原始字节；密码错误/被篡改抛 ValueError"""
    if len(envelope) < HEADER_LEN or envelope[: len(MAGIC)] != MAGIC:
        raise ValueError("不是 creeper 信封（魔数不匹配）")
    if envelope[len(MAGIC)] != VERSION:
        raise ValueError(f"不支持的版本: {envelope[len(MAGIC)]}")

    salt = envelope[len(MAGIC) + 1 : len(MAGIC) + 1 + SALT_LEN]
    nonce = envelope[len(MAGIC) + 1 + SALT_LEN : len(MAGIC) + 1 + SALT_LEN + NONCE_LEN]
    ct_len = struct.unpack(">I", envelope[HEADER_LEN - 4 : HEADER_LEN])[0]
    ct = envelope[HEADER_LEN : HEADER_LEN + ct_len]
    if len(ct) != ct_len:
        raise ValueError("信封截断")

    key = _derive_key(password.encode("utf-8"), salt)
    try:
        compressed = AESGCM(key).decrypt(nonce, ct, None)
    except Exception:
        raise ValueError("密码错误或数据被篡改（GCM 认证失败）")
    return zlib.decompress(compressed)


if __name__ == "__main__":
    # 自检
    import sys

    data = os.urandom(1024 * 64)
    env = seal(data, "test-pass")
    assert open_seal(env, "test-pass") == data
    try:
        open_seal(env, "wrong-pass")
        print("FAIL: 错误密码未拦截")
        sys.exit(1)
    except ValueError:
        pass
    tampered = env[:40] + bytes([env[40] ^ 0xFF]) + env[41:]
    try:
        open_seal(tampered, "test-pass")
        print("FAIL: 篡改未拦截")
        sys.exit(1)
    except ValueError:
        pass
    print(f"OK: envelope 自检通过（64KB 载荷，信封 {len(env)} 字节）")
