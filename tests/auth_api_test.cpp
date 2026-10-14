// =============================================================================
// 注册 / 登录 业务层（include/web/auth_api.h + src/web/auth_api.cpp）的单元测试
// 被测：SPEC.md §9 M3 T3.3（认证）；契约 §6.3；验收 AC-1
// 运行：tests/run_unit_tests.sh（即 ctest；`ctest -R AuthApi` 可单跑本文件）
//
// 【分两段，理由与 session_test.cpp 相同】
//   A. 纯函数（用户名/密码校验）永远会跑、跑得飞快 —— 而它们恰好是最容易被重构
//      悄悄放宽的地方：长度改成 > 而不是 >=、字符集白名单被"顺手"去掉、
//      全空白口令被当作合法，都会让用例仍然绿但产品行为变了。
//   B. users 表的读写只有真库能验，连不上整体跳过。
//
// 【AC-1 的三条断言放在 B 段】
//   重复用户名 → 1005；错误密码 → 1001；正确登录后 Cookie 形如 `sid=…; HttpOnly;
//   SameSite=Lax; Path=/`。§8 R-20 的第二条是"一次注册后立刻验证登录，
//   不把『注册成功』当作『哈希正确』的证据" —— RegisteredUserCanLogInImmediately
//   这条用例就是那个证据的自动化版本。
//
// 【探针数据策略】与 session_test.cpp 同款：只造带 `ojauth` 前缀的用户，
//   TearDown 里按前缀删掉（sessions 由 §3.6 的外键 CASCADE 一并带走）。
//   ⚠️ 断言一律用【增量】（"多了 1 行"），不用绝对计数 —— 库里本来就有别的数据。
// =============================================================================
#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <string>

#include "common/config.h"
#include "db/mysql_pool.h"
#include "web/auth_api.h"
#include "web/bcrypt.h"

namespace {

using namespace oj;
using namespace oj::db;
using namespace oj::web;

constexpr const char* kUserPrefix = "ojauth";

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

std::string unique_name(const char* suffix) {
  // 库里可能残留上一轮跑失败的探针行；带上 pid 让每次运行的名字都不重复。
  return std::string(kUserPrefix) + std::to_string(::getpid()) + "_" + suffix;
}

// =============================================================================
// A. 纯函数：用户名 / 密码校验
// =============================================================================

TEST(AuthApiUsername, AcceptsTheLegalRangeAndCharset) {
  EXPECT_FALSE(validate_username("abc").has_value()) << "3 字符是下界，必须接受";
  EXPECT_FALSE(validate_username(std::string(32, 'a')).has_value()) << "32 字符是上界，必须接受";
  EXPECT_FALSE(validate_username("Bob_42-x").has_value());
  EXPECT_FALSE(validate_username("_-9A").has_value());
}

TEST(AuthApiUsername, RejectsOutOfRangeLengths) {
  EXPECT_TRUE(validate_username("").has_value());
  EXPECT_TRUE(validate_username("ab").has_value());
  EXPECT_TRUE(validate_username(std::string(33, 'a')).has_value());
  // SPEC §5.8 的长度区间是 3–32，正好对齐 DDL 的 VARCHAR(32)：
  // 放宽到 33 不会在插入时报错（MySQL 会截断还是报错取决于 sql_mode），
  // 而是变成一个"注册成功但名字被改了"的账号。
}

TEST(AuthApiUsername, RejectsCharactersThatWouldSilentlyCollideOrMismatch) {
  // ① 大小写：uk_username 是 utf8mb4_general_ci，"Bob" 与 "bob" 会互相撞
  EXPECT_FALSE(validate_username("Bob").has_value());
  // ② 尾部空格：MySQL 的 PAD SPACE 比较会把它当不存在
  EXPECT_TRUE(validate_username("bob ").has_value());
  // ③ 非 ASCII 与不可见字符：OJ_ADMIN_USERNAMES 是按精确字符串匹配的（T3.11）
  EXPECT_TRUE(validate_username("用户").has_value());
  EXPECT_TRUE(validate_username("bob\n").has_value());
  EXPECT_TRUE(validate_username("bo b").has_value());
}

TEST(AuthApiUsername, NamesTheOffendingField) {
  const std::optional<FieldError> error = validate_username("ab");
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->field, "username");
  EXPECT_FALSE(error->reason.empty()) << "必须有可直接展示给用户的原因";
}

