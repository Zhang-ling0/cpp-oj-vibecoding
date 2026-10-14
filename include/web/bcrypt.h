// =============================================================================
// CppOJ 密码哈希 —— 【自实现 bcrypt】，底层原语取自 OpenSSL libcrypto 的 Blowfish
// 对应任务：SPEC.md §9 M3 T3.3「认证：自实现 bcrypt（libssl-dev Blowfish，D-75）」
// 规格来源：§6.2.1（4 条实现要点）/ §8 R-20（base64 变体是最易错点）/ AC-49
//           §3.2 DDL（users.password_hash VARCHAR(255)，bcrypt 输出 60 字符，不需要改 DDL）
//
// 【为什么是【自实现】而不是调一个现成库】[D-75]
//   Ubuntu 24.04 官方源没有 libbcrypt-dev（`apt-cache policy` 无 Candidate），
//   glibc 的 crypt() 不保证算法是 bcrypt，而 OpenSSL 只提供 Blowfish 底层原语 ——
//   没有 bcrypt 的 EksBlowfishSetup / 盐值编码 / 自己的 base64 变体。
//   这三样加起来就是本文件的全部内容（约 300 行），其余全部复用 OpenSSL：
//     · Blowfish 的 16 轮 encipher（BF_encrypt）与 P/S 状态结构 → libcrypto
//     · 随机盐（RAND_bytes 16 字节）                                → libcrypto
//     · 定长比较（CRYPTO_memcmp）                                   → libcrypto
//   唯一"自己抄"的是 Blowfish 的初始状态（P 与 S 共 1042 个字面量 = π 的十六进制
//   小数位）—— 那是 Blowfish 算法规范的一部分，不是密码学秘密。
//
// 【本文件最容易写错、且错了【不会崩】的三处】（R-20 的三条）
//   ① base64 字母表是 `./A-Za-z0-9`，不是标准 base64 的 `A-Za-z0-9+/`。
//      写错时密码校验会【全线失败或全被绕过】，而程序不会崩 —— 所以 AC-49 要求
//      拿参考实现（RFC 6234 / Provos 的官方向量）逐字节比对，本文件下的
//      `hash_password_with_salt()` 就是为这件事留的测试入口。
//   ② 密钥长度取 **strlen**（到第一个 NUL 为止），不是 std::string::size()。
//      参考实现（Cyrpt_blowfish / OpenBSD / libxcrypt）一律用 strlen，于是
//      口令里的 NUL 之后的内容【不参与哈希】。用 size() 会与所有其他实现算出不同结果。
//   ③ 输出的 31 个哈希字符来自对【24 字节】密文的 base64（32 字符）再截掉最后一个字符。
//      不是"对前 23 字节做 base64"—— 两者的最后一个字符相差 2 个比特，
//      而它恰恰就是 hash 的最后一个字符。
//
// 【本模块不碰 DB、不依赖 config，可独立单测】
//   cost 由调用方给（auth_api 传 §6.2.1 要点 3 定的 10）。这里【没有第二处
//   默认 cost 常量】以外的东西，也没有口令强度策略 —— 那是 auth_api 的职责
//   （§5.8 的 6–64 位），两者分开是为了让密码学原语与业务规则各自可测。
// =============================================================================
#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace oj {
namespace web {

// §6.2.1 要点 3：cost=10（1967MB 机器上单次约 60~100ms，与改密端点在全局 DB mutex
// 内的耗时一起 < 200ms，可接受）。这是【唯一】的默认 cost；auth_api 不另设常量。
inline constexpr int kBcryptDefaultCost = 10;
// 算法本身的合法区间（$2b$ 的 cost 字段是 2 位十进制）。下界取 4 是 bcrypt 规范里的
// 合法最小值 —— 单测用 4（否则每条用例几十毫秒），生产取 10。
inline constexpr int kBcryptMinCost = 4;
inline constexpr int kBcryptMaxCost = 31;

inline constexpr std::size_t kBcryptSaltBytes = 16;    // 原始盐字节数
inline constexpr std::size_t kBcryptSaltChars = 22;    // 盐在哈希串里的字符数
inline constexpr std::size_t kBcryptDigestChars = 31;  // 哈希部分固定 31 字符
inline constexpr std::size_t kBcryptHashLength = 60;   // $2b$ + 10$ + 22 + 31
// bcrypt 只用密钥流的前 72 字节（18 个 P 字 × 4 字节）。§5.8 把口令上限定在 64 字节，
// 正是为了让这个截断点【永远不发生】—— 少一位哈希的密码等于密码里少一位。
inline constexpr std::size_t kBcryptMaxKeyBytes = 72;

// 完整形状的哈希串：`$2b$` + 2 位 cost + `$` + 22 位盐 + 31 位哈希。
// 前缀取 `$2b$`：它是 2007 年之后唯一的正确变体（修掉了 $2a$ 对 ≥256 字节口令的
// 符号扩展问题，并统一了"72 字节截断 + 含结尾 NUL"的密钥长度规则）。
inline constexpr const char* kBcryptPrefix = "$2b$";

// 校验一个哈希串的形状：长度 60、`$2[aby]$NN$` + 22 位合法盐字符 + 31 位合法摘要字符。
// ⚠️ 【只查形状，不查成本】—— 形状非法的哈希一律【不可信】（可能被塞进超长串或
//   cost=99 的串来拖垮 CPU），verify() 据此直接返回 false 而不去计算。
bool is_hash_wellformed(const std::string& stored);

// 取出哈希串里的 cost；形状非法时返回 nullopt。
std::optional<int> cost_of_hash(const std::string& stored);

// 哈希口令，盐由 RAND_bytes 生成。
// cost 不在 [4,31] 抛 std::invalid_argument；熵源不可用抛 std::runtime_error
// （与 session::generate_token 同一判断：悄悄退化成可预测的盐比直接失败危险得多）。
std::string hash_password(const std::string& password, int cost = kBcryptDefaultCost);

// 【单测专用】用给定盐哈希口令。salt_b64 必须是 22 个合法 bcrypt base64 字符
// （即从某个哈希串里抄下来的那段），否则抛 std::invalid_argument。
// 存在的唯一理由：AC-49 要求与参考实现【逐字节】比对，而参考实现接受固定盐。
// 生产路径不调用它 —— 生产永远走 hash_password() 的随机盐。
std::string hash_password_with_salt(const std::string& password, int cost,
                                    const std::string& salt_b64);

// 校验口令。**形状非法的 stored 返回 false 而不是抛异常** —— 它来自 DB，
// 是一条数据而不是一段代码，坏数据不该让请求崩掉。
// 比较用 CRYPTO_memcmp（定长、与短路无关）。
bool verify_password(const std::string& password, const std::string& stored);

}  // namespace web
}  // namespace oj
