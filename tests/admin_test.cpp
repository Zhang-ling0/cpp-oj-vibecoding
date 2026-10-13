// =============================================================================
// 只读管理端点（include/web/admin.h + src/web/admin.cpp）的单元测试
// 被测：SPEC.md §9 M3 T3.12 `[D-37]`；契约 §6.3.3，口径 §6.4 / §6.5
// 运行：tests/run_unit_tests.sh（即 ctest；`ctest -R Admin` 可单跑本文件）
//
// 【被测的两个接口，其余函数都是为它们服务的】
//   ① require_admin()：放行守卫。全部决策都在这一行代码里，且【顺序】本身就是契约
//      （1002 先于 1003）—— 它不碰 DB、不读 Cookie、没有任何外部状态，
//      所以这一组永远会跑、跑得又快，是最该被钉死的那部分。
//   ② build_overview()：概览组装，含 to_json() 序列化与两条聚合 SQL。
//      它有两个重载：provider 重载（生产路径，先快照后租约）与 session 重载（已知快照）。
//      这一组的重点不是"数得对"，而是四件容易写错且不会立刻报错的事：
//      锁的先后、失败时 out 不变式、运行态三项不被 DB 覆盖、字段名与可空字段的形状。
//
// 【分两段，理由与 mysql_pool_test.cpp 相同】
//   守卫 / 序列化 / 锁顺序 / 失败路径 是纯计算，不需要库，永远会跑 —— 它们恰好是本模块里
//   【最容易在重构中被悄悄改掉】的几样（守卫的先后顺序、字段名、快照与租约的先后）。
//   聚合 SQL 的数值正确性只有真库能验，连不上就整体 GTEST_SKIP，不标红。
//
// 【探针数据策略：不建独立库，只在配置的库里造带前缀的行并在 TearDown 删净】
//   建独立库需要账号有 CREATE DATABASE 权限，而 oj 账号按 §6.7.3 只有库内权限；
//   为跑测试去扩权限是更大的副作用。带前缀（ojadm / ojadm-probe-）+ 级联删除足够隔离：
//   users 侧的 submissions / user_problems / sessions 由 CASCADE 带走（§3.6），
//   problems 侧的 problem_cases 由 CASCADE 带走。
//   断言用【增量】而不是绝对值：库里本来就题库数据（T1.2 同步进来两道题），
//   写死绝对值会让这个文件在题库一变就红 —— 那是测试对环境的依赖，不是回归。
//   ⚠️ 增量断言只在【ctest 串行】下成立（Makefile 已按 2 核机禁了 -j）：若另一个进程
//   边跑边插行，baseline 与实际值之差就不再是本用例造的那几条。真要并发跑，
//   得先把 baseline 换成"只数带前缀的行"这种与他人无关的口径。
// =============================================================================
#include <gtest/gtest.h>

#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "db/mysql_pool.h"
#include "web/admin.h"