TEST(AuthApiPassword, AcceptsSixToSixtyFourBytes) {
  EXPECT_FALSE(validate_password("123456").has_value()) << "6 是下界";
  EXPECT_FALSE(validate_password(std::string(64, 'x')).has_value()) << "64 是上界（§5.8）";
  EXPECT_FALSE(validate_password("  a b ").has_value()) << "允许含空白，只是不能全空白";
}

TEST(AuthApiPassword, RejectsTooShortTooLongOrAllBlank) {
  EXPECT_TRUE(validate_password("").has_value());
  EXPECT_TRUE(validate_password("12345").has_value());
  EXPECT_TRUE(validate_password(std::string(65, 'x')).has_value());
  EXPECT_TRUE(validate_password("      ").has_value());
}

// §6.2.1：口令上限 64 < bcrypt 的 72 字节密钥流，截断分支在本项目里永远不发生。
// 这条用例把两处常量的关系钉住：哪天有人把 §5.8 的上限改成 72 以上，这里立刻红。
TEST(AuthApiPassword, MaxLengthStaysBelowBcryptsSeventyTwoByteKeyStream) {
  EXPECT_LT(kPasswordMaxLength, kBcryptMaxKeyBytes)
      << "超过 72 字节的口令会被 bcrypt 静默截断 —— 少一位哈希的密码等于密码里少一位";
}

// =============================================================================
// A′. DB 故障路径（纯逻辑，不需要真库）
// =============================================================================

// 【为什么这条不放进真库组】：它要的是"会话不可用"，用一个空的 db::Session 就够，
//   而真库组的价值在于验证 SQL 与事务；把它塞进去只会多一次连接往返。
// 断连/池未就绪时必须回 1008 且 server_side=true：路由层据此记 ERROR 而不是把
// "数据库挂了"当成"用户名格式不对"回给用户。
TEST(AuthApiDbFailure, RegisterAgainstAnUnusableSessionReturns1008) {
  db::Session dead;
  AuthFailure failure;
  std::string error;
  EXPECT_FALSE(register_user(dead, unique_name("dead"), "hunter2", nullptr, &failure, &error));
  EXPECT_EQ(code(failure.error), 1008);
  EXPECT_TRUE(failure.server_side) << "DB 故障必须能被日志侧区分出来";
  EXPECT_FALSE(error.empty()) << "错误详情进 app.log 与上层，不该被吞掉";
}

TEST(AuthApiDbFailure, LoginAgainstAnUnusableSessionReturns1008) {
  db::Session dead;
  AuthFailure failure;
  std::string error;
  EXPECT_FALSE(login_user(dead, unique_name("dead"), "hunter2", nullptr, nullptr, &failure, &error));
  EXPECT_EQ(code(failure.error), 1008);
  EXPECT_TRUE(failure.server_side);
}

