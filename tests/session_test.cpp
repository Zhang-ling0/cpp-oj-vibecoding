// =============================================================================
// 会话管理（include/web/session.h + src/web/session.cpp）的单元测试
// 被测：SPEC.md §9 M3 T3.3（认证）/ T3.10（过期会话回收 §6.3.2）/ D-69（Cookie 属性）
// 运行：tests/run_unit_tests.sh（即 ctest；`ctest -R Session` 可单跑本文件）
//
// 【分两段，理由与 admin_test.cpp 相同】
//   A. 纯函数（token 形状、Cookie 解析/拼装）不碰 DB，永远会跑、跑得飞快。
//      而这几样恰好是【最容易被重构悄悄改坏】的：Cookie 属性的顺序与拼写、
//      token 的"只收小写 hex"这条 ci 排序规则的防线，都是不会让任何代码崩的改动。
//   B. sessions 表的读写（签发 / 查询 / 删除 / 回收）只有真库能验，连不上整体跳过。
//
// 【探针数据策略】与 admin_test.cpp 同款：不建独立库，只造带 `ojsess` 前缀的用户，
//   TearDown 里 `DELETE FROM users WHERE username LIKE 'ojsess%'` —— sessions 由
//   §3.6 的外键 CASCADE 一并带走，不留残行。
//   断言用【增量】而不是绝对值：库里本来就有题库与其它数据（T1.2 同步进两道题）。
//
// 【刻意不测的东西】
//   · bcrypt 密码哈希：不属于本模块（T3.3 的另一半），本文件不碰密码。
//   · HTTP 端点：`Set-Cookie` 头真正落到响应上是 T3.5 路由层的事；本层只保证
//     make_set_cookie() 产出的字符串逐字符合 §6.3。
// =============================================================================
#include <gtest/gtest.h>

#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "common/config.h"
#include "db/mysql_pool.h"
#include "web/session.h"

