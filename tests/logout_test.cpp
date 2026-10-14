// =============================================================================
// 登出端点 `POST /api/auth/logout` 的单元测试（SPEC §9 M3 T3.3 认证 / T3.5 路由）
// 被测：web::plan_logout（include/web/routes.h）+ web::logout_user（web/auth_api.h）
//       + web::session::{issue, destroy, make_set_cookie, make_clear_cookie}
// 契约：§6.3 端点表那一行「删除会话行，清 Cookie」（Auth=否 ⇒ 未登录也 200）
// 验收：AC-1 第三条「登出后旧 Cookie 返回 1002」
// 运行：ctest -R Logout ；或 ./build/tests/logout_test
//
// 【本文件补的是哪一段空白 —— 说清楚才值得存在】
//   已有的三处覆盖各自都成立，但**没有一处穿过路由层的接线**：
//     · session_test.cpp  : destroy(token) 本身，token 由用例自己造
//     · auth_api_test.cpp : logout_user(token)，token 也是用例自己造
//     · routes_test.cpp   : 只测 error_reply_for 与静态挂载，全程不起 HTTP
//   而线上真实的路径是「**浏览器发来的 Cookie 头** → plan_logout → logout_user」。
//   本文件从 Cookie 头（`sid=<token>; …` 的完整 Set-Cookie 值）起步，
//   走完 plan_logout + logout_user，再回到 authenticate ——
//   钉住的是【这一段的组合】，任何一环被单独改坏都会在这里露出来。
//   最典型的失效：plan_logout 若改成"形状不合法就不给 token"，
//   上面三个测试仍然全绿（它们都传合法 token），而线上"坏 sid 永远登不掉"。
//
// 【两段式，理由与 session_test.cpp / auth_api_test.cpp 相同】
//   A. 纯决策：不需要 MySQL、不起服务，永远会跑
//   B. 组合动作：需要真库，连不上整体 GTEST_SKIP
//
// 【探针数据策略】沿用同款：只造 `ojlogout` 前缀的用户，
//   TearDown 按前缀删掉（sessions 由 §3.6 的外键 CASCADE 一并带走）。
//   断言一律用【增量】（"多了 1 行"），不用绝对计数 —— 库里本来就有别的数据。
// =============================================================================
#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <string>

#include "db/mysql_pool.h"
#include "web/auth_api.h"
#include "web/routes.h"
#include "web/session.h"