namespace {

using namespace oj;
using namespace oj::db;
using namespace oj::web;

constexpr const char* kUserPrefix = "ojadm";
constexpr const char* kProblemPrefix = "ojadm-probe-";

std::string env_or(const char* key, const char* fallback) {
  const char* value = std::getenv(key);
  return (value != nullptr && *value != '\0') ? std::string(value) : std::string(fallback);
}

db::PoolOptions base_options() {
  db::PoolOptions o;
  o.host = env_or("MYSQL_HOST", "127.0.0.1");
  o.port = static_cast<unsigned int>(std::atoi(env_or("MYSQL_PORT", "3306").c_str()));
  o.user = env_or("MYSQL_USER", "oj");
  o.password = env_or("MYSQL_PASSWORD", "oj");
  o.database = env_or("MYSQL_DB", "oj");
  return o;
}

// =============================================================================
// A. 接口 ① require_admin 守卫（纯逻辑，永远会跑）：判定顺序、角色封闭、错误码表
// =============================================================================

TEST(AdminGuard, UnauthenticatedBeatsForbidden) {
  // §6.3.3：先判登录再判角色。没登录的人携带了别人的 Cookie 时，正确答案是 1002，
  // 不是 1003 —— 后者会让他以为自己登录了却没权限。
  const std::optional<AuthUser> none;
  EXPECT_EQ(require_admin(none), GuardResult::kUnauthenticated);
  EXPECT_EQ(error_code_of(GuardResult::kUnauthenticated), 1002);
}

TEST(AdminGuard, PlainUserIsForbidden) {
  const AuthUser bob{"bob", "user"};
  EXPECT_EQ(require_admin(bob), GuardResult::kForbidden);
  EXPECT_EQ(error_code_of(GuardResult::kForbidden), 1003);
}

TEST(AdminGuard, AdminIsAllowed) {
  const AuthUser alice{"alice", "admin"};
  EXPECT_EQ(require_admin(alice), GuardResult::kAllowed);
  EXPECT_EQ(error_code_of(GuardResult::kAllowed), 0);
  EXPECT_STREQ(error_message_of(GuardResult::kAllowed), "ok");
}

TEST(AdminGuard, UnknownRoleIsNotAdmin) {
  // role 列是 ENUM('user','admin')，但守卫是【默认值封闭】的：任何非 "admin" 一律 403。
  // 将来若加了第三个角色，这里会立刻暴露，而不是悄悄放行或悄悄拦掉。
  for (const char* role : {"", "user", "Admin", "ADMIN", "administrator", " root"}) {
    const AuthUser u{"u", role};
    EXPECT_EQ(require_admin(u), GuardResult::kForbidden) << "role=" << role;
  }
}

TEST(AdminGuard, ErrorTableMatchesTheContractOfSection63) {
  // §6.3 的错误码表：本守卫只出 0 / 1002 / 1003 三个业务码，中文文案也在这里定死。
  // 连 message 一起断，是因为路由层（T3.5）会把文案原样回给前端 ——
  // 文案漂了不会让任何代码崩、不会让任何测试红，只会在某天让人看到一句错话。
  struct Row {
    GuardResult result;
    int code;
    const char* message;
  };
  for (const Row& row : {Row{GuardResult::kAllowed, 0, "ok"},
                         Row{GuardResult::kUnauthenticated, 1002, "未登录或会话已过期"},
                         Row{GuardResult::kForbidden, 1003, "无权限"}}) {
    EXPECT_EQ(error_code_of(row.result), row.code);
    EXPECT_STREQ(error_message_of(row.result), row.message);
  }
}

TEST(AdminGuard, RoleIsTheOnlyFieldTheDecisionLooksAt) {
  // optional 有值 == 会话层已经确认"这个人登录了"（Cookie 解析、过期检查都在 T3.3）。
  // 于是 username 空不空、长多长、带什么怪字符，都与放行无关 —— 它只是要显示的字符串。
  // 反过来【有值但 role 为空】是 1003 而不是 1002：会话确实存在，缺的是角色不是登录态。
  // role 列是 ENUM，空角色在生产里进不来；写在这里是为了把边界钉死在测试里。
  EXPECT_EQ(require_admin(AuthUser{"", "admin"}), GuardResult::kAllowed);
  EXPECT_EQ(require_admin(AuthUser{"名字里有\"引号\"和\\反斜杠", "admin"}), GuardResult::kAllowed);
  EXPECT_EQ(require_admin(AuthUser{"", ""}), GuardResult::kForbidden);
  EXPECT_EQ(require_admin(std::optional<AuthUser>{}), GuardResult::kUnauthenticated);
}

// =============================================================================
// B. 接口 ② build_overview 的纯逻辑面：锁顺序、失败不变式、序列化契约
//    （这些不碰真库，因此 mysql 没起来时它们照样跑、照样能挡住回归）
// =============================================================================

TEST(AdminJson, UsesCamelCaseKeysOfTheContract) {
  Overview overview;
  overview.system.user_count = 3;
  overview.system.admin_count = 1;
  overview.system.queue_length = 7;
  overview.system.active_judges = 2;
  overview.system.judge_alive = true;
  AdminUserRow row;
  row.username = "alice";
  row.role = "admin";
  row.created_at = "2026-10-11 08:00:00";
  row.solved = 5;
  row.attempted = 9;
  row.last_submit_at = "2026-10-11 09:00:00";
  row.last_verdict = "AC";
  overview.users.push_back(row);

  const nlohmann::json parsed = nlohmann::json::parse(to_json(overview));
  // §6.3.3 的 system 字段表逐个对照，少一个/多一个都是契约漂移
  for (const char* key : {"userCount", "adminCount", "problemCount", "caseCount",
                          "submissionCount", "acCount", "failedCount", "judgingCount",
                          "queueLength", "activeJudges", "judgeAlive"}) {
    EXPECT_TRUE(parsed["system"].contains(key)) << "system 缺少字段 " << key;
  }
  ASSERT_EQ(parsed["system"].size(), 11u);
  EXPECT_EQ(parsed["system"]["queueLength"], 7);
  EXPECT_EQ(parsed["system"]["judgeAlive"], true);

  ASSERT_EQ(parsed["users"].size(), 1u);
  const nlohmann::json& user = parsed["users"][0];
  for (const char* key : {"username", "role", "createdAt", "submissionCount", "acCount",
                          "solved", "attempted", "favorites", "lastSubmitAt", "lastVerdict"}) {
    EXPECT_TRUE(user.contains(key)) << "users[] 缺少字段 " << key;
  }
  EXPECT_EQ(user["solved"], 5);
  EXPECT_EQ(user["lastVerdict"], "AC");
}

TEST(AdminJson, EmptyOptionalFieldsBecomeJsonNull) {
  // 「从未提交」与「提交了但结论未知」在前端是两件事，必须能区分开
  Overview overview;
  AdminUserRow row;
  row.username = "bob";
  overview.users.push_back(row);
  const nlohmann::json parsed = nlohmann::json::parse(to_json(overview));
  const nlohmann::json& user = parsed["users"][0];
  EXPECT_TRUE(user["lastSubmitAt"].is_null());
  EXPECT_TRUE(user["lastVerdict"].is_null());
  EXPECT_EQ(user["solved"], 0);
}

TEST(AdminLockOrder, SnapshotIsTakenBeforeTheDatabaseLease) {
  // §6.3.3 实现约束 2：先取调度器快照并释放其锁，再取 DB 租约。
  // 反过来就多出一条 DB mutex → scheduler lock 的嵌套边。
  // 本例故意让 DB 不可用（进程级单例没初始化），于是只能观察到一件事：
  // provider 【确实被调用过】—— 即便如此，它也发生在租约失败之前。
  bool provider_called = false;
  Overview overview;
  std::string error;
  const bool ok = build_overview(
      [&provider_called]() {
        provider_called = true;
        JudgeRuntimeStatus status;
        status.queue_length = 5;
        return status;
      },
      &overview, &error);
  if (ok) {
    // 连接池恰好就绪（有人先初始化了单例）时，走的是正常路径
    EXPECT_TRUE(provider_called);
    EXPECT_EQ(overview.system.queue_length, 5);
  } else {
    EXPECT_TRUE(provider_called) << "DB 失败前就应已取过快照，锁顺序被倒过来了：" << error;
    EXPECT_FALSE(error.empty());
  }
}

TEST(AdminLockOrder, UnwiredProviderReportsJudgeNotAlive) {
  // judge 未接线（T2.7 之前）时必须【如实】说没起来，不能给出一个"看起来正常"的 0
  const JudgeRuntimeStatus status = unwired_status_provider()();
  EXPECT_FALSE(status.judge_alive);
  EXPECT_EQ(status.queue_length, 0);
  EXPECT_EQ(status.active_judges, 0);
}

TEST(AdminOverview, FailedQueryIsReportedAndLeavesOutUntouched) {
  // 默认构造的 Session 没有连接，run() 是【惰性失败】（见 db/mysql_pool.h 的说明），
  // 因此不需要真库就能走通失败路径 —— 而失败路径才是容易写出"半成品结果"的地方。
  db::Session dead;
  JudgeRuntimeStatus status;
  status.judge_alive = true;
  Overview overview;
  overview.system.user_count = 999;   // 不变式的反例素材
  std::string error;
  EXPECT_FALSE(build_overview(dead, status, &overview, &error));
  EXPECT_FALSE(error.empty());
  // 失败时 out 一个字节都不能被改：调用方会拿着旧值继续用，静默半更新比报错难查得多
  EXPECT_EQ(overview.system.user_count, 999);
  EXPECT_TRUE(overview.users.empty());
}

TEST(AdminOverview, EachLoaderKeepsItsOwnOutUntouchedOnFailure) {
  // 上面那条从 build_overview 走，是"聚合后的表现"；这里直接断两个 loader 自己的不变式，
  // 因为它们各自是公开入口（路由层将来可能单独用），而且失败是【各自独立】的 ——
  // system 成功、users 失败时，*out 必须整体不动，绝不能留下"半份快照"。
  db::Session dead;

  SystemBlock counts;
  counts.user_count = 42;
  counts.judging_count = 7;
  std::string error;
  EXPECT_FALSE(load_system_counts(dead, &counts, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(counts.user_count, 42);
  EXPECT_EQ(counts.judging_count, 7);

  std::vector<AdminUserRow> rows;
  AdminUserRow kept;
  kept.username = "kept";
  rows.push_back(kept);
  error.clear();
  EXPECT_FALSE(load_users(dead, &rows, &error));
  EXPECT_FALSE(error.empty());
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].username, "kept");
}

TEST(AdminOverview, EmptyProviderIsTreatedAsUnwiredInsteadOfThrowing) {
  // provider 理论上永远非空（接线时至少传 unwired_status_provider()）。但空 std::function
  // 若被直接调用会抛 std::bad_function_call，而抛异常的守卫/组装函数会让上层 500。
  // 这里断言它被当成"未接线"处理：先走 unwired_status()，再因取不到租约而正常失败。
  // ⚠️ 进程级单例必须处于未就绪，否则走不到失败分支 —— 故显式制造这个前提。
  if (Pool::instance().ready()) Pool::instance().shutdown();

  const StatusProvider empty_provider;
  Overview overview;
  overview.system.user_count = 7;
  std::string error;
  EXPECT_NO_THROW(EXPECT_FALSE(build_overview(empty_provider, &overview, &error)));
  EXPECT_EQ(overview.system.user_count, 7);   // 失败仍然不改 out
  EXPECT_FALSE(error.empty());
}

TEST(AdminJson, EmptyOverviewStillCarriesTheFullContractShape) {
  // 前端会直接 users.map(...)：users 序列化成 null 时页面整个白屏，而不是"列表为空"，
  // 这种错没有任何编译期或运行期信号。judgeAlive 同理 —— 判题没起来时它必须是
  // 【存在的 false】，缺失/null 与 false 在"judge 挂了"这件事上不可区分。
  const nlohmann::json parsed = nlohmann::json::parse(to_json(Overview{}));
  ASSERT_TRUE(parsed.is_object());
  EXPECT_EQ(parsed.size(), 2u);
  ASSERT_TRUE(parsed["users"].is_array());
  EXPECT_EQ(parsed["users"].size(), 0u);
  ASSERT_TRUE(parsed["system"].is_object());
  EXPECT_EQ(parsed["system"].size(), 11u);
  EXPECT_EQ(parsed["system"]["userCount"], 0);
  EXPECT_TRUE(parsed["system"]["judgeAlive"].is_boolean());
  EXPECT_EQ(parsed["system"]["judgeAlive"], false);
}

TEST(AdminJson, CountsSurviveTheInt64Boundary) {
  // 计数列是 BIGINT。走到 int64 上界时，若中途被当成 double 或被字符串化，
  // 页面上就会出现 9223372036854775808 这种多一位的数 —— 不崩溃、不报错、只是不对。
  const long long biggest = std::numeric_limits<long long>::max();
  Overview overview;
  overview.system.user_count = biggest;
  overview.system.queue_length = biggest;
  overview.system.active_judges = 0;
  const nlohmann::json parsed = nlohmann::json::parse(to_json(overview));
  ASSERT_TRUE(parsed["system"]["userCount"].is_number_integer());
  EXPECT_EQ(parsed["system"]["userCount"].get<long long>(), biggest);
  EXPECT_EQ(parsed["system"]["queueLength"].get<long long>(), biggest);
}

TEST(AdminJson, AwkwardStringsSurviveTheRoundTrip) {
  // 手拼字符串拼 JSON 时，这几个字符就是要命的：引号截断、反斜杠吃掉后面的字符、
  // 控制字符直接产出非法 JSON、多字节文本可能被按字节截断。
  // 本例是"为什么用 nlohmann::json 而不是自己 sprintf"的回归。
  Overview overview;
  AdminUserRow row;
  row.username = "oj\"adm\\in\n\tbell\x01 中文 🙂";
  row.role = "user";
  row.created_at = "2026-10-13 09:00:00";
  overview.users.push_back(row);

  const nlohmann::json parsed = nlohmann::json::parse(to_json(overview));
  ASSERT_EQ(parsed["users"].size(), 1u);
  EXPECT_EQ(parsed["users"][0]["username"].get<std::string>(), row.username);
  EXPECT_EQ(parsed["users"][0]["role"].get<std::string>(), "user");
}

// =============================================================================
// C. 接口 ② 的真库面：两条聚合 SQL、solved/attempted 口径、users[] 顺序、
//    以及生产路径 build_overview(provider, ...) 的单例池实跑（连不上整体跳过）
// =============================================================================

class AdminDbTest : public ::testing::Test {
 protected:
  void SetUp() override {
    options_ = base_options();
    Pool probe;
    if (!probe.init(options_)) {
      GTEST_SKIP() << "无法连接 MySQL " << options_.host << ":" << options_.port
                   << "/" << options_.database
                   << "，跳过依赖真实数据库的用例（设置 MYSQL_* 可指定）";
    }
    probe.shutdown();

    pool_.reset(new Pool());
    ASSERT_TRUE(pool_->init(options_)) << pool_->describe();

    Lease lease = pool_->acquire();
    ASSERT_TRUE(lease.valid());
    // 表必须已经由 §2.1-A 的启动建表建好；没有就跳过而不是报错 ——
    // "还没跑到 T0.3" 与 "本模块的 SQL 有问题" 是两件事，不该共用一个红色。
    if (!tables_exist(lease.session())) {
      pool_->shutdown();
      pool_.reset();
      GTEST_SKIP() << "当前库缺少 §3.2 的表，请先执行 ./build/oj 完成启动建表后再跑本组用例";
    }
    clear_probe_rows(lease.session());
    ASSERT_TRUE(load_system_counts(lease.session(), &baseline_, nullptr));
  }