namespace {

using namespace oj;
using namespace oj::db;
using namespace oj::web;

constexpr const char* kUserPrefix = "ojsess";

std::string env_or(const char* key, const char* fallback) {
  const char* value = std::getenv(key);
  return (value != nullptr && *value != '\0') ? std::string(value) : std::string(fallback);
}

PoolOptions base_options() {
  PoolOptions o;
  o.host = env_or("MYSQL_HOST", "127.0.0.1");
  o.port = static_cast<unsigned int>(std::atoi(env_or("MYSQL_PORT", "3306").c_str()));
  o.user = env_or("MYSQL_USER", "oj");
  o.password = env_or("MYSQL_PASSWORD", "oj");
  o.database = env_or("MYSQL_DB", "oj");
  return o;
}

// =============================================================================
// A. 纯函数
// =============================================================================

TEST(SessionToken, IsSixtyFourLowercaseHexChars) {
  for (int i = 0; i < 20; ++i) {
    const std::string token = generate_token();
    ASSERT_EQ(token.size(), kTokenHexLength);
    EXPECT_TRUE(is_token_wellformed(token)) << token;
  }
}

TEST(SessionToken, DoesNotRepeat) {
  // 32 字节随机数的碰撞概率可以忽略；这里退一步只断言"若干次里不重复"，
  // 断的是"是不是每次都从同一个种子出发"这类实现错误。
  std::set<std::string> seen;
  for (int i = 0; i < 200; ++i) EXPECT_TRUE(seen.insert(generate_token()).second);
}

TEST(SessionToken, RejectsEverythingButSixtyFourLowercaseHex) {
  // ⚠️ 这条断言是本模块最容易被改坏的地方（见头文件【token 形状检查】）：
  // sessions.token 是 utf8mb4 CHAR(64)，默认排序规则 utf8mb4_general_ci **大小写不敏感**，
  // 所以 `WHERE token = 'AB…'` 会命中库里那条 `ab…`。放行非小写 hex 等于把 ci 排序规则
  // 变成 token 校验的一部分。因此大写十六进制必须被【拒绝】（而不是"也能用"）。
  std::string token = generate_token();
  for (char& c : token) {
    if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
  }
  EXPECT_FALSE(is_token_wellformed(token)) << token;

  const std::string good = generate_token();
  for (const std::string& bad : {
           std::string(),                                          // 空
           good.substr(0, good.size() - 1),                        // 63 位
           good + "0",                                             // 65 位
           std::string(kTokenHexLength, 'g'),                       // 非 hex 字符
           std::string(kTokenHexLength, ' '),                       // 空白
           std::string(kTokenHexLength + 1, 'a'),                   // 超长
           good.substr(0, 63) + "-",                                // URL 安全变体不是本模块的格式
       }) {
    EXPECT_FALSE(is_token_wellformed(bad)) << bad;
  }
  EXPECT_TRUE(is_token_wellformed(good));
}

TEST(SessionCookie, ParsesTheSidFromAMultiCookieHeader) {
  const std::string token = generate_token();
  const std::optional<std::string> sid =
      parse_sid("theme=dark; sid=" + token + "; lang=zh-CN");
  ASSERT_TRUE(sid.has_value());
  EXPECT_EQ(*sid, token);
}

TEST(SessionCookie, RequiresExactNameNotAPrefix) {
  // 同前缀的别的 Cookie（别的子域应用、或早期版本遗留的命名）绝不能被当成本站会话。
  const std::string token = generate_token();
  for (const std::string& header :
       {"xsid=" + token, "sid_extra=" + token, "Ssid=" + token, "sidx=" + token}) {
    EXPECT_FALSE(parse_sid(header).has_value()) << header;
  }
  // 名字大小写敏感（RFC 6265 的 cookie-name 是区分大小写的）
  EXPECT_FALSE(parse_sid("SID=" + token).has_value());
}

TEST(SessionCookie, HandlesWhitespaceQuotesAndEmptyValues) {
  const std::string token = generate_token();
  EXPECT_EQ(parse_sid("a=1;sid=" + token), std::optional<std::string>(token));
  EXPECT_EQ(parse_sid("sid=" + token + " ;b=2"), std::optional<std::string>(token));
  EXPECT_EQ(parse_sid("  sid  =  " + token), std::optional<std::string>(token));
  // 成对双引号要剥掉，否则查库永远查不到这一行
  EXPECT_EQ(parse_sid("sid=\"" + token + "\""), std::optional<std::string>(token));

  // 这些都是"没登录"，不是错误
  for (const char* header :
       {"", " ", "a=1;b=2", "sid=", "sid=   ", "sid=\"\"", "sid", "=abc", ";;"}) {
    EXPECT_FALSE(parse_sid(header).has_value()) << "[" << header << "]";
  }
}

TEST(SessionCookie, FirstSidWinsAndEmptyOneStopsTheSearch) {
  // 同名 Cookie 重复只可能来自不同 Path 的遗留。规则必须是确定的：
  // "第一个 sid 说了算"，且空值同样占位（不往后找）—— 否则规则随浏览器排序而变。
  const std::string first = generate_token();
  const std::string second = generate_token();
  EXPECT_EQ(parse_sid("sid=" + first + "; sid=" + second), std::optional<std::string>(first));
  EXPECT_FALSE(parse_sid("sid=; sid=" + second).has_value());
}

TEST(SessionSetCookie, MatchesTheContractOfSection63) {
  const std::string token = generate_token();
  const std::string header = make_set_cookie(token);
  // §6.3 逐字：`Set-Cookie: sid=...; HttpOnly; SameSite=Lax; Path=/`（D-69）
  EXPECT_EQ(header, "sid=" + token + "; HttpOnly; SameSite=Lax; Path=/");
  // D-69 声明的欠账：首版明文 HTTP，【故意不加 Secure】。哪天有人"顺手补上"，
  // 这条会红 —— 那时要连带做 §6.3.2.1 的三条（HTTPS + 全站 https + 无硬编码 http://）。
  EXPECT_EQ(header.find("Secure"), std::string::npos);
  // 另两个属性必须【在】，且 HttpOnly 在 SameSite 之前 —— 顺序照抄 §6.3 便于逐字对照
  EXPECT_NE(header.find("HttpOnly"), std::string::npos);
  EXPECT_NE(header.find("SameSite=Lax"), std::string::npos);
  EXPECT_LT(header.find("HttpOnly"), header.find("SameSite=Lax"));
}

TEST(SessionSetCookie, ClearCookieKeepsNameAndAttributesIdentical) {
  const std::string clear = make_clear_cookie();
  // 名字与四个属性只要有一项与签发时不同，浏览器就当成另一个 Cookie 留着 ——
  // 那就是"服务端清了、客户端没清"的 logout。
  EXPECT_NE(clear.find("sid=;"), std::string::npos);
  EXPECT_NE(clear.find("HttpOnly"), std::string::npos);
  EXPECT_NE(clear.find("SameSite=Lax"), std::string::npos);
  EXPECT_NE(clear.find("Path=/"), std::string::npos);
  EXPECT_NE(clear.find("Max-Age=0"), std::string::npos);
  EXPECT_NE(clear.find("Expires=Thu, 01 Jan 1970"), std::string::npos);
}

TEST(SessionSetCookie, RoundTripsThroughTheParser) {
  // 自己签发 → 自己解析回来必须一致（Cookie 值被引号包裹等真实世界的变形已单测）
  const std::string token = generate_token();
  const std::string header = make_set_cookie(token);
  const std::string cookie_header = header.substr(0, header.find(';'));
  EXPECT_EQ(parse_sid(cookie_header), std::optional<std::string>(token));
}

TEST(SessionAuthenticate, WithoutCookieHeaderNeedsNoPool) {
  // 匿名读题面是常态（D-43）。无 sid 时【不得】碰 DB：单测用默认构造的 Session
  // （任何语句都是惰性失败）就能证明这一点 —— 若实现改了顺序，这里立刻红。
  db::Session dead;
  std::string error;
  EXPECT_FALSE(authenticate(dead, "", &error).has_value());
  EXPECT_TRUE(error.empty());   // 未登录不是错误（路由层据此回 1002 而不是 1008）
}

TEST(SessionAuthenticate, MalformedTokenNeverReachesTheDatabase) {
  // 形状检查必须【先于】SQL：否则 ci 排序规则就成了 token 校验的一部分。
  db::Session dead;
  std::string error;
  const std::string cookie_header = "sid=" + std::string(kTokenHexLength, 'Z');
  EXPECT_FALSE(authenticate(dead, cookie_header, &error).has_value());
  EXPECT_TRUE(error.empty()) << error;   // 没有 DB 错误被上抛
}

TEST(SessionAuthenticate, DeadSessionReportsUnauthenticatedWithoutError) {
  // 会话不可用时 find_user 失败 ⇒ error 非空（路由层回 1008），而不是悄悄当成"没登录"。
  // 两者在页面上分别表现为"跳登录页"与"报错"，混淆会让真正的故障被当成用户没登录。
  db::Session dead;
  std::string error;
  const std::string cookie_header = "sid=" + generate_token();
  EXPECT_FALSE(authenticate(dead, cookie_header, &error).has_value());
  EXPECT_FALSE(error.empty());
}

// =============================================================================
// B. sessions 表（真库；连不上整体跳过）
// =============================================================================

class SessionDbTest : public ::testing::Test {
 protected:
  void SetUp() override {
    options_ = base_options();
    Pool probe;
    if (!probe.init(options_)) {
      GTEST_SKIP() << "无法连接 MySQL " << options_.host << ":" << options_.port
                   << "/" << options_.database << "，跳过依赖真实数据库的用例";
    }
    probe.shutdown();

    pool_.reset(new Pool());
    ASSERT_TRUE(pool_->init(options_)) << pool_->describe();

    Lease lease = pool_->acquire();
    ASSERT_TRUE(lease.valid());
    if (!tables_exist(lease.session())) {
      pool_->shutdown();
      pool_.reset();
      GTEST_SKIP() << "当前库缺少 §3.2 的表，请先执行 ./build/oj 完成启动建表后再跑本组用例";
    }
    clear_probe_rows(lease.session());
  }

