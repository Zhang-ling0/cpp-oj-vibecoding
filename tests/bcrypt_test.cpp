// =============================================================================
// 自实现 bcrypt 的单测（AC-49 / §8 R-20）
// 规格来源：SPEC.md §6.2.1 要点 2（必须带测试向量）、§10.1 AC-49、§8 R-20
//
// 【这一组用例存在的唯一理由】
//   自实现密码哈希的失败方式是【静默的】："写错了但能编译、能跑、注册也成功，
//   只是所有口令都验不过（或全被绕过）"。R-20 因此要求：实现前先用官方向量
//   与参考实现【逐字节】比对，且明确写了"不得用『注册没报错』当作哈希正确的证据"。
//   本文件就是那个证据 —— 期望值全部来自外部参考实现（libxcrypt / pyca-bcrypt），
//   任何一处偏差都会让本文件变红。
//
// 【向量从哪来、覆盖什么】
//   第 1~4 条是公开流传最广的 bcrypt 官方向量（空口令、单字符、`U*U`、`U*U*`），
//   几乎所有实现都拿它们自测；其余覆盖 cost 4/5/6/10、非 ASCII、>72 字节截断。
//   三个本轮踩到并因此固化进断言的细节：
//     ① 每个 encipher 消耗【两个】盐字（分别异或到 l 与 r），不是 l/r 交替一个；
//     ② 摘要编码的是 24 字节密文的【前 23 字节】，不是"编码 24 字节再截掉最后一字符"
//        —— 两者差在最后一个字符的 2 个比特上；
//     ③ 密钥流长度取 strlen + 1（结尾 NUL 参与），且口令里的 NUL 会【截断】口令。
//   ①② 若写错，全部向量同时变红且症状一致（哈希与所有外部工具都不匹配）；
//   ③ 只影响含 NUL 或超长口令，用例 NulTruncatesAtTheFirstNulByte 与
//   LongerThan72BytesCollapsesToTheSameHash 单独把它钉住。
// =============================================================================
#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include "web/bcrypt.h"