  void TearDown() override {
    // 单例池先关：本组新增的 provider 重载用例会初始化它，而 shutdown() 会等待
    // 正在进行的租约结束 —— 必须在下面那次 acquire() 之前，不能交叉。
    if (singleton_ready_ && Pool::instance().ready()) Pool::instance().shutdown();
    if (pool_ != nullptr && pool_->ready()) {
      Lease lease = pool_->acquire();
      if (lease.valid()) clear_probe_rows(lease.session());
    }
    if (pool_ != nullptr) {
      pool_->shutdown();
      pool_.reset();
    }
  }

  // 生产代码只走进程级单例（[D-21] 的互斥量在进程级，Pool::instance() 是唯一入口），
  // 所以 build_overview(provider,...) 那条真实路径只能这样驱动它。返回 false 时
  // 该用例应当 ASSERT 失败而不是继续 —— 与"本组的 SQL 有问题"要分开。
  bool init_singleton_pool() {
    if (!Pool::instance().init(options_)) return false;
    singleton_ready_ = true;
    return true;
  }

  static bool tables_exist(db::Session& session) {
    const db::ResultSet r = session.run(
        "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = DATABASE()"
        " AND table_name IN ('users','problems','problem_cases','submissions','user_problems')");
    return !r.failed() && !r.empty() && r.at(0).u64(0) == 5;
  }