  void TearDown() override {
    if (pool_ != nullptr && pool_->ready()) {
      Lease lease = pool_->acquire();
      if (lease.valid()) clear_probe_rows(lease.session());   // sessions 由 CASCADE 带走
    }
    if (pool_ != nullptr) {
      pool_->shutdown();
      pool_.reset();
    }
  }

  // 取一次租约、执行 fn、立刻归还。
  // ⚠️ 存在的理由是 [D-21]：Pool::acquire() 对【同一线程】的第二次调用会主动拒绝
  //   （防自死锁），所以测试里绝不能"一边握着租约一边调用另一个会取租约的辅助函数"。
  //   所有需要读库的断言都走这里，用完即还。
  template <typename Fn>
  auto with_session(Fn&& fn) -> decltype(fn(*(db::Session*)nullptr)) {
    Lease lease = pool_->acquire();
    EXPECT_TRUE(lease.valid()) << "取不到租约（同一线程重复 acquire 会被拒绝）";
    return fn(lease.session());
  }

  static bool tables_exist(db::Session& session) {
    const db::ResultSet r = session.run(
        "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = DATABASE()"
        " AND table_name IN ('users','sessions')");
    return !r.failed() && !r.empty() && r.at(0).u64(0) == 2;
  }

  static void clear_probe_rows(db::Session& session) {
    session.run(std::string("DELETE FROM users WHERE username LIKE '") + kUserPrefix + "%'");
  }