namespace {

using namespace oj;
using namespace oj::db;
using namespace oj::web;

constexpr const char* kUserPrefix = "ojlogout";

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
// A. 纯决策：plan_logout（不起服务、不碰 DB）
// =============================================================================

// 【核心不变量】没有 Cookie 头 ⇒ 不碰 DB。
//
//   [D-21] 把全部 DB 访问串行化在一把进程级互斥量上。匿名登出（curl 直连、
//   页面回退后重复点击、顶栏状态与实际 Cookie 不一致）必然影响 0 行，
//   却要让整个服务的 DB 队列停一下 —— 这就是 §6.1 的延迟凭空多出来的来源。
//   把 plan_logout 改回"先取租约再解析 Cookie"，本条立刻红。
TEST(LogoutPlan, NoCookieHeaderMeansNoDatabaseAccessAtAll) {
  for (const char* header : {"", " ", "theme=dark", "sidx=abc", "SID=abc"}) {
    const LogoutPlan plan = plan_logout(header);
    EXPECT_FALSE(plan.needs_db) << "Cookie 头=[" << header << "] 不该触发查库";
    EXPECT_TRUE(plan.token.empty()) << "Cookie 头=[" << header << "]";
  }
}

// 空值 sid（`sid=` / `sid=""`）等价于"没有这个 Cookie"（见 session.h 的 parse_sid）：
// 取租约去删一个空 token 同样是白排队。
TEST(LogoutPlan, AnEmptySidCountsAsNoSession) {
  EXPECT_FALSE(plan_logout("sid=").needs_db);
  EXPECT_FALSE(plan_logout("sid=\"\"").needs_db);
  EXPECT_FALSE(plan_logout("theme=dark; sid=; other=1").needs_db);
}

// 有非空 sid ⇒ 交给 logout_user 的就是那个 sid，且【形状不合法也要交给它】。
// 登出对任意 token 都成立（删不掉就是 0 行，那也是成功，见 §6.3 的幂等语义）。
// ⚠️ 少这一层的话，"坏 sid 永远登不掉"这种回归不会被任何用例发现：
//   session_test 与 auth_api_test 传的都是合法 token。
TEST(LogoutPlan, AnyNonEmptySidGoesStraightToLogoutUser) {
  const std::string token = generate_token();
  const LogoutPlan good = plan_logout(make_set_cookie(token));
  EXPECT_TRUE(good.needs_db);
  EXPECT_EQ(good.token, token) << "取出的必须是 sid 的值本身，不是整条 Cookie 头";

  // 合法 token 包在别的 Cookie 中间（浏览器行为），仍取到同一个值
  const LogoutPlan wrapped = plan_logout("theme=dark; " + make_set_cookie(token) + "; tz=UTC");
  EXPECT_EQ(wrapped.token, token);

  // 形状非法（非 64 位小写 hex）—— 仍然要交给 logout_user
  const std::string junk = "not-a-token";
  const LogoutPlan bad = plan_logout("sid=" + junk);
  EXPECT_TRUE(bad.needs_db) <<
      "形状非法的 sid 若在此处被拦下，浏览器里那个坏 Cookie 就永远清不掉"
      "（清除 Cookie 那头照发，看似生效，下次带上同一个坏 sid 还是老样子）";
  EXPECT_EQ(bad.token, junk);
}

// 同一个 sid 出现两次时取第一个 —— parse_sid 的"第一个 sid 说了算"这条确定性规则
// 在登出路径上同样成立，不能因为登出是"删掉"而不是"查身份"就改成取最后一个。
TEST(LogoutPlan, TheFirstSidWinsWhenTheCookieIsRepeated) {
  const std::string first = generate_token();
  const std::string second = generate_token();
  EXPECT_EQ(plan_logout("sid=" + first + "; sid=" + second).token, first);
}

// 清除 Cookie 必须【逐字】等于 session::make_clear_cookie()：
// handle_logout 的三处（logout_user 成功 / plan.needs_db 为 false / 失败前未生成）
// 最终都走这一份字符串，任何一处自己拼都会让"服务端清了客户端没清"重新出现。
// 本条钉的是"路由层不得另造一份 Cookie 文本"这件事的可见形式。
TEST(LogoutCookie, TheClearCookieIsTheOneTheSessionLayerProduces) {
  const std::string clear = make_clear_cookie();
  EXPECT_EQ(clear.rfind("sid=;", 0), 0U) << clear;
  EXPECT_NE(clear.find("Max-Age=0"), std::string::npos) << clear;
  EXPECT_NE(clear.find("Expires=Thu, 01 Jan 1970"), std::string::npos) << clear;
}

// 清除头的【属性串必须逐字是签发头属性串的前缀】—— 同名 + 同 Path + 同 HttpOnly
// + 同 SameSite + 同顺序，任一不同浏览器就当成另一个 Cookie 留着，
// 症状是"服务端已登出、刷新又变回登录态"。
//
// ⚠️ 比的是【从第一个分号起的后缀】而不是整条：`sid=<token>; …` 与 `sid=; …`
// 的前半段本来就该不同（一个是会话值，一个是空值），拿整条比会永远红。
// session_test 已有 ClearCookieKeepsNameAndAttributesIdentical 逐项查存在性，
// 那查不出"顺序被调换"与"属性被加了一节"；本条查的是前缀关系，两者不重叠。
TEST(LogoutCookie, TheClearCookieSharesTheExactAttributePrefixWithTheSetCookie) {
  const std::string set = make_set_cookie(generate_token());
  const std::string clear = make_clear_cookie();
  const std::string set_attrs = set.substr(set.find(';'));
  const std::string clear_attrs = clear.substr(clear.find(';'));
  EXPECT_EQ(clear.find("sid="), set.find("sid=")) << "Cookie 名必须一致";
  ASSERT_EQ(set_attrs, "; HttpOnly; SameSite=Lax; Path=/") << set_attrs;
  EXPECT_EQ(clear_attrs.rfind(set_attrs, 0), 0U)
      << "清除头的属性必须【以】签发头的属性串开头，实际：" << clear_attrs;
}

// =============================================================================
// B. 组合动作：Cookie 头 → plan_logout → logout_user → authenticate（真库）
// =============================================================================

class LogoutDbTest : public ::testing::Test {
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