  static void clear_probe_rows(db::Session& session) {
    session.run(std::string("DELETE FROM users WHERE username LIKE '") + kUserPrefix + "%'");
    session.run(std::string("DELETE FROM problems WHERE slug LIKE '") + kProblemPrefix + "%'");
  }

  // 造一个用户（role 可选）。返回 user id。
  unsigned long long make_user(const std::string& suffix, const std::string& role) {
    Lease lease = pool_->acquire();
    EXPECT_TRUE(lease.valid());
    const db::ResultSet r = lease->run(
        "INSERT INTO users (username, password_hash, role, created_at)"
        " VALUES (?, 'x', ?, UTC_TIMESTAMP())",
        {db::Value::from_text(std::string(kUserPrefix) + suffix), db::Value::from_text(role)});
    EXPECT_FALSE(r.failed()) << r.error().format();
    return r.insert_id();
  }

  // 造一道题 + count 条用例，返回 problem id。
  unsigned long long make_problem(const std::string& suffix, int case_count) {
    Lease lease = pool_->acquire();
    EXPECT_TRUE(lease.valid());
    const db::ResultSet r = lease->run(
        "INSERT INTO problems (slug, title, difficulty, statement_md, signature, method_name,"
        " return_type, code_template, case_count, created_at, updated_at)"
        " VALUES (?, 'probe', 'easy', 's', 'class Solution {};', 'f', 'int', '', ?, "
        "         UTC_TIMESTAMP(), UTC_TIMESTAMP())",
        {db::Value::from_text(std::string(kProblemPrefix) + suffix),
         db::Value::from_u64(static_cast<unsigned long long>(case_count))});
    EXPECT_FALSE(r.failed()) << r.error().format();
    const unsigned long long problem_id = r.insert_id();
    for (int i = 0; i < case_count; ++i) {
      const db::ResultSet c = lease->run(
          "INSERT INTO problem_cases (problem_id, case_idx, args_json, expected_json)"
          " VALUES (?, ?, '[]', '[]')",
          {db::Value::from_u64(problem_id), db::Value::from_u64(static_cast<unsigned long long>(i))});
      EXPECT_FALSE(c.failed()) << c.error().format();
    }
    return problem_id;
  }