TEST(AuthApiDbFailure, LogoutAgainstAnUnusableSessionReportsFailure) {
  db::Session dead;
  std::string set_cookie;
  std::string error;
  EXPECT_FALSE(logout_user(dead, generate_token(), &set_cookie, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_TRUE(set_cookie.empty()) << "失败时不得写出过期 Cookie —— 否则浏览器被清了、服务端没清";
}

// =============================================================================
// B. users 表（真库；连不上整体跳过）
// =============================================================================

class AuthApiDbTest : public ::testing::Test {
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
    // 注册端点要读 OJ_ALLOW_REGISTER；未 load_config 时 config() 返回内置默认值
    // （allow_register=true），本组用例正是在这个默认下跑的。
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

  // 取一次租约、执行 fn、立刻归还。⚠️ [D-21]：同一线程第二次 acquire 会被主动拒绝，
  // 所以所有读库断言都必须走这里（与 session_test.cpp 同一条纪律）。
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

  long count_users(const std::string& username) {
    return with_session([&](db::Session& s) -> long {
      const db::ResultSet r =
          s.run("SELECT COUNT(*) FROM users WHERE username = ?", {Value::from_text(username)});
      EXPECT_FALSE(r.failed()) << r.error().format();
      return r.empty() ? -1L : static_cast<long>(r.at(0).i64(0));
    });
  }

  std::string stored_hash(const std::string& username) {
    return with_session([&](db::Session& s) -> std::string {
      const db::ResultSet r =
          s.run("SELECT password_hash FROM users WHERE username = ?", {Value::from_text(username)});
      EXPECT_FALSE(r.failed()) << r.error().format();
      return r.empty() ? std::string() : r.at(0).str(0);
    });
  }

  std::string stored_role(const std::string& username) {
    return with_session([&](db::Session& s) -> std::string {
      const db::ResultSet r =
          s.run("SELECT role FROM users WHERE username = ?", {Value::from_text(username)});
      EXPECT_FALSE(r.failed()) << r.error().format();
      return r.empty() ? std::string() : r.at(0).str(0);
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

  PoolOptions options_;
  std::unique_ptr<Pool> pool_;
};

TEST_F(AuthApiDbTest, RegistersAUserWithABcryptHashAndRoleUser) {
  const std::string username = unique_name("reg");
  AuthUserRecord user;
  AuthFailure failure;
  std::string error;
  const bool ok_registered = with_session([&](db::Session& s) {
    return register_user(s, username, "hunter2", &user, &failure, &error);
  });
  ASSERT_TRUE(ok_registered) << error;

  EXPECT_EQ(count_users(username), 1L);
  EXPECT_EQ(user.username, username);
  EXPECT_EQ(user.role, "user") << "注册【不做提权】：角色只由 T3.11 的启动同步决定（§6.3.1）";
  EXPECT_NE(user.user_id, 0ULL);
  EXPECT_EQ(stored_role(username), "user");

  // ⚠️ 落库的必须是 bcrypt 哈希，不是明文（AC-1 / R-20 的第二条）
  const std::string hash = stored_hash(username);
  EXPECT_EQ(hash.size(), kBcryptHashLength);
  EXPECT_EQ(hash.substr(0, 4), "$2b$");
  EXPECT_EQ(hash.substr(4, 2), "10") << "§6.2.1 要点 3：cost 固定 10";
  EXPECT_EQ(hash.find("hunter2"), std::string::npos) << "哈希里绝不能出现明文";
  EXPECT_TRUE(verify_password("hunter2", hash));
  EXPECT_FALSE(verify_password("hunter3", hash));
}

TEST_F(AuthApiDbTest, DuplicateUsernameReturns1005) {
  const std::string username = unique_name("dup");
  AuthFailure failure;
  std::string error;
  const bool first = with_session([&](db::Session& s) {
    return register_user(s, username, "hunter2", nullptr, &failure, &error);
  });
  ASSERT_TRUE(first) << error;

  AuthFailure second_failure;
  const bool second = with_session([&](db::Session& s) {
    return register_user(s, username, "other-pw", nullptr, &second_failure, &error);
  });
  EXPECT_FALSE(second);
  EXPECT_EQ(code(second_failure.error), 1005) << "AC-1：重复用户名必须回 1005";
  EXPECT_FALSE(second_failure.server_side) << "这是用户输入问题，不该按服务端故障记 ERROR";
  EXPECT_EQ(count_users(username), 1L) << "判重后不能再插第二行";
}

TEST_F(AuthApiDbTest, InvalidInputReturns1001WithTheOffendingFieldAndWritesNothing) {
  AuthFailure username_failure;
  std::string error;
  const bool short_name = with_session([&](db::Session& s) {
    return register_user(s, "ab", "hunter2", nullptr, &username_failure, &error);
  });
  EXPECT_FALSE(short_name);
  EXPECT_EQ(code(username_failure.error), 1001);
  EXPECT_EQ(username_failure.field.field, "username");

  AuthFailure password_failure;
  const bool short_password = with_session([&](db::Session& s) {
    return register_user(s, unique_name("shortpw"), "123", nullptr, &password_failure, &error);
  });
  EXPECT_FALSE(short_password);
  EXPECT_EQ(code(password_failure.error), 1001);
  EXPECT_EQ(password_failure.field.field, "password");

  EXPECT_EQ(count_users(unique_name("shortpw")), 0L) << "校验失败必须【不落库】";
}

// §8 R-20 的第二条：注册成功不等于哈希正确 —— 紧接着用同一套凭证走一遍登录。
TEST_F(AuthApiDbTest, RegisteredUserCanLogInImmediately) {
  const std::string username = unique_name("login");
  AuthUserRecord registered;
  AuthFailure failure;
  std::string error;
  ASSERT_TRUE(with_session([&](db::Session& s) {
    return register_user(s, username, "correct horse", &registered, &failure, &error);
  })) << error;

  AuthUserRecord logged_in;
  std::string set_cookie;
  const bool ok_logged_in = with_session([&](db::Session& s) {
    return login_user(s, username, "correct horse", &logged_in, &set_cookie, &failure, &error);
  });
  ASSERT_TRUE(ok_logged_in) << error;

  EXPECT_EQ(logged_in.user_id, registered.user_id);
  EXPECT_EQ(logged_in.username, username);
  EXPECT_EQ(logged_in.role, "user");
  EXPECT_EQ(count_sessions(registered.user_id), 1L) << "登录必须真的落一条会话行（§6.3.2）";

  // Set-Cookie 的四个属性逐字核对（§6.3 / [D-69]）：名字、`HttpOnly`、`SameSite=Lax`、`Path=/`
  EXPECT_EQ(set_cookie.rfind("sid=", 0), 0U) << set_cookie;
  EXPECT_NE(set_cookie.find("; HttpOnly;"), std::string::npos) << set_cookie;
  EXPECT_NE(set_cookie.find("; SameSite=Lax;"), std::string::npos) << set_cookie;
  EXPECT_NE(set_cookie.find("; Path=/"), std::string::npos) << set_cookie;
  EXPECT_EQ(set_cookie.find("Secure"), std::string::npos)
      << "首版是明文 HTTP，加了 Secure 浏览器永不下发（§6.3.2.1 的三条欠账之一）";

  // Cookie 里的 token 必须是 64 位小写 hex，且能原样解析回来（session 层）
  const std::string sid = set_cookie.substr(4, set_cookie.find(';') - 4);
  EXPECT_TRUE(is_token_wellformed(sid)) << sid;
  const std::optional<AuthUser> resolved = with_session([&](db::Session& s) -> std::optional<AuthUser> {
    return authenticate(s, "sid=" + sid, &error);
  });
  ASSERT_TRUE(resolved.has_value()) << "拿登录刚发的 Cookie 反查会话必须成功";
  EXPECT_EQ(resolved->user_id, registered.user_id);
}

TEST_F(AuthApiDbTest, WrongPasswordAndUnknownUserBothReturn1001) {
  const std::string username = unique_name("wrongpw");
  AuthUserRecord user;
  AuthFailure failure;
  std::string error;
  ASSERT_TRUE(with_session([&](db::Session& s) {
    return register_user(s, username, "hunter2", &user, &failure, &error);
  })) << error;

  AuthFailure wrong_password_failure;
  const bool wrong_password = with_session([&](db::Session& s) {
    return login_user(s, username, "hunter3", nullptr, nullptr, &wrong_password_failure, &error);
  });
  EXPECT_FALSE(wrong_password);
  EXPECT_EQ(code(wrong_password_failure.error), 1001) << "AC-1：错误密码必须回 1001";
  EXPECT_EQ(wrong_password_failure.field.field, "password");
  EXPECT_EQ(count_sessions(user.user_id), 0L) << "密码错时不得留下任何会话行";

  AuthFailure unknown_failure;
  const bool unknown = with_session([&](db::Session& s) {
    return login_user(s, unique_name("ghost"), "hunter2", nullptr, nullptr, &unknown_failure, &error);
  });
  EXPECT_FALSE(unknown);
  // ⚠️ 两种情况的码与文案【完全一致】：区分开就等于给用户名枚举开了个接口
  //   （见 auth_api.h【登录为什么不给"用户不存在"和"密码错误"分别的文案】）。
  EXPECT_EQ(code(unknown_failure.error), code(wrong_password_failure.error));
  EXPECT_EQ(unknown_failure.field.reason, wrong_password_failure.field.reason);
}

TEST_F(AuthApiDbTest, LogoutDeletesTheSessionRowAndReturnsAnExpiringCookie) {
  const std::string username = unique_name("logout");
  AuthUserRecord user;
  AuthFailure failure;
  std::string error;
  ASSERT_TRUE(with_session([&](db::Session& s) {
    return register_user(s, username, "hunter2", &user, &failure, &error);
  })) << error;

  std::string set_cookie;
  ASSERT_TRUE(with_session([&](db::Session& s) {
    return login_user(s, username, "hunter2", nullptr, &set_cookie, &failure, &error);
  })) << error;
  const std::string sid = set_cookie.substr(4, set_cookie.find(';') - 4);
  ASSERT_EQ(count_sessions(user.user_id), 1L);

  std::string clear_cookie;
  const bool ok_logged_out = with_session([&](db::Session& s) {
    return logout_user(s, sid, &clear_cookie, &error);
  });
  EXPECT_TRUE(ok_logged_out) << error;
  EXPECT_EQ(count_sessions(user.user_id), 0L);
  EXPECT_EQ(clear_cookie, make_clear_cookie());
  EXPECT_EQ(clear_cookie.rfind("sid=;", 0), 0U) << clear_cookie;
  EXPECT_NE(clear_cookie.find("Max-Age=0"), std::string::npos) << clear_cookie;

  // 幂等：再登出一次仍然成功（前端连点两次不该报错）
  std::string again;
  EXPECT_TRUE(with_session([&](db::Session& s) { return logout_user(s, sid, &again, &error); }));
  // 形状非法的 token 也不报错 —— 删不掉就是 0 行
  EXPECT_TRUE(with_session([&](db::Session& s) { return logout_user(s, "not-a-token", &again, &error); }));

  // AC-1 第三条：「登出后旧 Cookie 返回 1002」—— 行删了还不够，必须证明
  // 【拿这个旧 Cookie 再也查不到登录身份】。只断言 count==0 的话，
  // "删了行但查询路径仍认它"这种回归会漏过去。
  const std::optional<AuthUser> after_logout = with_session(
      [&](db::Session& s) -> std::optional<AuthUser> { return authenticate(s, "sid=" + sid, &error); });
  EXPECT_FALSE(after_logout.has_value())
      << "登出后旧 Cookie 必须查不到身份（路由层据此回 1002，AC-1）";
  EXPECT_TRUE(error.empty()) << "查不到是【未登录】而不是【服务端故障】";
}

// §6.3 端点表：「register 入口先查 OJ_ALLOW_REGISTER，为 0 时返回 1010 且不落库」。
// 策略做成 RegisterPolicy 递进来（见 auth_api.h 的说明），因为 config() 是进程级
// 只读快照 —— 单测改它会污染同进程其它模块的结论。
TEST_F(AuthApiDbTest, RegisterClosedReturns1010AndWritesNothing) {
  const std::string username = unique_name("closed");
  RegisterPolicy closed;
  closed.allow_register = false;

  AuthFailure failure;
  std::string error;
  const bool ok_registered = with_session([&](db::Session& s) {
    return register_user(s, closed, username, "hunter2", nullptr, &failure, &error);
  });
  EXPECT_FALSE(ok_registered);
  EXPECT_EQ(code(failure.error), 1010);
  EXPECT_FALSE(failure.server_side) << "这是运维策略，不是故障";
  EXPECT_EQ(count_users(username), 0L) << "§6.3 明确要求 1010 时【不落库】";
  EXPECT_TRUE(error.empty()) << "1010 是预期内的拒绝，不该有服务端错误详情";

  // ⚠️ 顺序也是契约的一部分：注册关闭时连参数校验都不做。
  //   若哪天有人把开关挪到校验之后，"用户名为空"就会在关闭状态下仍返回 1001，
  //   于是外部能靠错误码反推"这里曾经开放注册"。
  AuthFailure invalid_failure;
  const bool invalid = with_session([&](db::Session& s) {
    return register_user(s, closed, "ab", "1", nullptr, &invalid_failure, &error);
  });
  EXPECT_FALSE(invalid);
  EXPECT_EQ(code(invalid_failure.error), 1010) << "开关必须【先于】参数校验（§6.3 原文）";

  // 反过来：开关打开时同一条路径必须真的注册成功，否则上面两条可能一起"恒真"
  RegisterPolicy open;
  open.allow_register = true;
  EXPECT_TRUE(with_session([&](db::Session& s) {
    return register_user(s, open, username, "hunter2", nullptr, &failure, &error);
  })) << error;
  EXPECT_EQ(count_users(username), 1L);
}

// 「我是谁」的闭环：登录发的那个 Cookie → 反查登录身份 → 序列化响应体，
// 三个字段必须与注册时同一组值。路由层（T3.5）只做包络拼接，这三行的形状由这里定。
TEST_F(AuthApiDbTest, MePayloadFromTheLoginCookieMatchesTheRegisteredUser) {
  const std::string username = unique_name("me");
  AuthUserRecord registered;
  AuthFailure failure;
  std::string error;
  ASSERT_TRUE(with_session([&](db::Session& s) {
    return register_user(s, username, "hunter2", &registered, &failure, &error);
  })) << error;

  std::string set_cookie;
  ASSERT_TRUE(with_session([&](db::Session& s) {
    return login_user(s, username, "hunter2", nullptr, &set_cookie, &failure, &error);
  })) << error;
  const std::string cookie_header = set_cookie.substr(0, set_cookie.find("; Path=/"));

  const std::optional<AuthUser> viewer = with_session(
      [&](db::Session& s) -> std::optional<AuthUser> { return authenticate(s, cookie_header, &error); });
  ASSERT_TRUE(viewer.has_value()) << error;
  EXPECT_EQ(viewer->user_id, registered.user_id);
  EXPECT_EQ(viewer->username, registered.username);
  EXPECT_EQ(viewer->role, "user");

  const std::string payload = user_payload_json(*viewer);
  EXPECT_NE(payload.find("\"userId\":" + std::to_string(registered.user_id)), std::string::npos)
      << payload;
  EXPECT_NE(payload.find("\"username\":\"" + username + "\""), std::string::npos) << payload;
  EXPECT_NE(payload.find("\"role\":\"user\""), std::string::npos) << payload;
}

// 未登录访问 /api/auth/me 的判定在会话层（web::authenticate 返回 nullopt），
// 路由层据此回 1002 —— 端到端的 1002 要等 T3.5 的包络才验得了，这里钉住它的前提。
TEST_F(AuthApiDbTest, NoCookieMeansNoViewer) {
  std::string error;
  EXPECT_FALSE(
      with_session([&](db::Session& s) -> std::optional<AuthUser> { return authenticate(s, "", &error); })
          .has_value());
  EXPECT_TRUE(error.empty()) << "未登录不是错误（[D-43]：匿名可读题面），error 必须保持空";
}

TEST_F(AuthApiDbTest, UserPayloadJsonUsesTheCamelCaseContractOfSection63) {
  AuthUser user;
  user.user_id = 42;
  user.username = "bob";
  user.role = "admin";
  const std::string payload = user_payload_json(user);
  EXPECT_NE(payload.find("\"userId\":42"), std::string::npos) << payload;
  EXPECT_NE(payload.find("\"username\":\"bob\""), std::string::npos) << payload;
  EXPECT_NE(payload.find("\"role\":\"admin\""), std::string::npos) << payload;
}

}  // namespace