  // ⚠️ [D-21]：Pool::acquire() 对同一线程的第二次调用会主动拒绝（防自死锁），
  //   所以绝不能"一边握着租约一边调用另一个会取租约的辅助函数"。用完即还。
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

  // 造一个用户并返回 id。登出用例不需要真的走 bcrypt（那是 login 组的事）：
  // 这里要验的是**会话行**的增删，直接插会话行更少一个变量。
  unsigned long long make_user(const std::string& suffix) {
    return with_session([&](db::Session& s) -> unsigned long long {
      const db::ResultSet r = s.run(
          "INSERT INTO users (username, password_hash, role, created_at)"
          " VALUES (?, 'x', 'user', UTC_TIMESTAMP())",
          {Value::from_text(std::string(kUserPrefix) + suffix)});
      EXPECT_FALSE(r.failed()) << r.error().format();
      return r.insert_id();
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

  std::unique_ptr<Pool> pool_;
  PoolOptions options_;
};

// ⭐ 本文件最重要的一条：完整走一遍线上路径（Cookie 头 → plan_logout → logout_user）。
//   AC-1 第三条「登出后旧 Cookie 返回 1002」在这里才有意义 —— 只断言
//   count_sessions == 0 的话，"删了行但查询路径仍认它"这种回归会漏过去。
TEST_F(LogoutDbTest, AfterLogoutTheOldCookieNoLongerIdentifiesAnyone) {
  const unsigned long long user_id = make_user("-alice");
  const std::string token = generate_token();
  const std::string cookie_header = make_set_cookie(token);
  with_session([&](db::Session& s) {
    const db::ResultSet r = s.run(
        "INSERT INTO sessions (token, user_id, expires_at, created_at)"
        " VALUES (?, ?, UTC_TIMESTAMP() + INTERVAL 7 DAY, UTC_TIMESTAMP())",
        {Value::from_text(token), Value::from_u64(user_id)});
    EXPECT_FALSE(r.failed()) << r.error().format();
  });
  ASSERT_EQ(count_sessions(user_id), 1L);

  // 登出前：这个 Cookie 查得到身份
  std::string error;
  const std::optional<AuthUser> before = with_session(
      [&](db::Session& s) -> std::optional<AuthUser> { return authenticate(s, cookie_header, &error); });
  ASSERT_TRUE(before.has_value()) << error;

  // 路由层的决策：从整条 Cookie 头里取出 sid
  const LogoutPlan plan = plan_logout(cookie_header);
  ASSERT_TRUE(plan.needs_db);
  ASSERT_EQ(plan.token, token);

  std::string clear_cookie;
  const bool ok = with_session(
      [&](db::Session& s) { return logout_user(s, plan.token, &clear_cookie, &error); });
  EXPECT_TRUE(ok) << error;
  EXPECT_EQ(clear_cookie, make_clear_cookie());
  EXPECT_EQ(count_sessions(user_id), 0L);

  // 登出后：同一个 Cookie 查不到身份 ⇒ 路由层回 1002（AC-1）
  error.clear();
  const std::optional<AuthUser> after = with_session(
      [&](db::Session& s) -> std::optional<AuthUser> { return authenticate(s, cookie_header, &error); });
  EXPECT_FALSE(after.has_value()) << "登出后旧 Cookie 必须查不到身份（AC-1）";
  EXPECT_TRUE(error.empty()) << "查不到是【未登录】而不是【服务端故障】";
}

// §6.3.2 原话：「logout 只删自己那一行」。
//   掉这一条的症状是"在自己电脑上点了登出，手机上也被登出" ——
//   而 D-40 的改密码才是"踢掉其他设备"那条路径，两者的差别正是这一行 SQL。
//   现有用例都只造了一个会话，故删错了也看不出来。
TEST_F(LogoutDbTest, LogoutOnlyRemovesItsOwnSessionRow) {
  const unsigned long long user_id = make_user("-multi");
  const std::string desktop = generate_token();
  const std::string phone = generate_token();
  const unsigned long long bystander_id = make_user("-bystander");
  const std::string bystander = generate_token();
  with_session([&](db::Session& s) {
    for (const auto& pair : {std::make_pair(desktop, user_id),
                             std::make_pair(phone, user_id),
                             std::make_pair(bystander, bystander_id)}) {
      const db::ResultSet r = s.run(
          "INSERT INTO sessions (token, user_id, expires_at, created_at)"
          " VALUES (?, ?, UTC_TIMESTAMP() + INTERVAL 7 DAY, UTC_TIMESTAMP())",
          {Value::from_text(pair.first), Value::from_u64(pair.second)});
      EXPECT_FALSE(r.failed()) << r.error().format();
    }
  });
  ASSERT_EQ(count_sessions(user_id), 2L);

  const LogoutPlan plan = plan_logout("theme=dark; " + make_set_cookie(desktop));
  ASSERT_TRUE(plan.needs_db);
  std::string error;
  std::string clear_cookie;
  ASSERT_TRUE(with_session(
      [&](db::Session& s) { return logout_user(s, plan.token, &clear_cookie, &error); }))
      << error;

  EXPECT_EQ(count_sessions(user_id), 1L) << "只该删自己那一行，手机上的会话必须还在";
  EXPECT_EQ(count_sessions(bystander_id), 1L) << "别人的会话不受影响";

  std::string lookup_error;
  const std::optional<AuthUser> kept = with_session([&](db::Session& s) -> std::optional<AuthUser> {
    return authenticate(s, make_set_cookie(phone), &lookup_error);
  });
  EXPECT_TRUE(kept.has_value()) << lookup_error;
}

// 幂等：连点两次 / Cookie 已失效后再点，都必须成功（§6.3 的端点语义是幂等的）。
// 这一条在路由层的表现是【两次都回 200 且两次都带清除 Cookie】，
// 而不只是 logout_user 返回 true —— plan.needs_db 为 false 的那一次同样要发清除头。
TEST_F(LogoutDbTest, LoggingOutTwiceStillSucceeds) {
  const unsigned long long user_id = make_user("-twice");
  const std::string token = generate_token();
  with_session([&](db::Session& s) {
    const db::ResultSet r = s.run(
        "INSERT INTO sessions (token, user_id, expires_at, created_at)"
        " VALUES (?, ?, UTC_TIMESTAMP() + INTERVAL 7 DAY, UTC_TIMESTAMP())",
        {Value::from_text(token), Value::from_u64(user_id)});
    EXPECT_FALSE(r.failed()) << r.error().format();
  });

  const std::string cookie_header = make_set_cookie(token);
  std::string error;
  for (const int round : {1, 2}) {
    const LogoutPlan plan = plan_logout(cookie_header);
    ASSERT_TRUE(plan.needs_db) << "第 " << round << " 轮";
    std::string clear_cookie;
    EXPECT_TRUE(with_session(
        [&](db::Session& s) { return logout_user(s, plan.token, &clear_cookie, &error); }))
        << "第 " << round << " 轮登出：" << error;
    EXPECT_EQ(clear_cookie, make_clear_cookie()) << "第 " << round << " 轮";
  }
  EXPECT_EQ(count_sessions(user_id), 0L);
}

// 形状非法的 sid 走到 DB 层仍然成功（删不掉就是 0 行）。
// 与 plan_logout 那条纯逻辑用例配对：那条钉住"不该在路由层拦"，
// 这条钉住"拦不住也没关系，DB 层照样回成功"。
TEST_F(LogoutDbTest, AMalformedSidIsDeletedAsZeroRowsAndStillSucceeds) {
  const LogoutPlan plan = plan_logout("sid=not-a-real-token");
  ASSERT_TRUE(plan.needs_db);
  std::string error;
  std::string clear_cookie;
  EXPECT_TRUE(with_session(
      [&](db::Session& s) { return logout_user(s, plan.token, &clear_cookie, &error); }))
      << error;
  EXPECT_TRUE(error.empty()) << error;
  EXPECT_EQ(clear_cookie, make_clear_cookie());
}

// DB 不可用时必须回 false 且**不写清除 Cookie**。
// 反过来的症状是"服务器一挂，用户全被踢下线" —— 而这次踢下线什么也没解决。
TEST(LogoutDbFailure, AnUnusableSessionReportsFailureAndWritesNoCookie) {
  db::Session dead;
  const LogoutPlan plan = plan_logout(make_set_cookie(generate_token()));
  ASSERT_TRUE(plan.needs_db);
  std::string clear_cookie;
  std::string error;
  EXPECT_FALSE(logout_user(dead, plan.token, &clear_cookie, &error));
  EXPECT_FALSE(error.empty()) << "错误详情进 app.log 与上层，不该被吞掉";
  EXPECT_TRUE(clear_cookie.empty()) << "失败时不得写出过期 Cookie —— 浏览器被清了、服务端没清";
}

}  // namespace