  // 造一条提交。verdict 传空串 = NULL：pending / judging 的提交 verdict 就是 NULL，
  // 而"最新一条还没结论"正是 last_verdict 最容易写错的场景（见 src/web/admin.cpp 文件头）。
  // created_at 传空串 = 用服务端"当前时间减 1 分钟"；传固定值则是为了验证时间列的
  // 透传口径（§2.2 / [D-17]：库里存 UTC，本层一律原样透传，本地化是前端的活）。
  // ⚠️ 两条 SQL 都是【字面量常量】，不把 created_at 拼进语句 —— 拼接会让"参数化"这条
  //   约定在测试里就先破一次，而测试是最容易被抄走的地方。
  void make_submission(unsigned long long user_id, unsigned long long problem_id,
                       const std::string& status, const std::string& verdict,
                       const std::string& created_at = "") {
    Lease lease = pool_->acquire();
    EXPECT_TRUE(lease.valid());
    std::vector<db::Value> params = {db::Value::from_u64(user_id),
                                     db::Value::from_u64(problem_id),
                                     db::Value::from_text("class Solution{};"),
                                     db::Value::from_text(status)};
    if (verdict.empty()) {
      params.emplace_back(db::Value::null());
    } else {
      params.emplace_back(db::Value::from_text(verdict));
    }
    const char* kColumns = "INSERT INTO submissions (user_id, problem_id, language, code,"
                           " status, verdict, created_at) VALUES (?, ?, 'cpp', ?, ?, ?, ";
    std::string sql = kColumns;
    if (created_at.empty()) {
      sql += "UTC_TIMESTAMP() - INTERVAL 1 MINUTE)";
    } else {
      sql += "?)";
      params.emplace_back(db::Value::from_text(created_at));
    }
    const db::ResultSet r = lease->run(sql, params);
    EXPECT_FALSE(r.failed()) << r.error().format();
  }

  void set_favorite(unsigned long long user_id, unsigned long long problem_id, bool favorite) {
    Lease lease = pool_->acquire();
    EXPECT_TRUE(lease.valid());
    const db::ResultSet r = lease->run(
        "INSERT INTO user_problems (user_id, problem_id, favorite, updated_at)"
        " VALUES (?, ?, ?, UTC_TIMESTAMP())"
        " ON DUPLICATE KEY UPDATE favorite = VALUES(favorite)",
        {db::Value::from_u64(user_id), db::Value::from_u64(problem_id),
         db::Value::from_bool(favorite)});
    EXPECT_FALSE(r.failed()) << r.error().format();
  }

  const AdminUserRow* find_user(const std::vector<AdminUserRow>& rows,
                                const std::string& username) {
    for (const AdminUserRow& row : rows) {
      if (row.username == username) return &row;
    }
    return nullptr;
  }