namespace oj {
namespace web {
namespace {

// 参考向量：口令 / 22 位盐 / cost / 期望哈希（60 字符）
struct ReferenceVector {
  const char* password;
  const char* salt;
  int cost;
  const char* expected;
};

constexpr ReferenceVector kReferenceVectors[] = {
    {"", "DCq7YPn5Rq63x1Lad4cll.", 6,
     "$2b$06$DCq7YPn5Rq63x1Lad4cll.TV4S6ytwfsfvkgY8jIucDrjc8deX1s."},
    {"abc", "If6bvum7DFjUnE9p2uDeDu", 6,
     "$2b$06$If6bvum7DFjUnE9p2uDeDu0YHzrHM6tf.iqN8.yx.jNN1ILEf7h0i"},
    {"a", "m0CrhHm10qJ3lXRY.5zDGO", 6,
     "$2b$06$m0CrhHm10qJ3lXRY.5zDGO3rS2KdeeWLuGmsfGlMfOxih58VYVfxe"},
    {"U*U", "CCCCCCCCCCCCCCCCCCCCC.", 5,
     "$2b$05$CCCCCCCCCCCCCCCCCCCCC.E5YPO9kmyuRGyh0XouQYb4YMJKvyOeW"},
    {"U*U*", "XXXXXXXXXXXXXXXXXXXXXO", 5,
     "$2b$05$XXXXXXXXXXXXXXXXXXXXXOEA5zdESHGZg1el8MmIQBrOadA9Lngi6"},
    {"abcdefghijklmnopqrstuvwxyz", "If6bvum7DFjUnE9p2uDeDu", 6,
     "$2b$06$If6bvum7DFjUnE9p2uDeDugSm/r4ClComu5mWRI4RIK8QxNtrr8pm"},
    {"~!@#$%^&*()      ~!@#$%^&*()PNBFRD", "If6bvum7DFjUnE9p2uDeDu", 6,
     "$2b$06$If6bvum7DFjUnE9p2uDeDujh/PYDxJ353ABfbCEzNdJK32eAvYbTq"},
    {"correct horse battery staple", "abcdefghijklmnopqrstuu", 4,
     "$2b$04$abcdefghijklmnopqrstuu7EJV7kdjBBQxyb0HjTh9KS7.Lah/6CG"},
    {"\xe9\x9d\xa2\xe8\xaf\x95\xe5\x87\x86\xe5\xa4\x87""2026",  // 面试准备2026（非 ASCII）
     "abcdefghijklmnopqrstuu", 4,
     "$2b$04$abcdefghijklmnopqrstuu8DBFgnMIYqekEftL3mu7BbXi8h7g64a"},
    {"pass", "DCq7YPn5Rq63x1Lad4cll.", 10,   // 生产默认 cost=10 的形状
     "$2b$10$DCq7YPn5Rq63x1Lad4cll.pF1QU3GHhtPZhUztFkfH2zDajWxe16C"},
};

}  // namespace

// ---- AC-49：与参考实现逐字节一致 ------------------------------------------------

TEST(BcryptTest, MatchesReferenceImplementationByteForByte) {
  for (const ReferenceVector& v : kReferenceVectors) {
    const std::string actual = hash_password_with_salt(v.password, v.cost, v.salt);
    EXPECT_EQ(actual, v.expected) << "password=\"" << v.password << "\" cost=" << v.cost;
    EXPECT_EQ(actual.size(), kBcryptHashLength);
  }
}

// 官方向量里最短的那个（空口令）单独再钉一次：它最容易被"空口令当成特例"写错
TEST(BcryptTest, EmptyPasswordMatchesPublishedVector) {
  EXPECT_EQ(hash_password_with_salt("", 6, "DCq7YPn5Rq63x1Lad4cll."),
            "$2b$06$DCq7YPn5Rq63x1Lad4cll.TV4S6ytwfsfvkgY8jIucDrjc8deX1s.");
}

// $2a$ / $2b$ / $2y$ 三个变体对同一口令与盐产出【相同摘要】（只有前缀不同）——
// 本项目只写 $2b$，但必须能验历史遗留的 $2a$ 行。
TEST(BcryptTest, VerifyAcceptsTheLegacyMinorPrefixes) {
  const std::string canonical =
      "$2a$06$If6bvum7DFjUnE9p2uDeDu0YHzrHM6tf.iqN8.yx.jNN1ILEf7h0i";
  EXPECT_TRUE(verify_password("abc", canonical));
  EXPECT_FALSE(verify_password("abd", canonical));
  EXPECT_EQ(hash_password_with_salt("abc", 6, "If6bvum7DFjUnE9p2uDeDu").substr(7), canonical.substr(7));
}

// ---- 密钥流长度：strlen + 1，且 NUL 截断（§6.2.1 要点 2 / R-20 ③）--------------

TEST(BcryptTest, LongerThan72BytesCollapsesToTheSameHash) {
  const std::string seventy_two(72, 'A');
  const std::string eighty(80, 'A');
  EXPECT_EQ(hash_password_with_salt(seventy_two, 4, "DCq7YPn5Rq63x1Lad4cll."),
            hash_password_with_salt(eighty, 4, "DCq7YPn5Rq63x1Lad4cll."));
  // 71 字节与 72 字节【必须】不同 —— 否则说明截断点被写成了 71 或更小
  EXPECT_NE(hash_password_with_salt(std::string(71, 'A'), 4, "DCq7YPn5Rq63x1Lad4cll."),
            hash_password_with_salt(seventy_two, 4, "DCq7YPn5Rq63x1Lad4cll."));
}

// 口令里的 NUL 之后的内容不参与哈希：与参考实现一致（参考实现用 strlen 取长度）。
// ⚠️ 这是"与外部实现互操作"的代价，不是本模块的取舍 —— 见 bcrypt.cpp 文件头。
TEST(BcryptTest, NulTruncatesAtTheFirstNulByte) {
  const std::string with_nul("ab\0cd", 5);
  EXPECT_EQ(hash_password_with_salt(with_nul, 4, "DCq7YPn5Rq63x1Lad4cll."),
            hash_password_with_salt("ab", 4, "DCq7YPn5Rq63x1Lad4cll."));
  EXPECT_TRUE(verify_password(with_nul, hash_password("ab", 4)));
}

// ---- 随机盐路径（生产路径）------------------------------------------------------

TEST(BcryptTest, HashPasswordProducesAWellformedSixtyCharHash) {
  const std::string hash = hash_password("hunter2", kBcryptDefaultCost);
  EXPECT_EQ(hash.size(), kBcryptHashLength);
  EXPECT_EQ(hash.substr(0, 4), "$2b$");
  EXPECT_EQ(hash.substr(4, 2), "10");
  EXPECT_TRUE(is_hash_wellformed(hash));
  EXPECT_EQ(cost_of_hash(hash), kBcryptDefaultCost);
}

TEST(BcryptTest, SamePasswordHashesDifferBecauseSaltIsRandom) {
  const std::string first = hash_password("hunter2", kBcryptMinCost);
  const std::string second = hash_password("hunter2", kBcryptMinCost);
  EXPECT_NE(first, second) << "两次哈希的盐段必须不同，否则就是可预测的盐";
  EXPECT_NE(first.substr(7, kBcryptSaltChars), second.substr(7, kBcryptSaltChars));
}

TEST(BcryptTest, VerifyAcceptsItsOwnOutputAndRejectsWrongPasswords) {
  const std::string hash = hash_password("correct horse", kBcryptMinCost);
  EXPECT_TRUE(verify_password("correct horse", hash));
  EXPECT_FALSE(verify_password("correct horse ", hash));
  EXPECT_FALSE(verify_password("Correct horse", hash));
  EXPECT_FALSE(verify_password("", hash));
}

// ---- 形状校验：坏数据不该让请求崩掉（verify 返回 false 而不是抛）----------------

TEST(BcryptTest, VerifyRejectsMalformedStoredHashesWithoutThrowing) {
  const std::string good = hash_password("x", kBcryptMinCost);
  EXPECT_FALSE(verify_password("x", ""));
  EXPECT_FALSE(verify_password("x", "not-a-hash"));
  EXPECT_FALSE(verify_password("x", good + "0"));                                  // 61 字符
  EXPECT_FALSE(verify_password("x", good.substr(0, 59)));                         // 59 字符
  EXPECT_FALSE(verify_password("x", good.substr(0, 57) + "!" + good.substr(58)));  // 非法 base64 字符
  EXPECT_FALSE(verify_password("x", std::string("$2x$04$") + good.substr(7)));      // $2x$ 是"有 bug 的实现"的标记
  EXPECT_FALSE(verify_password("x", std::string("$1$04$") + good.substr(7)));       // 不是 bcrypt
  EXPECT_FALSE(verify_password("x", std::string("$2b$99$") + good.substr(7)));      // cost 越界（算力攻击）
  EXPECT_TRUE(is_hash_wellformed(good));
}

TEST(BcryptTest, CostOutsideTheLegalRangeThrows) {
  EXPECT_THROW(hash_password("x", kBcryptMinCost - 1), std::invalid_argument);
  EXPECT_THROW(hash_password("x", kBcryptMaxCost + 1), std::invalid_argument);
  EXPECT_THROW(hash_password_with_salt("x", 0, "DCq7YPn5Rq63x1Lad4cll."), std::invalid_argument);
}

TEST(BcryptTest, MalformedSaltThrowsInTheTestOnlyEntryPoint) {
  EXPECT_THROW(hash_password_with_salt("x", 4, "short"), std::invalid_argument);
  EXPECT_THROW(hash_password_with_salt("x", 4, std::string(22, '!')), std::invalid_argument);
}

}  // namespace web
}  // namespace oj
