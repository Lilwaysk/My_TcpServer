/*
 * randomHex(16) 用于生成 salt。
 * randomHex(32) 用于生成 token。
 * hashPassword 先按 hex 解码 salt，再计算 SHA-256(salt 原始字节 + password UTF-8 字节)。
 * 比较哈希使用 CRYPTO_memcmp 或等价实现。
 */
std::string randomHex(std::size_t bytes);
std::string sha256Hex(const std::strinng& data);
std::string hashPassword(const std::string& saltHex,
                         const std::string& password);
bool constantTimeEqual(const std::string& left,
                       const std::string& right);