  PoolOptions options_;
  std::unique_ptr<Pool> pool_;
  bool singleton_ready_ = false;
  SystemBlock baseline_;
};

TEST_F(AdminDbTest, SystemCountsFollowTheContract) {
  const unsigned long long admin = make_user("_sys_admin", "admin");
  make_user("_sys_plain", "user");
  const unsigned long long problem = make_problem("-sys", 2);   // → case_count +2
  ASSERT_NE(problem, 0u);
  make_submission(admin, problem, "done", "AC");    // submission +1 / ac +1
  make_submission(admin, problem, "done", "WA");    // submission +1
  make_submission(admin, problem, "failed", "");   // status=failed、verdict NULL
  make_submission(admin, problem, "pending", "");  // 未完成（pending 也算）
  // 下面两行是为了让【各列的增量两两不同】，否则"两列写反了"这种回归测不出来：
  //   两条 AC → ac +2（若与 failed 的 +1 对调，断言立刻错位）
  //   一条 judging → 未完成共 +2（若把 IN ('pending','judging') 写成 = 'pending' 就只剩 +1）
  make_submission(admin, problem, "done", "AC");
  make_submission(admin, problem, "judging", "");  // 正在评测中，verdict 同样为 NULL

  Lease lease = pool_->acquire();
  ASSERT_TRUE(lease.valid());
  SystemBlock now;
  ASSERT_TRUE(load_system_counts(lease.session(), &now, nullptr));

  EXPECT_EQ(now.user_count, baseline_.user_count + 2);
  EXPECT_EQ(now.admin_count, baseline_.admin_count + 1);
  EXPECT_EQ(now.problem_count, baseline_.problem_count + 1);
  EXPECT_EQ(now.case_count, baseline_.case_count + 2);
  EXPECT_EQ(now.submission_count, baseline_.submission_count + 6);
  EXPECT_EQ(now.ac_count, baseline_.ac_count + 2);
  EXPECT_EQ(now.failed_count, baseline_.failed_count + 1);
  // pending 与 judging 都算"未完成"，否则 judge 未接线时排队中的提交在页面上彻底不可见
  EXPECT_EQ(now.judging_count, baseline_.judging_count + 2);
}

TEST_F(AdminDbTest, UserRowUsesTheSharedProgressDefinition) {
  const unsigned long long alice = make_user("_row_alice", "admin");
  const unsigned long long p1 = make_problem("-row1", 1);
  const unsigned long long p2 = make_problem("-row2", 1);

  make_submission(alice, p1, "done", "AC");
  make_submission(alice, p1, "done", "WA");
  make_submission(alice, p2, "done", "WA");
  set_favorite(alice, p1, true);
  set_favorite(alice, p2, false);   // favorite=0 不该被计入 favorites

  Lease lease = pool_->acquire();
  ASSERT_TRUE(lease.valid());
  std::vector<AdminUserRow> rows;
  ASSERT_TRUE(load_users(lease.session(), &rows, nullptr));

  const AdminUserRow* row = find_user(rows, std::string(kUserPrefix) + "_row_alice");
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->role, "admin");
  EXPECT_FALSE(row->created_at.empty());
  EXPECT_EQ(row->submission_count, 3);
  EXPECT_EQ(row->ac_count, 1);
  // solved = 有过 AC 的题数（1）；attempted = 有过任意提交的题数（2）
  // 两者【不互斥】：p1 同时计入两者，与 §6.5 的 byDifficulty / byTag 写法一致。
  EXPECT_EQ(row->solved, 1);
  EXPECT_EQ(row->attempted, 2);
  EXPECT_EQ(row->favorites, 1);
  EXPECT_FALSE(row->last_submit_at.empty());
  // 最后插入的那条 verdict 才是 last_verdict
  EXPECT_EQ(row->last_verdict, "WA");
}

TEST_F(AdminDbTest, LastVerdictIsNullWhenLatestSubmissionIsUndecided) {
  const unsigned long long bob = make_user("_pending_bob", "user");
  const unsigned long long problem = make_problem("-pending", 1);
  make_submission(bob, problem, "done", "AC");       // 先一条 AC
  make_submission(bob, problem, "pending", "");  // 最新一条还没结论

  Lease lease = pool_->acquire();
  ASSERT_TRUE(lease.valid());
  std::vector<AdminUserRow> rows;
  ASSERT_TRUE(load_users(lease.session(), &rows, nullptr));
  const AdminUserRow* row = find_user(rows, std::string(kUserPrefix) + "_pending_bob");
  ASSERT_NE(row, nullptr);
  // 若用 GROUP_CONCAT(verdict ORDER BY id DESC)，NULL 被跳过就会返回上面那条 'AC' ——
  // 这是"提交还没判完"被显示成"刚刚答对"的静默失真。
  EXPECT_TRUE(row->last_verdict.empty());
  EXPECT_FALSE(row->last_submit_at.empty());
  EXPECT_EQ(row->ac_count, 1);
}

TEST_F(AdminDbTest, UserWithoutSubmissionsHasEmptyOptionalFields) {
  make_user("_fresh_carol", "user");
  Lease lease = pool_->acquire();
  ASSERT_TRUE(lease.valid());
  std::vector<AdminUserRow> rows;
  ASSERT_TRUE(load_users(lease.session(), &rows, nullptr));
  const AdminUserRow* row = find_user(rows, std::string(kUserPrefix) + "_fresh_carol");
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->submission_count, 0);
  EXPECT_EQ(row->solved, 0);
  EXPECT_EQ(row->attempted, 0);
  EXPECT_EQ(row->favorites, 0);
  EXPECT_TRUE(row->last_submit_at.empty());
  EXPECT_TRUE(row->last_verdict.empty());
}

TEST_F(AdminDbTest, BuildOverviewKeepsTheInjectedRuntimeStatus) {
  const unsigned long long admin = make_user("_overview", "admin");
  ASSERT_NE(admin, 0u);

  JudgeRuntimeStatus status;
  status.queue_length = 4;
  status.active_judges = 2;
  status.judge_alive = true;

  Lease lease = pool_->acquire();
  ASSERT_TRUE(lease.valid());
  Overview overview;
  std::string error;
  ASSERT_TRUE(build_overview(lease.session(), status, &overview, &error)) << error;

  // 运行态三项来自递进来的快照，不来自 DB
  EXPECT_EQ(overview.system.queue_length, 4);
  EXPECT_EQ(overview.system.active_judges, 2);
  EXPECT_TRUE(overview.system.judge_alive);
  EXPECT_EQ(overview.system.admin_count, baseline_.admin_count + 1);

  // 序列化后能原样还原（字段名与类型都不漂）
  const nlohmann::json parsed = nlohmann::json::parse(to_json(overview));
  EXPECT_EQ(parsed["system"]["queueLength"], 4);
  EXPECT_EQ(parsed["system"]["activeJudges"], 2);
  EXPECT_EQ(parsed["system"]["judgeAlive"], true);
  bool found = false;
  for (const nlohmann::json& user : parsed["users"]) {
    if (user["username"] == std::string(kUserPrefix) + "_overview") {
      found = true;
      EXPECT_EQ(user["role"], "admin");
    }
  }
  EXPECT_TRUE(found);
}