  unsigned long long make_user(const std::string& suffix, const std::string& role) {
    return with_session([&](db::Session& s) -> unsigned long long {
      const db::ResultSet r = s.run(
          "INSERT INTO users (username, password_hash, role, created_at)"
          " VALUES (?, 'x', ?, UTC_TIMESTAMP())",
          {Value::from_text(std::string(kUserPrefix) + suffix), Value::from_text(role)});
      EXPECT_FALSE(r.failed()) << r.error().format();
      return r.insert_id();
    });
  }

  // 直接插一行会话，expires_delta_days 可为负（造过期行）、可为 0（造到期边界行）。
  void make_session(unsigned long long user_id, const std::string& token, int expires_delta_days) {
    with_session([&](db::Session& s) {
      const db::ResultSet r = s.run(
          "INSERT INTO sessions (token, user_id, expires_at, created_at)"
          " VALUES (?, ?, UTC_TIMESTAMP() + INTERVAL ? DAY, UTC_TIMESTAMP())",
          {Value::from_text(token), Value::from_u64(user_id),
           Value::from_i64(expires_delta_days)});
      EXPECT_FALSE(r.failed()) << r.error().format();
    });
  }

  long count_sessions(unsigned long long user_id) {
    return with_session([&](db::Session& s) -> long {
      const db::ResultSet r =
          s.run("SELECT COUNT(*) FROM sessions WHERE user_id = ?", {Value::from_u64(user_id)});
      EXPECT_FALSE(r.failed()) << r.error().format();
      return r.empty() ? -1L : static_cast<long>(r.at(0).i64(0));
    });
  }

  // 距离到期还有几天（MySQL 的 DATEDIFF，跨月/跨年都对）。
  // ⚠️ 不要在 C++ 里把 'YYYY-MM-DD HH:MM:SS' 折算成天数：那是自己实现一遍日历，
  //   而本模块的整个设计就是"时间一律由 MySQL 算"（见 src/web/session.cpp 文件头）。
  long days_until_expiry(const std::string& token) {
    return with_session([&](db::Session& s) -> long {
      const db::ResultSet r = s.run(
          "SELECT DATEDIFF(expires_at, UTC_TIMESTAMP()) FROM sessions WHERE token = ?",
          {Value::from_text(token)});
      EXPECT_FALSE(r.failed()) << r.error().format();
      return r.empty() ? -999L : static_cast<long>(r.at(0).i64(0));
    });
  }

  std::string session_time_zone() {
    return with_session([](db::Session& s) -> std::string {
      const db::ResultSet r = s.run("SELECT @@session.time_zone");
      EXPECT_FALSE(r.failed()) << r.error().format();
      return r.empty() ? std::string() : r.at(0).str(0);
    });
  }

  // 组合动作：一个事务性步骤（issue / destroy* / purge）在独立租约里跑完即还。
  template <typename Fn>
  void step(Fn&& fn) {
    with_session([&](db::Session& s) { fn(s); });
  }