TEST_F(AdminDbTest, RuntimeStatusReachesTheJsonEvenWhenJudgeWired) {
  // 成功路径下"快照值不被 DB 查询覆盖"：上面 BuildOverviewKeepsTheInjectedRuntimeStatus
  // 已覆盖数值，本例只补一条端到端断言 —— 序列化后的三个字段与递进来的一致。
  const unsigned long long admin = make_user("_json", "admin");
  ASSERT_NE(admin, 0u);
  JudgeRuntimeStatus status;
  status.queue_length = 11;
  status.active_judges = 3;
  status.judge_alive = true;
  Lease lease = pool_->acquire();
  ASSERT_TRUE(lease.valid());
  Overview overview;
  ASSERT_TRUE(build_overview(lease.session(), status, &overview, nullptr));
  const nlohmann::json parsed = nlohmann::json::parse(to_json(overview));
  EXPECT_EQ(parsed["system"]["queueLength"], 11);
  EXPECT_EQ(parsed["system"]["activeJudges"], 3);
  EXPECT_EQ(parsed["system"]["judgeAlive"], true);
}

TEST_F(AdminDbTest, CreatedAtIsPassedThroughAsUtcVerbatim) {
  // §2.2 / [D-17]：时间列存 UTC，本层【一律原样透传】，本地化是前端的活（§6.2）。
  // 断言用的是"写入一个已知值、读回必须一模一样"，而不是"看起来像 UTC" ——
  // 后者在本机 TZ=UTC 时恒真，等于没测。差 8 小时、改格式、带上毫秒，都会在这里露出来。
  const unsigned long long user_id = make_user("_utc", "user");
  const unsigned long long problem = make_problem("-utc", 1);
  make_submission(user_id, problem, "done", "AC", "2026-01-02 03:04:05");

  Lease lease = pool_->acquire();
  ASSERT_TRUE(lease.valid());
  std::vector<AdminUserRow> rows;
  ASSERT_TRUE(load_users(lease.session(), &rows, nullptr));
  const AdminUserRow* row = find_user(rows, std::string(kUserPrefix) + "_utc");
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->last_submit_at, "2026-01-02 03:04:05");

  Overview overview;
  overview.users.push_back(*row);
  const nlohmann::json parsed = nlohmann::json::parse(to_json(overview));
  // 序列化层同样不碰时间：既不转本地时区，也不改成 ISO 8601（那是前端的事）
  EXPECT_EQ(parsed["users"][0]["createdAt"].get<std::string>(), row->created_at);
  EXPECT_EQ(parsed["users"][0]["lastSubmitAt"].get<std::string>(), "2026-01-02 03:04:05");
}

TEST_F(AdminDbTest, ProviderOverloadReadsTheRealDatabaseAfterTheSnapshot) {
  // 这是生产上【唯一】会被调用的 build_overview 重载：先 provider() 拿快照（随即释放
  // 调度器的锁），再 acquire() 取 DB 租约。前面 AdminLockOrder 只在"池未就绪"时
  // 观察到前半段；这里让整条路径跑通，并断言快照值不会被随后的 DB 查询覆盖。
  ASSERT_TRUE(init_singleton_pool()) << "进程级单例池初始化失败";
  make_user("_singleton", "admin");

  int provider_calls = 0;
  long active_leases_at_snapshot = -1;
  JudgeRuntimeStatus status;
  status.queue_length = 13;
  status.active_judges = 2;
  status.judge_alive = true;

  Overview overview;
  std::string error;
  ASSERT_TRUE(build_overview(
                 [&]() {
                   ++provider_calls;
                   // §2.3 的死锁四要素里最要紧的那条边：DB 锁 → scheduler 锁。
                   // 此刻若已持有租约，就说明实现把顺序倒过来了（先取租约再取快照）。
                   // active_leases 是【在途租约数】，单线程下它就是"当前是否持锁"的
                   // 可观测代理；stats() 只加池自身的 mutex，不会与在途租约互锁，
                   // 因此即使顺序真的写错了，这里也是断言失败而不是卡死。
                   active_leases_at_snapshot = Pool::instance().stats().active_leases;
                   return status;
                 },
                 &overview, &error))
      << error;

  // 恰好一次：多次调用意味着某处在循环里反复取快照，锁顺序的"先快照后租约"就名存实亡
  EXPECT_EQ(provider_calls, 1);
  EXPECT_EQ(active_leases_at_snapshot, 0) << "取快照时已经持有 DB 租约，锁顺序被倒过来了（§2.3）";
  EXPECT_EQ(overview.system.queue_length, 13);
  EXPECT_EQ(overview.system.active_judges, 2);
  EXPECT_TRUE(overview.system.judge_alive);
  // 真实 DB 计数确实回来了（用相对 baseline 的增量，不写死绝对值）
  EXPECT_EQ(overview.system.user_count, baseline_.user_count + 1);
  EXPECT_FALSE(overview.users.empty());

  const nlohmann::json parsed = nlohmann::json::parse(to_json(overview));
  EXPECT_EQ(parsed["system"]["queueLength"], 13);
  EXPECT_TRUE(parsed["system"]["judgeAlive"]);
}

TEST_F(AdminDbTest, UnwiredProviderReportsJudgeDownWithoutHidingTheRest) {
  // 空 StatusProvider = judge 没接线。此时必须如实报 judgeAlive=false，
  // 但【不能】因此把 DB 统计一起清零 —— 那会让运维以为数据库也挂了。
  ASSERT_TRUE(init_singleton_pool()) << "进程级单例池初始化失败";
  make_user("_down", "admin");

  Overview overview;
  std::string error;
  ASSERT_TRUE(build_overview(StatusProvider{}, &overview, &error)) << error;
  EXPECT_FALSE(overview.system.judge_alive);
  EXPECT_EQ(overview.system.queue_length, 0);
  EXPECT_EQ(overview.system.active_judges, 0);
  EXPECT_EQ(overview.system.user_count, baseline_.user_count + 1);
}

TEST_F(AdminDbTest, UsersComeBackInRegistrationOrder) {
  // §6.3.3 要求 users[] 稳定可复现，故 ORDER BY u.id（注册顺序）而不是 username：
  // 按名字排的话，改一次名、换一种语言、两个用户撞名，都会让页面顺序跳来跳去。
  const unsigned long long z = make_user("_order_z", "user");
  const unsigned long long a = make_user("_order_a", "user");
  const unsigned long long m = make_user("_order_m", "user");
  ASSERT_LT(z, a);
  ASSERT_LT(a, m);   // 造出来的 id 递增，与字典序恰好相反

  Lease lease = pool_->acquire();
  ASSERT_TRUE(lease.valid());
  std::vector<AdminUserRow> rows;
  ASSERT_TRUE(load_users(lease.session(), &rows, nullptr));

  std::vector<std::string> probes;
  for (const AdminUserRow& row : rows) {
    if (row.username.rfind(kUserPrefix, 0) == 0) probes.push_back(row.username);
  }
  ASSERT_EQ(probes.size(), 3u);
  EXPECT_EQ(probes[0], std::string(kUserPrefix) + "_order_z");
  EXPECT_EQ(probes[1], std::string(kUserPrefix) + "_order_a");
  EXPECT_EQ(probes[2], std::string(kUserPrefix) + "_order_m");
}

TEST_F(AdminDbTest, SolvedCountsDistinctProblemsNotSubmissions) {
  // §6.4 / §6.5：solved 是"有过 AC 的题数"（COUNT DISTINCT problem_id），
  // 同一题刷过十次也只算一题。这与"AC 次数"是两个数，混起来会让进度虚高。
  const unsigned long long user_id = make_user("_distinct", "user");
  const unsigned long long p1 = make_problem("-distinct1", 1);
  const unsigned long long p2 = make_problem("-distinct2", 1);
  make_submission(user_id, p1, "done", "AC");
  make_submission(user_id, p1, "done", "AC");   // 同一题第二次通过
  make_submission(user_id, p2, "done", "WA");

  Lease lease = pool_->acquire();
  ASSERT_TRUE(lease.valid());
  std::vector<AdminUserRow> rows;
  ASSERT_TRUE(load_users(lease.session(), &rows, nullptr));
  const AdminUserRow* row = find_user(rows, std::string(kUserPrefix) + "_distinct");
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->submission_count, 3);
  EXPECT_EQ(row->ac_count, 2);
  EXPECT_EQ(row->solved, 1);      // 两题里只有一题 AC 过
  EXPECT_EQ(row->attempted, 2);   // 两题都提交过（与 solved 不互斥）
}

TEST_F(AdminDbTest, AwkwardUsernameSurvivesDbToJson) {
  // 真实 DB → JSON 的一条龙：用户名里的引号 / 反斜杠 / 中文 / emoji 要原样回来。
  // 顺带证明聚合查询用的是预处理参数而不是把用户名拼进 SQL —— 若哪天有人改成拼接，
  // 这个引号就会变成一条语法错误或（更糟）一条注入。
  const std::string suffix = "_we\"ird\\x 中文 🙂";
  make_user(suffix, "user");
  const std::string username = std::string(kUserPrefix) + suffix;

  Lease lease = pool_->acquire();
  ASSERT_TRUE(lease.valid());
  std::vector<AdminUserRow> rows;
  ASSERT_TRUE(load_users(lease.session(), &rows, nullptr));
  const AdminUserRow* row = find_user(rows, username);
  ASSERT_NE(row, nullptr) << "用户名在插入或查询环节被截断了";

  Overview overview;
  overview.users.push_back(*row);
  const nlohmann::json parsed = nlohmann::json::parse(to_json(overview));
  EXPECT_EQ(parsed["users"][0]["username"].get<std::string>(), username);
}

}  // namespace