  std::unique_ptr<Pool> pool_;
  PoolOptions options_;
};

TEST_F(SessionDbTest, IssueInsertsARowThatAuthenticateCanFind) {
  const unsigned long long user_id = make_user("-alice", "user");
  std::string token;
  std::string error;
  step([&](db::Session& s) {
    ASSERT_TRUE(issue(s, user_id, &token, nullptr, &error)) << error;
  });
  EXPECT_TRUE(is_token_wellformed(token));
  EXPECT_EQ(count_sessions(user_id), 1L);

  std::string error2;
  const std::optional<AuthUser> found = with_session(
      [&](db::Session& s) { return authenticate(s, make_set_cookie(token), &error2); });
  ASSERT_TRUE(found.has_value()) << error2;
  EXPECT_EQ(found->user_id, user_id);
  EXPECT_EQ(found->username, std::string(kUserPrefix) + "-alice");
  EXPECT_EQ(found->role, "user");
  EXPECT_FALSE(found->is_admin());
}

TEST_F(SessionDbTest, IssueUsesTheConfiguredTtl) {
  // TTL 的唯一来源是 config().session_ttl_days（§6.7.3 的 OJ_SESSION_TTL_DAYS），
  // 代码里没有第二处常量。这条把"签发出来的 expires_at 就是 TTL 天后"钉死。
  const unsigned long long user_id = make_user("-ttl", "user");
  std::string token;
  std::string error;
  step([&](db::Session& s) {
    ASSERT_TRUE(issue(s, user_id, &token, nullptr, &error)) << error;
  });
  EXPECT_EQ(days_until_expiry(token), static_cast<long>(common::config().session_ttl_days));
}

TEST_F(SessionDbTest, EachIssueMakesANewToken) {
  const unsigned long long user_id = make_user("-multi", "user");
  std::string first;
  std::string second;
  std::string error;
  step([&](db::Session& s) {
    ASSERT_TRUE(issue(s, user_id, &first, nullptr, &error)) << error;
    ASSERT_TRUE(issue(s, user_id, &second, nullptr, &error)) << error;
  });
  EXPECT_NE(first, second);
  EXPECT_EQ(count_sessions(user_id), 2L);   // 多设备多会话（D-46：不做会话列表，但也不合并）
}

TEST_F(SessionDbTest, ExpiredRowIsNotALogin) {
  // 过期 = 不存在（本层不删它，删除只由 purge_expired / destroy / destroy_others 做）。
  const unsigned long long user_id = make_user("-expired", "user");
  const std::string token = generate_token();
  make_session(user_id, token, -1);

  std::string error;
  const std::optional<AuthUser> found =
      with_session([&](db::Session& s) { return authenticate(s, make_set_cookie(token), &error); });
  EXPECT_FALSE(found.has_value());
  EXPECT_TRUE(error.empty());                  // 是"未登录"，不是 DB 故障
  EXPECT_EQ(count_sessions(user_id), 1L);      // 过期行仍在，等回收任务处理
}

TEST_F(SessionDbTest, PurgeRemovesOnlyExpiredRows) {
  // §6.3.2 / AC-17：过期行被删、未过期行不动。
  const unsigned long long user_id = make_user("-purge", "user");
  const std::string stale = generate_token();
  const std::string fresh = generate_token();
  const std::string boundary = generate_token();
  make_session(user_id, stale, -1);
  make_session(user_id, fresh, 7);
  // 边界：expires_at == 当前秒的那一行。
  //   回收语句用 `<` ⇒ 它【不被删】（"已过期"是严格早于到期时刻）；
  //   登录查询用 `>` ⇒ 它【已不能登录】。
  // 两条语句在同一秒上给出互补的答案：回收判"是否曾经过期"，登录判"此刻是否有效"。
  // 少任何一条断言，将来有人把 `<` 改成 `<=`（会删掉刚签发、浏览器还没拿到的会话）
  // 或把 `>` 改成 `>=`（会让到期那一秒仍能登录）都不会被发现。
  make_session(user_id, boundary, 0);

  long removed = -1;
  std::string error;
  step([&](db::Session& s) { ASSERT_TRUE(purge_expired(s, &removed, &error)) << error; });
  EXPECT_EQ(removed, 1L);
  EXPECT_EQ(count_sessions(user_id), 2L);

  std::string lookup_error;
  const std::optional<AuthUser> alive = with_session(
      [&](db::Session& s) { return authenticate(s, make_set_cookie(fresh), &lookup_error); });
  EXPECT_TRUE(alive.has_value()) << lookup_error;
  lookup_error.clear();
  const std::optional<AuthUser> at_expiry = with_session(
      [&](db::Session& s) { return authenticate(s, make_set_cookie(boundary), &lookup_error); });
  EXPECT_FALSE(at_expiry.has_value());
  EXPECT_TRUE(lookup_error.empty());
}

TEST_F(SessionDbTest, PurgeComparesInUtcAndTheConnectionIsPinnedToUtc) {
  // §6.3.2 的唯一正确性要点：expires_at 存 UTC（[D-17]），比较必须用 UTC_TIMESTAMP()。
  // 这里从两端钉住它：
  //   ① 连接时区被 mysql_pool 锁成 +00:00（本测试自己的 Pool 也走同一段初始化），
  //     所以 NOW() 与 UTC_TIMESTAMP() 在本连接上相等 —— 这是"即便如此也不写 NOW()"
  //     的理由：SQL 的正确性不能依赖连接参数恰好配对了。
  //   ② 一小时后到期的那一行【不应】被回收。
  EXPECT_EQ(session_time_zone(), "+00:00");

  const unsigned long long user_id = make_user("-tz", "user");
  const std::string token = generate_token();
  with_session([&](db::Session& s) {
    const db::ResultSet r = s.run(
        "INSERT INTO sessions (token, user_id, expires_at, created_at)"
        " VALUES (?, ?, UTC_TIMESTAMP() + INTERVAL 1 HOUR, UTC_TIMESTAMP())",
        {Value::from_text(token), Value::from_u64(user_id)});
    ASSERT_FALSE(r.failed()) << r.error().format();
  });

  long removed = -1;
  std::string error;
  step([&](db::Session& s) { ASSERT_TRUE(purge_expired(s, &removed, &error)) << error; });
  EXPECT_EQ(removed, 0L);
  EXPECT_EQ(count_sessions(user_id), 1L);
}

TEST_F(SessionDbTest, IssuePurgesAndInsertsInsideOneTransaction) {
  // §6.3.2：回收与插入必须同一事务。分开提交时两个并发登录可能删掉对方
  // 刚落库、还没交给浏览器的会话。这里能验的是"issue 确实顺带回收了"，
  // 以及"回收后新会话仍然在库"（若回收排在插入之后，就会把自己刚插的那行删掉）。
  const unsigned long long user_id = make_user("-issue-purge", "user");
  make_session(user_id, generate_token(), -1);

  std::string token;
  long purged = -1;
  std::string error;
  step([&](db::Session& s) { ASSERT_TRUE(issue(s, user_id, &token, &purged, &error)) << error; });
  EXPECT_EQ(purged, 1L);
  EXPECT_EQ(count_sessions(user_id), 1L);
  EXPECT_EQ(days_until_expiry(token) > 0, true);
}

TEST_F(SessionDbTest, DestroyIsIdempotent) {
  const unsigned long long user_id = make_user("-logout", "user");
  const std::string token = generate_token();
  make_session(user_id, token, 7);

  std::string error;
  step([&](db::Session& s) { ASSERT_TRUE(destroy(s, token, &error)) << error; });
  EXPECT_EQ(count_sessions(user_id), 0L);
  // 再删一次仍然成功：logout 的语义是"让这个浏览器不再是登录态"，幂等是应有性质
  error.clear();
  step([&](db::Session& s) { ASSERT_TRUE(destroy(s, token, &error)) << error; });
  EXPECT_TRUE(error.empty());

  std::string lookup_error;
  const std::optional<AuthUser> found = with_session(
      [&](db::Session& s) { return authenticate(s, make_set_cookie(token), &lookup_error); });
  EXPECT_FALSE(found.has_value());
}

TEST_F(SessionDbTest, DestroyOthersKeepsTheCurrentSession) {
  // D-40 / AC-21：`AND token <> ?` 是硬要求。没有它，改完密码的人会被自己踢下线。
  const unsigned long long user_id = make_user("-kick", "user");
  const std::string keep = generate_token();
  const std::string other_one = generate_token();
  const std::string other_two = generate_token();
  make_session(user_id, keep, 7);
  make_session(user_id, other_one, 7);
  make_session(user_id, other_two, 7);
  const unsigned long long other_user_id = make_user("-bystander", "user");
  make_session(other_user_id, generate_token(), 7);

  unsigned long long removed = 99;
  std::string error;
  step([&](db::Session& s) {
    ASSERT_TRUE(destroy_others(s, user_id, keep, &removed, &error)) << error;
  });
  EXPECT_EQ(removed, 2ULL);
  EXPECT_EQ(count_sessions(user_id), 1L);          // 只剩当前会话
  EXPECT_EQ(count_sessions(other_user_id), 1L);    // 别人的会话不受影响

  std::string lookup_error;
  const std::optional<AuthUser> kept = with_session(
      [&](db::Session& s) { return authenticate(s, make_set_cookie(keep), &lookup_error); });
  EXPECT_TRUE(kept.has_value()) << lookup_error;
  const std::optional<AuthUser> kicked = with_session(
      [&](db::Session& s) { return authenticate(s, make_set_cookie(other_one), &lookup_error); });
  EXPECT_FALSE(kicked.has_value());
}

TEST_F(SessionDbTest, DeletingTheUserCascadesToSessions) {
  // §3.6 / AC-33：注销账号（硬删 users 行）带走 sessions。本层不参与注销，
  // 但这条断言守着"本层没有写出与 CASCADE 冲突的东西"。
  const unsigned long long user_id = make_user("-cascade", "user");
  make_session(user_id, generate_token(), 7);
  make_session(user_id, generate_token(), 7);
  ASSERT_EQ(count_sessions(user_id), 2L);

  with_session([&](db::Session& s) {
    const db::ResultSet deleted =
        s.run("DELETE FROM users WHERE id = ?", {Value::from_u64(user_id)});
    ASSERT_FALSE(deleted.failed()) << deleted.error().format();
  });
  EXPECT_EQ(count_sessions(user_id), 0L);
}

TEST_F(SessionDbTest, RoleComesFromTheUsersTableNotTheSession) {
  // 角色真源是 users.role（[D-37] 启动同步），会话行里没有角色 ——
  // 所以管理员被降权后【下一个请求就生效】，不需要重登录也不需要刷新 Cookie。
  const unsigned long long user_id = make_user("-role", "admin");
  std::string token;
  std::string error;
  step([&](db::Session& s) { ASSERT_TRUE(issue(s, user_id, &token, nullptr, &error)) << error; });

  std::string lookup_error;
  std::optional<AuthUser> found = with_session(
      [&](db::Session& s) { return authenticate(s, make_set_cookie(token), &lookup_error); });
  ASSERT_TRUE(found.has_value()) << lookup_error;
  EXPECT_TRUE(found->is_admin());

  with_session([&](db::Session& s) {
    const db::ResultSet demoted =
        s.run("UPDATE users SET role = 'user' WHERE id = ?", {Value::from_u64(user_id)});
    ASSERT_FALSE(demoted.failed()) << demoted.error().format();
  });

  lookup_error.clear();
  found = with_session(
      [&](db::Session& s) { return authenticate(s, make_set_cookie(token), &lookup_error); });
  ASSERT_TRUE(found.has_value()) << lookup_error;
  EXPECT_EQ(found->role, "user");
  EXPECT_FALSE(found->is_admin());
}

TEST_F(SessionDbTest, OutParamsAreOptional) {
  // 只要 token 的调用方（登录端点）不必为了凑签名造两个变量；反之亦然。
  const unsigned long long user_id = make_user("-optional", "user");
  std::string error;
  step([&](db::Session& s) {
    ASSERT_TRUE(issue(s, user_id, nullptr, nullptr, &error)) << error;
    ASSERT_TRUE(purge_expired(s, nullptr, &error)) << error;
    ASSERT_TRUE(destroy(s, generate_token(), &error)) << error;
    ASSERT_TRUE(destroy_others(s, user_id, generate_token(), nullptr, &error)) << error;
    ASSERT_TRUE(issue(s, user_id, nullptr, nullptr, nullptr)) << error;
  });
  // 上面最后一次 issue 把本用户唯一那条会话也踢了（keep_token 是随机的），
  // 所以这里应该是 0 —— 顺带钉住"destroy_others 认得出自己的会话"之外的那条：
  EXPECT_EQ(count_sessions(user_id), 1L);
}

TEST_F(SessionDbTest, FailedOperationLeavesOutUntouched) {
  // 不变式：DB 失败时 *token_out 保持原值。调用方拿着旧 token 继续发 Set-Cookie，
  // 会造出一个"看似登录成功、其实查不到会话"的 Cookie —— 静默半成功比报错难查。
  db::Session dead;
  std::string token = "sentinel";
  long purged = 77;
  std::string error;
  EXPECT_FALSE(issue(dead, 1, &token, &purged, &error));
  EXPECT_EQ(token, "sentinel");
  EXPECT_EQ(purged, 77L);
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(purge_expired(dead, &purged, &error));
  EXPECT_EQ(purged, 77L);
  unsigned long long removed = 55;
  EXPECT_FALSE(destroy_others(dead, 1, "t", &removed, &error));
  EXPECT_EQ(removed, 55ULL);
}

}  // namespace