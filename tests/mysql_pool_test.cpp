// =============================================================================
// MySQL 连接池单元测试（gtest 1.14）
// 被测：include/db/mysql_pool.h + src/db/mysql_pool.cpp（SPEC.md §9 M3 T3.1）
// 运行：tests/run_unit_tests.sh（即 ctest）
//
// 【为什么这批用例要真库】本模块的价值全在【与 MySQL 的真实交互】上：预处理语句的
//   二进制协议、截断后按精确长度重取列、DDL 变更后服务端句柄失效、断连后的恢复。
//   这些行为无法用 mock 验证 —— mock 掉的正是"最容易错"的那一层。
//   因此：连不上库时【全部跳过】（GTEST_SKIP）而不是失败，理由与 config_test.cpp
//   清理环境变量是同一条：CI 上没有 MySQL 不代表代码坏了，红色会淹没真正的回归。
//
// 【不测】DAO 的 SQL 本身（T3.2）、建表 DDL 的内容（T0.3）、登录态与业务错误码映射
//   （T3.3/T3.5）。本文件只保证"DB 通道本身是对的"。
//
// 【每条用例自建自销一张影子表】oj_pool_selftest，用例之间不共享状态；固定的表名是
//   刻意的：崩溃后残留的同名表会被下一轮 CREATE TABLE IF NOT EXISTS 直接复用，
//   而随机名会留下一堆永远清不掉的垃圾表。
// =============================================================================
#include <gtest/gtest.h>

#include <mysql/mysql.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "common/config.h"
#include "db/mysql_pool.h"

namespace {

using namespace oj::db;

constexpr const char* kProbeTable = "oj_pool_selftest";
constexpr const char* kProbeDdl =
    "CREATE TABLE IF NOT EXISTS oj_pool_selftest ("
    "  id INT NOT NULL PRIMARY KEY,"
    "  s MEDIUMTEXT NULL,"
    "  i BIGINT NULL,"
    "  u BIGINT UNSIGNED NULL,"
    "  d DOUBLE NULL,"
    "  b TINYINT NULL,"
    // 自增列：只为测 ResultSet::insert_id()。DAO 的 users/problems/submissions 三张表
    // 都靠它拿回新行 id（§6.3 的注册、建表、提交三个端点），漏测代价太大。
    "  ai BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
    "  UNIQUE KEY uk_ai (ai)"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";

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

// 一条【不经池】的裸连接。用途只有两个，且都很正当：
//   ① 从外部 KILL 掉池里的连接，验证恢复路径；
//   ② 手工持有行锁，验证 innodb_lock_wait_timeout 真的把楔死限制住了。
// 它不能替池做别的事 —— 一旦用它跑业务 SQL，本文件就测不到"串行化"了。
class RawConnection {
 public:
  ~RawConnection() {
    if (mysql_ != nullptr) mysql_close(mysql_);
  }
  bool connect(const PoolOptions& o) {
    mysql_ = mysql_init(nullptr);
    if (mysql_ == nullptr) return false;
    const char* charset = "utf8mb4";
    mysql_options(mysql_, MYSQL_SET_CHARSET_NAME, charset);
    return mysql_real_connect(mysql_, o.host.c_str(), o.user.c_str(),
                              o.password.c_str(), o.database.c_str(), o.port,
                              nullptr, 0) != nullptr;
  }
  bool exec(const std::string& sql) {
    if (mysql_query(mysql_, sql.c_str()) != 0) return false;
    // 必须消费掉结果集：MySQL 的 C API 允许一条连接上"还有未读结果"，
    // 但那样下一条语句会直接报 Commands out of sync —— 本文件里最容易被忽略的一个坑。
    MYSQL_RES* res = mysql_store_result(mysql_);
    if (res != nullptr) mysql_free_result(res);
    return true;
  }
  MYSQL* raw() const { return mysql_; }

 private:
  MYSQL* mysql_ = nullptr;
};

// ---- fixture ------------------------------------------------------------------

class PoolTest : public ::testing::Test {
 protected:
  void SetUp() override {
    options_ = base_options();

    // 1) 探测可达性（连不上就整体跳过，别把每个用例都标红）
    if (!Pool::instance().init(options_)) {
      GTEST_SKIP() << "无法连接 MySQL " << options_.host << ":" << options_.port
                   << "/" << options_.database << "，跳过全部依赖真实数据库的用例"
                   << "（设置 MYSQL_HOST/MYSQL_PORT/MYSQL_USER/MYSQL_PASSWORD/MYSQL_DB 可指定）";
    }
    Pool::instance().shutdown();

    pool_.reset(new Pool());
    ASSERT_TRUE(pool_->init(options_)) << pool_->describe();

    Lease lease = pool_->acquire();
    ASSERT_TRUE(lease.valid());
    ASSERT_TRUE(lease->exec(kProbeDdl));
    ASSERT_TRUE(lease->exec("DELETE FROM " + std::string(kProbeTable)));
  }

  void TearDown() override {
    if (pool_ != nullptr && pool_->ready()) {
      Lease lease = pool_->acquire();
      if (lease.valid()) lease->exec("DROP TABLE IF EXISTS " + std::string(kProbeTable));
    }
    if (pool_ != nullptr) {
      pool_->shutdown();
      pool_.reset();
    }
  }

  PoolOptions options_;
  std::unique_ptr<Pool> pool_;
};

// =============================================================================
// A. init 的失败路径（不需要真库：参数校验发生在任何网络 I/O 之前）
// =============================================================================

TEST(PoolInit, RejectsInvalidOptionsBeforeTouchingTheNetwork) {
  struct Case {
    const char* name;
    void (*mutate)(PoolOptions&);
  };
  const Case cases[] = {
      {"size=0", [](PoolOptions& o) { o.size = 0; }},
      {"host 为空", [](PoolOptions& o) { o.host.clear(); }},
      {"port=0", [](PoolOptions& o) { o.port = 0; }},
      {"port 越界", [](PoolOptions& o) { o.port = 70000; }},
      {"user 为空", [](PoolOptions& o) { o.user.clear(); }},
      {"database 为空", [](PoolOptions& o) { o.database.clear(); }},
      {"connect_timeout=0", [](PoolOptions& o) { o.connect_timeout_s = 0; }},
      {"read_timeout=0", [](PoolOptions& o) { o.read_timeout_s = 0; }},
      {"write_timeout=0", [](PoolOptions& o) { o.write_timeout_s = 0; }},
      {"lock_wait_timeout=0", [](PoolOptions& o) { o.lock_wait_timeout_s = 0; }},
      {"stmt_cache_capacity=0", [](PoolOptions& o) { o.stmt_cache_capacity = 0; }},
  };
  for (const Case& entry : cases) {
    PoolOptions o = base_options();
    entry.mutate(o);
    const std::string name = entry.name;
    Pool pool;
    std::vector<std::string> problems;
    EXPECT_FALSE(pool.init(o, &problems)) << name;
    EXPECT_FALSE(problems.empty()) << name;
    EXPECT_FALSE(pool.ready()) << name;
  }
}

TEST(PoolInit, FailsWithActionableMessageOnBadCredentials) {
  PoolOptions o = base_options();
  o.password = "definitely-not-the-password";
  // 端口指向一个没人监听的高位端口，必须在 connect_timeout 内失败而不是卡住
  Pool pool;
  std::vector<std::string> problems;
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(pool.init(o, &problems));
  const auto elapsed = std::chrono::steady_clock::now() - start;
  ASSERT_EQ(problems.size(), 1u);
  // 失败原因必须能定位到目标与原因，且【不能】把密码写进日志
  EXPECT_NE(problems[0].find(o.host), std::string::npos) << problems[0];
  EXPECT_EQ(problems[0].find(o.password), std::string::npos) << problems[0];
  EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(), 15);
}

TEST(PoolInit, UnreachablePortFailsFast) {
  PoolOptions o = base_options();
  o.port = 1;              // 没人监听
  o.connect_timeout_s = 2; // 明确给 2s，验证"起不来时快速失败"
  Pool pool;
  std::vector<std::string> problems;
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(pool.init(o, &problems));
  EXPECT_LE(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count(),
            5000);
}

// =============================================================================
// B. 参数化语句与结果取回
// =============================================================================

TEST_F(PoolTest, BindsEveryParameterTypeIncludingNullAndUtf8) {
  Lease lease = pool_->acquire();
  ASSERT_TRUE(lease.valid());
  Session& s = lease.session();

  const ResultSet inserted = s.run(
      "INSERT INTO " + std::string(kProbeTable) + " (id, s, i, u, d, b) "
      "VALUES (?, ?, ?, ?, ?, ?)",
      {Value::from_i64(7), Value::from_text("中文 utf8mb4 ✓"),
       Value::from_i64(-42), Value::from_u64(18446744073709551615ULL),
       Value::from_double(3.5), Value::from_bool(true)});
  ASSERT_TRUE(inserted.ok()) << inserted.error().format();
  EXPECT_EQ(inserted.affected_rows(), 1u);

  const ResultSet got =
      s.run("SELECT id, s, i, u, d, b FROM " + std::string(kProbeTable) +
            " WHERE id = ?",
            {Value::from_i64(7)});
  ASSERT_TRUE(got.ok()) << got.error().format();
  ASSERT_EQ(got.size(), 1u);
  ASSERT_EQ(got.column_count(), 6u);
  EXPECT_EQ(got.columns()[0], "id");
  EXPECT_EQ(got.at(0).i64(0), 7);
  EXPECT_EQ(got.at(0).str(1), "中文 utf8mb4 ✓");
  EXPECT_EQ(got.at(0).i64(2), -42);
  // BIGINT UNSIGNED 必须原样往返（18446744073709551615 超过 int64 上限）
  EXPECT_EQ(got.at(0).u64(3), 18446744073709551615ULL);
  EXPECT_DOUBLE_EQ(got.at(0).real(4), 3.5);
  EXPECT_TRUE(got.at(0).boolean(5, false));
}

TEST_F(PoolTest, NullAndEmptyStringAreDifferentValues) {
  Lease lease = pool_->acquire();
  Session& s = lease.session();
  const std::string base = "INSERT INTO " + std::string(kProbeTable) + " (id, s) VALUES (?, ?)";

  ASSERT_TRUE(s.run(base, {Value::from_i64(1), Value::null()}).ok());
  ASSERT_TRUE(s.run(base, {Value::from_i64(2), Value::from_text("")}).ok());

  const ResultSet rs =
      s.run("SELECT id, s FROM " + std::string(kProbeTable) + " ORDER BY id");
  ASSERT_TRUE(rs.ok());
  ASSERT_EQ(rs.size(), 2u);
  EXPECT_TRUE(rs.at(0).is_null(1));
  EXPECT_TRUE(rs.at(0).str(1).empty());
  EXPECT_FALSE(rs.at(1).is_null(1));
  EXPECT_TRUE(rs.at(1).str(1).empty());  // '' 与 NULL 的字节内容相同，必须靠 is_null 区分
  // str_or 是"NULL 才回落"的快捷方式，语义必须与 is_null 一致
  EXPECT_EQ(rs.at(0).str_or(1, "fallback"), "fallback");
  EXPECT_EQ(rs.at(1).str_or(1, "fallback"), "");
}

TEST_F(PoolTest, PlaceholderCountMismatchIsRejectedBeforeSendingAnything) {
  Lease lease = pool_->acquire();
  Session& s = lease.session();

  // 2 个占位符只传 1 个参数：若放行，MySQL 会照常执行，只是少了一段赋值
  const ResultSet bad = s.run(
      "INSERT INTO " + std::string(kProbeTable) + " (id, s) VALUES (?, ?)",
      {Value::from_i64(99)});
  ASSERT_TRUE(bad.failed());
  EXPECT_FALSE(bad.error().server_side()) << "必须是本地拦截，不该发给服务端";
  EXPECT_EQ(bad.error().message.find("占位符"), 0u);
  EXPECT_NE(bad.error().sql.find('?'), std::string::npos) << "记录的是带占位符的 SQL 原文";

  const ResultSet count =
      s.run("SELECT COUNT(*) FROM " + std::string(kProbeTable) + " WHERE id = ?",
            {Value::from_i64(99)});
  ASSERT_TRUE(count.ok());
  EXPECT_EQ(count.at(0).i64(0), 0) << "本地拦截必须没有产生任何副作用";
}

TEST_F(PoolTest, LargeMediumTextRoundTripsThroughTruncationAndRefetch) {
  // §3.2 的 statement_md / code / compiler_output 都是 MEDIUMTEXT，代码上限 64KB
  // （§5.8）。列的初始 buffer 只有 256 字节，因此这条用例走的是【截断后按 length
  // 重新取整列】那条路径 —— 它必须是常态路径而不是罕见边角。
  std::string big;
  big.reserve(100 * 1024);
  for (int i = 0; i < 100 * 1024; ++i) {
    big.push_back(static_cast<char>('a' + (i % 26)));
  }
  // 用字符串字面量拼接而不是 char('中')：后者在 C++ 里是【多字符常量】（int），
  // 截成 char 只会得到一个乱码字节 —— 那样测的就不是 utf8mb4 了
  std::string big_zh;
  big_zh.reserve(20 * 1024 * 3);
  for (int i = 0; i < 20 * 1024; ++i) big_zh += "中";

  Lease lease = pool_->acquire();
  Session& s = lease.session();
  ASSERT_TRUE(s.run("INSERT INTO " + std::string(kProbeTable) + " (id, s) VALUES (?, ?)",
                    {Value::from_i64(1), Value::from_text(big)})
                  .ok());
  ASSERT_TRUE(s.run("INSERT INTO " + std::string(kProbeTable) + " (id, s) VALUES (?, ?)",
                    {Value::from_i64(2), Value::from_text(big_zh)})
                  .ok());

  const ResultSet rs =
      s.run("SELECT id, s FROM " + std::string(kProbeTable) + " ORDER BY id");
  ASSERT_TRUE(rs.ok()) << rs.error().format();
  ASSERT_EQ(rs.size(), 2u);
  EXPECT_EQ(rs.at(0).str(1).size(), big.size());
  EXPECT_EQ(rs.at(0).str(1), big);
  EXPECT_EQ(rs.at(1).str(1).size(), big_zh.size());
  EXPECT_EQ(rs.at(1).str(1), big_zh);
}

TEST_F(PoolTest, RowParsersFallBackInsteadOfGuessing) {
  Lease lease = pool_->acquire();
  Session& s = lease.session();
  ASSERT_TRUE(s.run("INSERT INTO " + std::string(kProbeTable) + " (id, s, i) VALUES (?, ?, ?)",
                    {Value::from_i64(1), Value::from_text("not-a-number"), Value::null()})
                  .ok());

  const ResultSet rs =
      s.run("SELECT id, s, i, b FROM " + std::string(kProbeTable) + " WHERE id = ?",
            {Value::from_i64(1)});
  ASSERT_TRUE(rs.ok());
  ASSERT_EQ(rs.size(), 1u);
  const Row& row = rs.at(0);
  EXPECT_EQ(row.i64(1, -1), -1) << "非数字必须回落，不能静默变成 0";
  EXPECT_TRUE(row.is_null(2));
  EXPECT_EQ(row.i64(2, -1), -1) << "NULL 必须回落";
  EXPECT_TRUE(row.is_null(3));
  EXPECT_TRUE(row.boolean(3, true)) << "NULL 上的 boolean 用 fallback";

  const ResultSet twelve =
      s.run("SELECT '12abc' AS v UNION ALL SELECT '12'");
  ASSERT_TRUE(twelve.ok());
  EXPECT_EQ(twelve.at(0).i64(0, -1), -1);
  EXPECT_EQ(twelve.at(1).i64(0, -1), 12);
}

TEST_F(PoolTest, InsertIdAndAffectedRowsFollowTheStatement) {
  // insert_id() 是 DAO 的刚需：users / problems / submissions 三张表的新行 id
  // 全靠它（§6.3 的注册、题库同步、提交三个端点）。在补上这条用例之前，
  // 整个文件没有一处依赖它 —— 也就是说这个字段即便完全接错，38 条用例照样全绿。
  //
  // 本条同时把三个【反直觉】的服务端语义钉死，DAO 作者不必再去查 MySQL 手册：
  //   ① INSERT 显式给了 AUTO_INCREMENT 列的值 → insert_id 就是那个值
  //   ② UPDATE / DELETE 的 affected_rows 是【实际变更行数】，写回同值时是 0
  //      （未启用 CLIENT_FOUND_ROWS）
  //   ③ SELECT 的 affected_rows 是 (unsigned)-1，insert_id 是【上一次 INSERT 的残留值】
  Lease lease = pool_->acquire();
  Session& s = lease.session();
  const std::string table = kProbeTable;

  const ResultSet auto_inserted =
      s.run("INSERT INTO " + table + " (id, s) VALUES (?, ?)",
            {Value::from_i64(1), Value::from_text("a")});
  ASSERT_TRUE(auto_inserted.ok()) << auto_inserted.error().format();
  EXPECT_EQ(auto_inserted.affected_rows(), 1u);
  const unsigned long long first_id = auto_inserted.insert_id();
  EXPECT_GT(first_id, 0u) << "AUTO_INCREMENT 的新行 id 必须能取回";

  // 再插一行：id 必须单调递增，说明取到的是【本语句】的值而不是上一次残留
  const ResultSet second =
      s.run("INSERT INTO " + table + " (id, s) VALUES (?, ?)",
            {Value::from_i64(2), Value::from_text("b")});
  ASSERT_TRUE(second.ok()) << second.error().format();
  EXPECT_GT(second.insert_id(), first_id);

  // 与服务端自己的视角一致（而不是只有客户端看起来对）
  const ResultSet server = s.run("SELECT LAST_INSERT_ID() AS id");
  ASSERT_TRUE(server.ok());
  EXPECT_EQ(server.at(0).u64(0), second.insert_id());

  // 显式指定 AUTO_INCREMENT 列 → insert_id 就是它
  const ResultSet explicit_id =
      s.run("INSERT INTO " + table + " (id, s, ai) VALUES (?, ?, ?)",
            {Value::from_i64(3), Value::from_text("c"), Value::from_u64(4242)});
  ASSERT_TRUE(explicit_id.ok()) << explicit_id.error().format();
  EXPECT_EQ(explicit_id.insert_id(), 4242u);

  // UPDATE 的 affected_rows 是变更行数：改 1 行 → 1；改回原值 → 0
  const ResultSet changed =
      s.run("UPDATE " + table + " SET s = ? WHERE id = ?",
            {Value::from_text("changed"), Value::from_i64(1)});
  ASSERT_TRUE(changed.ok()) << changed.error().format();
  EXPECT_EQ(changed.affected_rows(), 1u);
  const ResultSet noop =
      s.run("UPDATE " + table + " SET s = ? WHERE id = ?",
            {Value::from_text("changed"), Value::from_i64(1)});
  ASSERT_TRUE(noop.ok());
  EXPECT_EQ(noop.affected_rows(), 0u)
      << "写回同值时 affected_rows 是 0，不是 1 —— DAO 判'命中了几行'时别指望它等于 1";
  const ResultSet none =
      s.run("UPDATE " + table + " SET s = ? WHERE id = ?",
            {Value::from_text("x"), Value::from_i64(9999)});
  ASSERT_TRUE(none.ok());
  EXPECT_EQ(none.affected_rows(), 0u);

  // ③ SELECT：affected_rows 是 (unsigned)-1；insert_id 则是【未定义】的 ——
  // 实测本机 8.0.46 上它可能是 0，也可能是上一次 INSERT 的残留（取决于中间夹了
  // 哪些语句）。所以不对它做具体断言，只钉住真正重要的性质：
  // SELECT 夹在两个 INSERT 中间，绝不能污染下一个 INSERT 的 insert_id。
  const ResultSet selected = s.run("SELECT id FROM " + table);
  ASSERT_TRUE(selected.ok());
  EXPECT_EQ(selected.affected_rows(), ~0ULL) << "SELECT 的 affected_rows 是 (unsigned)-1";

  const ResultSet after_select =
      s.run("INSERT INTO " + table + " (id, s) VALUES (?, ?)",
            {Value::from_i64(4), Value::from_text("d")});
  ASSERT_TRUE(after_select.ok()) << after_select.error().format();
  EXPECT_EQ(after_select.insert_id(), 4243u)
      << "SELECT 不能污染下一次 INSERT 的 insert_id";
}

TEST_F(PoolTest, ResultSetFindAndMetadata) {
  Lease lease = pool_->acquire();
  Session& s = lease.session();
  for (int i = 1; i <= 3; ++i) {
    ASSERT_TRUE(s.run("INSERT INTO " + std::string(kProbeTable) + " (id, s) VALUES (?, ?)",
                      {Value::from_i64(i), Value::from_text("v" + std::to_string(i))})
                      .ok());
  }
  const ResultSet rs =
      s.run("SELECT id, s FROM " + std::string(kProbeTable) + " ORDER BY id");
  ASSERT_TRUE(rs.ok());
  EXPECT_TRUE(rs.find(1, "v2") != nullptr);
  EXPECT_EQ(rs.find(1, "v2")->i64(0), 2);
  EXPECT_TRUE(rs.find(1, "nope") == nullptr);
  EXPECT_FALSE(rs.empty());
}

// =============================================================================
// C. 错误上抛：SQLSTATE、错误码、以及"错误里绝不含参数值"
// =============================================================================

TEST_F(PoolTest, DuplicateKeyReportsServerCodeAndNeverLeaksParameterValues) {
  Lease lease = pool_->acquire();
  Session& s = lease.session();
  const std::string secret = "p@ssw0rd-should-never-appear-in-errors";
  ASSERT_TRUE(s.run("INSERT INTO " + std::string(kProbeTable) + " (id, s) VALUES (?, ?)",
                    {Value::from_i64(1), Value::from_text("first")})
                  .ok());

  const ResultSet dup =
      s.run("INSERT INTO " + std::string(kProbeTable) + " (id, s) VALUES (?, ?)",
            {Value::from_i64(1), Value::from_text(secret)});
  ASSERT_TRUE(dup.failed());
  EXPECT_TRUE(dup.error().server_side());
  EXPECT_EQ(dup.error().code, 1062u);  // ER_DUP_ENTRY
  EXPECT_EQ(dup.error().sqlstate, "23000");
  // 记录的 SQL 必须是【带 ? 的模板原文】：用户代码、密码、编译输出都走这条通道，
  // 参数值一旦被拼进去或被记进错误信息，就等于从日志里泄露
  EXPECT_NE(dup.error().sql.find('?'), std::string::npos);
  EXPECT_EQ(dup.error().sql.find(secret), std::string::npos);
  EXPECT_EQ(dup.error().message.find(secret), std::string::npos);
  EXPECT_EQ(s.error().sql.find(secret), std::string::npos);
}

TEST_F(PoolTest, ExecRejectsPlaceholdersAndResultSets) {
  Lease lease = pool_->acquire();
  Session& s = lease.session();

  // 第二道护栏：带 ? 的语句不许走 exec（否则"拼字符串再 exec"就绕过了预处理）
  EXPECT_FALSE(s.exec("DELETE FROM " + std::string(kProbeTable) + " WHERE id = ?"));
  EXPECT_NE(s.error().message.find('?'), std::string::npos);

  // 用错 API：SELECT 走 exec 会丢结果，必须当场拒绝而不是让连接卡在未读状态
  EXPECT_FALSE(s.exec("SELECT 1"));
  EXPECT_NE(s.error().message.find("run()"), std::string::npos);
  // 拒绝之后连接仍然是干净的：下一条语句照常工作
  EXPECT_TRUE(s.run("SELECT 1 AS one").ok());
}

// =============================================================================
// D. 事务（T2.11 的三段写依赖它）
// =============================================================================

TEST_F(PoolTest, ExecReportsServerSideErrorAndLeavesConnectionUsable) {
  // exec() 的本地拒绝（占位符 / 返回结果集）之外，还有【真·服务端报错】这一条路：
  // §6.3.3 的角色同步与 §2.1-A 的启动建表都走 exec()，写错一条语句时
  // 拿到的必须是一个带 errno/SQLSTATE 的 DbError，且连接随后仍然干净。
  Lease lease = pool_->acquire();
  Session& s = lease.session();

  EXPECT_FALSE(s.exec("THIS IS NOT SQL"));
  EXPECT_TRUE(s.error().server_side());
  EXPECT_EQ(s.error().code, 1064u) << s.error().format();  // ER_PARSE_ERROR
  EXPECT_EQ(s.error().sqlstate, "42000");
  EXPECT_NE(s.error().sql.find("THIS IS NOT SQL"), std::string::npos);

  // 连接没被这次失败搞坏：紧接着的语句照常工作
  EXPECT_TRUE(s.run("SELECT 1 AS one").ok());
  EXPECT_TRUE(s.exec("CREATE TABLE IF NOT EXISTS oj_exec_probe (id INT PRIMARY KEY)"));
  EXPECT_TRUE(s.exec("DROP TABLE oj_exec_probe"));
}

TEST_F(PoolTest, ExecScriptStopsAtFirstFailingStatement) {
  // 启动建表依赖这个语义：01-schema.sql 里第 k 条失败时必须【停下】并报出是哪一条，
  // 而不是继续执行后面的 —— 后面的表大概率依赖前面的（外键），继续跑只会把
  // 报错冲成一串不相干的 "foreign key constraint fails"，把真正的病因埋掉。
  const std::string script =
      "CREATE TABLE IF NOT EXISTS oj_script_step (id INT PRIMARY KEY);"
      "THIS IS NOT SQL;"
      "CREATE TABLE IF NOT EXISTS oj_script_should_not_exist (id INT PRIMARY KEY);";
  std::vector<std::string> failed;
  {
    Lease lease = pool_->acquire();
    ASSERT_TRUE(lease.valid());
    EXPECT_EQ(lease->exec_script(script, &failed), 1u)
        << "应当在第 1 条成功后、第 2 条失败处停下";
    ASSERT_EQ(failed.size(), 1u);
    EXPECT_NE(failed[0].find("1064"), std::string::npos) << failed[0];
    EXPECT_TRUE(lease->exec("DROP TABLE IF EXISTS oj_script_step"));
  }
  Lease check = pool_->acquire();
  const ResultSet rs = check->run(
      "SELECT COUNT(*) FROM information_schema.tables WHERE table_name = 'oj_script_should_not_exist'");
  ASSERT_TRUE(rs.ok());
  EXPECT_EQ(rs.at(0).i64(0), 0) << "失败之后的语句不该被执行";
}

TEST_F(PoolTest, TransactionGuardCommitsAndRollsBack) {
  {
    Lease lease = pool_->acquire();
    Session& s = lease.session();
    {
      Transaction tx(s);
      ASSERT_TRUE(tx.ok());
      EXPECT_TRUE(s.run("INSERT INTO " + std::string(kProbeTable) + " (id, s) VALUES (?, ?)",
                        {Value::from_i64(1), Value::from_text("committed")})
                        .ok());
      EXPECT_TRUE(s.in_transaction());
      ASSERT_TRUE(tx.commit());
    }
    EXPECT_FALSE(s.in_transaction());
  }
  {
    Lease lease = pool_->acquire();
    Session& s = lease.session();
    {
      Transaction tx(s);
      ASSERT_TRUE(tx.ok());
      EXPECT_TRUE(s.run("INSERT INTO " + std::string(kProbeTable) + " (id, s) VALUES (?, ?)",
                        {Value::from_i64(2), Value::from_text("rolled-back")})
                        .ok());
      // 不 commit：析构必须回滚
    }
    EXPECT_FALSE(s.in_transaction());
  }
  Lease lease = pool_->acquire();
  Session& s = lease.session();
  const ResultSet rs =
      s.run("SELECT id FROM " + std::string(kProbeTable) + " ORDER BY id");
  ASSERT_TRUE(rs.ok());
  ASSERT_EQ(rs.size(), 1u);
  EXPECT_EQ(rs.at(0).i64(0), 1) << "只有 commit 的那条在";
}

TEST_F(PoolTest, LeaseEndRollsBackUncommittedTransaction) {
  {
    Lease lease = pool_->acquire();
    Session& s = lease.session();
    ASSERT_TRUE(s.begin());
    ASSERT_TRUE(s.run("INSERT INTO " + std::string(kProbeTable) + " (id, s) VALUES (?, ?)",
                      {Value::from_i64(1), Value::from_text("leaked")})
                    .ok());
    // 故意不 commit 也不 rollback：连接是共享的，兜底回滚是连接池的职责
  }
  Lease lease = pool_->acquire();
  const ResultSet rs =
      lease->run("SELECT COUNT(*) FROM " + std::string(kProbeTable) + " WHERE id = ?",
                 {Value::from_i64(1)});
  ASSERT_TRUE(rs.ok());
  EXPECT_EQ(rs.at(0).i64(0), 0) << "未提交事务必须被兜底回滚，否则会污染下一个租约";
  EXPECT_GT(pool_->stats().rollbacks, 0);
}

TEST_F(PoolTest, CommitWithoutBeginIsRejected) {
  Lease lease = pool_->acquire();
  Session& s = lease.session();
  EXPECT_FALSE(s.commit());
  EXPECT_NE(s.error().message.find("没有处于事务"), std::string::npos);
  EXPECT_TRUE(s.rollback()) << "无事务可回滚应当幂等";
}

// =============================================================================
// E. D-21 的核心承诺：DB 访问被一把全局互斥量串行化
// =============================================================================

TEST_F(PoolTest, ConcurrentLeasesNeverOverlap) {
  // 8 个线程（§2.3 的 HTTP 线程池大小）各取 40 次租约。若串行化真的生效，
  // 任意时刻在租约内的线程数必须是 1。
  pool_->reset_stats();  // 把 SetUp 里两条 DDL 的记账清掉，下面的断言才是纯并发的
  std::atomic<int> in_flight{0};
  std::atomic<int> max_in_flight{0};
  std::atomic<long> ok_count{0};
  std::atomic<long> fail_count{0};
  constexpr int kThreads = 8;
  constexpr int kIterations = 40;

  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&] {
      for (int i = 0; i < kIterations; ++i) {
        Lease lease = pool_->acquire();
        if (!lease.valid()) {
          fail_count.fetch_add(1);
          continue;
        }
        const int now = in_flight.fetch_add(1) + 1;
        int expected = max_in_flight.load();
        while (now > expected &&
               !max_in_flight.compare_exchange_weak(expected, now)) {
        }
        const ResultSet rs = lease->run("SELECT 1 AS one");
        if (rs.ok()) ok_count.fetch_add(1);
        in_flight.fetch_sub(1);
      }
    });
  }
  for (std::thread& w : workers) w.join();

  EXPECT_EQ(fail_count.load(), 0);
  EXPECT_EQ(ok_count.load(), kThreads * kIterations);
  EXPECT_EQ(max_in_flight.load(), 1) << "[D-21] 的全局互斥量没有真正串行化";
  EXPECT_EQ(pool_->stats().statements, kThreads * kIterations);
}

TEST_F(PoolTest, WaitedAndMaxWaitRecordQueueingUnderContention) {
  // 头文件把 max_wait 称作「[D-21] 串行化是否成为瓶颈的唯一直接证据」（§8 R-6）——
  // 在补上这条用例之前，这个字段【没有任何测试支撑】，那句话等于没有依据。
  pool_->reset_stats();
  {
    Lease solo = pool_->acquire();
    ASSERT_TRUE(solo.valid());
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(solo.waited()).count(), 200)
        << "无竞争时 waited() 应接近 0；一直很大说明串行化本身出了问题";
  }

  // 制造竞争：一个线程持锁跑 SLEEP(0.4)，主线程的 acquire 必须被阻塞住。
  // 等待者用「hogging 标志」而不是「等对方通知」来对齐时序 —— 后者会与
  // [D-21] 的全局互斥量构成死锁（等待者永远等不到通知，因为对方也在等锁）。
  std::atomic<bool> hogging{false};
  std::thread hog([&] {
    Lease lease = pool_->acquire();
    if (!lease.valid()) return;
    hogging.store(true);
    lease->run("SELECT SLEEP(0.4) AS slept");
  });
  while (!hogging.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));

  std::chrono::nanoseconds mine{0};
  {
    Lease lease = pool_->acquire();  // 阻塞约 400ms
    mine = lease.waited();
    ASSERT_TRUE(lease.valid());
    EXPECT_TRUE(lease->run("SELECT 1 AS one").ok());
  }
  hog.join();

  EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(mine).count(), 100)
      << "被 SLEEP 拖住的那次 acquire 必须记录到明显的排队时长";
  const PoolStats stats = pool_->stats();
  EXPECT_GE(stats.max_wait.count(), mine.count())
      << "池级 max_wait 必须不小于任何单次 waited()";
  EXPECT_EQ(stats.active_leases, 0) << "所有租约都应已归还";
}

// =============================================================================
// F. 语句缓存：命中、淘汰、以及表结构变化后的重备
// =============================================================================

TEST_F(PoolTest, PreparedStatementsAreCachedAndEvictedFifo) {
  pool_->reset_stats();
  {
    Lease lease = pool_->acquire();
    Session& s = lease.session();
    const std::string sql =
        "SELECT id FROM " + std::string(kProbeTable) + " WHERE id = ?";
    for (int i = 0; i < 5; ++i) {
      ASSERT_TRUE(s.run(sql, {Value::from_i64(i)}).ok());
    }
  }
  PoolStats stats = pool_->stats();
  EXPECT_EQ(stats.prepares, 1) << "同一条 SQL 只应 prepare 一次";
  EXPECT_EQ(stats.stmt_cache_hits, 4);

  // 缓存容量必须真的生效：超过容量后按 FIFO 淘汰，而不是无限增长
  Pool tiny;
  PoolOptions small = options_;
  small.stmt_cache_capacity = 2;
  ASSERT_TRUE(tiny.init(small));
  {
    Lease lease = tiny.acquire();
    Session& s = lease.session();
    for (int i = 0; i < 5; ++i) {
      const std::string sql = "SELECT " + std::to_string(i) + " AS v";
      ASSERT_TRUE(s.run(sql).ok()) << sql;
    }
  }
  EXPECT_GE(tiny.stats().stmt_cache_evictions, 3);
  tiny.shutdown();
}

TEST_F(PoolTest, SchemaChangeDoesNotBreakCachedStatement) {
  // 契约不是"缓存里的语句永远有效"，而是【表结构变化后，同一个缓存句柄仍能返回正确结果】。
  // ⚠️ 实测结论（MySQL 8.0.46，本机逐条试过 ALTER ADD COLUMN / TRUNCATE /
  //    DROP+CREATE / FLUSH TABLES 四种）：服务端会【透明重备】，从不返回 1243。
  //    因此本模块的"句柄失效后重备并重试"那条分支在本机是兜底而非常态 ——
  //    保留它是因为服务端的这个行为随版本与 optimizer_switch 变化，
  //    不能靠"我这里不会发生"来保证。stmt_reprepares 在这里必须是 0。
  const std::string sql =
      "SELECT id FROM " + std::string(kProbeTable) + " WHERE id = ?";
  ASSERT_TRUE(pool_->acquire()
                  ->run(sql, {Value::from_i64(1)})
                  .ok());  // 第一次：走 prepare 并缓存
  {
    Lease lease = pool_->acquire();
    ASSERT_TRUE(lease->exec("ALTER TABLE " + std::string(kProbeTable) +
                            " ADD COLUMN tmp INT NULL"));
  }
  pool_->reset_stats();
  {
    Lease lease = pool_->acquire();
    const ResultSet rs = lease->run(sql, {Value::from_i64(1)});
    ASSERT_TRUE(rs.ok()) << rs.error().format();
    EXPECT_EQ(rs.size(), 0);
  }
  const PoolStats stats = pool_->stats();
  EXPECT_EQ(stats.prepares, 0) << "应当命中缓存，没有重新 prepare";
  EXPECT_EQ(stats.stmt_cache_hits, 1);
  EXPECT_EQ(stats.stmt_reprepares, 0)
      << "若本机服务端开始返回 1243/1615，本用例的期望值必须一并更新并补测重试分支";

  // 新列必须真的能被取到 —— 证明"表结构变了"这件事没有让结果集元信息错位
  {
    Lease lease = pool_->acquire();
    ASSERT_TRUE(lease->run("INSERT INTO " + std::string(kProbeTable) +
                               " (id, s, tmp) VALUES (?, ?, ?)",
                           {Value::from_i64(1), Value::from_text("x"),
                            Value::null()})
                      .ok());
    const ResultSet rs = lease->run(
        "SELECT id, tmp FROM " + std::string(kProbeTable) + " WHERE id = ?",
        {Value::from_i64(1)});
    ASSERT_TRUE(rs.ok()) << rs.error().format();
    ASSERT_EQ(rs.column_count(), 2u);
    EXPECT_EQ(rs.columns()[1], "tmp");
    ASSERT_EQ(rs.size(), 1u);
    EXPECT_EQ(rs.at(0).i64(0), 1);
    EXPECT_TRUE(rs.at(0).is_null(1));
  }
}

// =============================================================================
// G. 会话设置（[D-17] 存 UTC / utf8mb4 / 超时确实生效）
// =============================================================================

TEST_F(PoolTest, SessionIsPinnedToUtcAndUtf8mb4) {
  Lease lease = pool_->acquire();
  Session& s = lease.session();
  const ResultSet tz = s.run(
      "SELECT @@session.time_zone AS tz, @@session.character_set_client AS cs, "
      "@@session.max_execution_time AS met, @@session.innodb_lock_wait_timeout AS lw");
  ASSERT_TRUE(tz.ok()) << tz.error().format();
  ASSERT_EQ(tz.size(), 1u);
  EXPECT_EQ(tz.at(0).str(0), "+00:00");
  EXPECT_EQ(tz.at(0).str(1), "utf8mb4");
  EXPECT_EQ(tz.at(0).i64(2), 10000);
  EXPECT_EQ(tz.at(0).i64(3), 5);

  // NOW() 与 UTC_TIMESTAMP() 必须相等 —— 不等就说明写进去的 DATETIME 是本地时间
  const ResultSet same = s.run("SELECT NOW() = UTC_TIMESTAMP() AS same");
  ASSERT_TRUE(same.ok());
  EXPECT_EQ(same.at(0).i64(0), 1) << "[D-17] 约定存 UTC，会话时区必须锁死成 +00:00";
}

TEST_F(PoolTest, ReadTimeoutBoundsAHungStatement) {
  // 串行化把"最慢的一条语句"放大成"所有请求之和"，因此 read_timeout 不是优化而是
  // 必需品：它把一次楔死限制在超时值内，而不是让全站永久卡住。
  Pool bounded;
  PoolOptions o = options_;
  o.read_timeout_s = 1;
  ASSERT_TRUE(bounded.init(o)) << bounded.describe();
  {
    Lease lease = bounded.acquire();
    ASSERT_TRUE(lease.valid());
    const auto start = std::chrono::steady_clock::now();
    const ResultSet rs = lease->run("SELECT SLEEP(5) AS slept");
    const auto elapsed = std::chrono::steady_clock::now() - start;
    ASSERT_TRUE(rs.failed());
    EXPECT_EQ(rs.error().code, 2013u);  // CR_SERVER_LOST
    EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(), 4)
        << "read_timeout 没有生效";
  }
  bounded.shutdown();
}

TEST_F(PoolTest, LockWaitTimeoutIsBounded) {
  // MySQL 的 innodb_lock_wait_timeout 默认 50s。串行化模型下那等于"一条锁等待把全站
  // 拖停 50s"，所以连接建立时必须压到秒级。验证方式：裸连接按住行锁，池里的 UPDATE
  // 必须在配置的超时内失败。
  // ⚠️ 准备数据与等待锁超时【必须分属两段作用域】：[D-21] 的互斥量是进程级的，
  // 还着主池的租约时去取另一个池的租约会永远等下去（这正是上面 shutdown 的坑）。
  {
    Lease lease = pool_->acquire();
    ASSERT_TRUE(lease.valid());
    ASSERT_TRUE(lease->run("INSERT INTO " + std::string(kProbeTable) + " (id, s) VALUES (?, ?)",
                           {Value::from_i64(1), Value::from_text("locked")})
                      .ok());
    ASSERT_GT(lease.session().connection_id(), 0u);
  }

  RawConnection holder;
  ASSERT_TRUE(holder.connect(options_));
  ASSERT_TRUE(holder.exec("START TRANSACTION"));
  ASSERT_TRUE(holder.exec("SELECT id FROM " + std::string(kProbeTable) +
                          " WHERE id = 1 FOR UPDATE"));

  Pool bounded;
  PoolOptions o = options_;
  o.lock_wait_timeout_s = 2;
  ASSERT_TRUE(bounded.init(o));
  {
    Lease blocked = bounded.acquire();
    ASSERT_TRUE(blocked.valid());
    const auto start = std::chrono::steady_clock::now();
    const ResultSet rs = blocked->run(
        "UPDATE " + std::string(kProbeTable) + " SET s = ? WHERE id = ?",
        {Value::from_text("blocked"), Value::from_i64(1)});
    const auto elapsed = std::chrono::steady_clock::now() - start;
    ASSERT_TRUE(rs.failed()) << "行锁未超时，测试前提不成立";
    EXPECT_EQ(rs.error().code, 1205u);  // ER_LOCK_WAIT_TIMEOUT
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 1000);
  }
  bounded.shutdown();
  ASSERT_TRUE(holder.exec("ROLLBACK"));
}

// =============================================================================
// H. 断连与恢复（本模块唯一"自动做事"的地方）
// =============================================================================

TEST_F(PoolTest, KilledConnectionFailsOnceThenRecoversOnItsOwn) {
  // 契约（见头文件【为什么不自动重试失败的语句】）：
  //   ① 断连后第一条语句【如实失败】——不重连、不重试；
  //   ② 连接被标记可疑，下一次取用时自动重建，运维什么都不用做；
  //   ③ 从此恢复。
  // 先预热一次：把语句放进缓存，第二次执行才会走 execute 而不是 prepare，
  // 于是"断连"这个错误是干净的 2013（否则会先死在 prepare 上，见 run() 里的注释）。
  const std::string sql = "SELECT id FROM " + std::string(kProbeTable) + " WHERE id = ?";
  unsigned long long id = 0;
  {
    Lease lease = pool_->acquire();
    ASSERT_TRUE(lease->run(sql, {Value::from_i64(1)}).ok());
    id = lease.session().connection_id();
    ASSERT_GT(id, 0u);
  }
  {
    RawConnection killer;
    ASSERT_TRUE(killer.connect(options_));
    ASSERT_TRUE(killer.exec("KILL " + std::to_string(id)));
  }
  pool_->reset_stats();
  {
    Lease lease = pool_->acquire();
    ASSERT_TRUE(lease.valid());
    const ResultSet dead = lease->run(sql, {Value::from_i64(1)});
    ASSERT_TRUE(dead.failed());
    EXPECT_EQ(dead.error().code, 2013u) << dead.error().format();
    EXPECT_TRUE(dead.error().server_side());
    EXPECT_EQ(pool_->stats().statements, 0) << "失败的语句不该被计入成功";
  }
  {
    Lease lease = pool_->acquire();
    ASSERT_TRUE(lease.valid());
    const ResultSet rs = lease->run(sql, {Value::from_i64(1)});
    ASSERT_TRUE(rs.ok()) << rs.error().format();
    EXPECT_GT(pool_->stats().reconnects, 0);
  }
  {
    Lease lease = pool_->acquire();
    ASSERT_TRUE(lease->run(sql, {Value::from_i64(1)}).ok()) << "重建后应当彻底恢复";
  }
}

TEST_F(PoolTest, DeadStatementIsNeverReplayed) {
  // 「不重试」的另一半：不重连重放是为了不把一次写变成两次写。
  // 这条用例把它钉死 —— 断连时正在写的行，恢复后【必须】仍然不存在。
  const std::string insert =
      "INSERT INTO " + std::string(kProbeTable) + " (id, s) VALUES (?, ?)";
  const std::string select =
      "SELECT id FROM " + std::string(kProbeTable) + " WHERE id = ?";
  unsigned long long id = 0;
  {
    Lease lease = pool_->acquire();
    ASSERT_TRUE(lease->run(insert, {Value::from_i64(1), Value::from_text("a")}).ok());
    ASSERT_TRUE(lease->run(insert, {Value::from_i64(2), Value::from_text("b")}).ok());
    id = lease.session().connection_id();
  }
  {
    RawConnection killer;
    ASSERT_TRUE(killer.connect(options_));
    ASSERT_TRUE(killer.exec("KILL " + std::to_string(id)));
  }
  {
    Lease lease = pool_->acquire();
    const ResultSet dead = lease->run(insert, {Value::from_i64(3), Value::from_text("c")});
    EXPECT_TRUE(dead.failed());
  }
  {
    Lease lease = pool_->acquire();  // 此处自动重建
    const ResultSet rs = lease->run(select, {Value::from_i64(3)});
    ASSERT_TRUE(rs.ok()) << rs.error().format();
    EXPECT_EQ(rs.size(), 0u) << "被断连打断的 INSERT 绝不能被重放";
  }
}

TEST_F(PoolTest, PrepareOnDeadConnectionAlsoMarksItSuspect) {
  // 断连后第一条【没被缓存过】的语句会先死在 prepare 上。
  // 若这里不标记连接可疑，它会被当成健康的归还 → 之后每条新语句都失败一次，
  // 而缓存里已命中的语句却正常，那是最难查的一种"半死"状态。
  unsigned long long id = 0;
  {
    Lease lease = pool_->acquire();
    ASSERT_TRUE(lease->run("SELECT 1 AS warm").ok());
    id = lease.session().connection_id();
  }
  {
    RawConnection killer;
    ASSERT_TRUE(killer.connect(options_));
    ASSERT_TRUE(killer.exec("KILL " + std::to_string(id)));
  }
  {
    Lease lease = pool_->acquire();
    const ResultSet dead = lease->run("SELECT 2 + 2 AS four");  // 从未 prepare 过
    ASSERT_TRUE(dead.failed());
    EXPECT_EQ(dead.error().code, 2013u) << dead.error().format();
  }
  {
    Lease lease = pool_->acquire();
    EXPECT_TRUE(lease->run("SELECT 2 + 2 AS four").ok());
  }
  EXPECT_GT(pool_->stats().reconnects, 0);
}

TEST_F(PoolTest, IdlePingDetectsSilentlyDeadConnection) {
  Pool pingy;
  PoolOptions o = options_;
  o.idle_ping_ms = 1;  // 任何一次取用前都先 ping
  ASSERT_TRUE(pingy.init(o));
  unsigned long long id = 0;
  {
    Lease lease = pingy.acquire();
    id = lease.session().connection_id();
  }
  {
    RawConnection killer;
    ASSERT_TRUE(killer.connect(options_));
    ASSERT_TRUE(killer.exec("KILL " + std::to_string(id)));
  }
  pingy.reset_stats();
  {
    Lease lease = pingy.acquire();
    ASSERT_TRUE(lease.valid());
    EXPECT_TRUE(lease->run("SELECT 1 AS one").ok());
  }
  EXPECT_GT(pingy.stats().pings, 0);
  pingy.shutdown();
}

TEST_F(PoolTest, SplitStatementsMatchesExecScriptCount) {
  // split_statements() 是 exec_script() 的公开版（同一套实现）。两者必须一致 ——
  // main() 的建表核对就是靠"切出 N 条"与"执行 N 条"相等来判断脚本没被截断，
  // 而那个 N 来自 split_statements()。
  const std::string script =
      "-- 注释里的分号 ; 不算数\n"
      "/* 块注释 ; 也不算数 */\n"
      "CREATE TABLE IF NOT EXISTS oj_split_probe (id INT PRIMARY KEY, n VARCHAR(8) DEFAULT 'a;b');\n"
      "INSERT INTO oj_split_probe (id) VALUES (1);\n"
      "DROP TABLE oj_split_probe;\n";
  DbError error;
  const std::vector<std::string> statements = split_statements(script, &error);
  EXPECT_TRUE(error.ok()) << error.format();
  EXPECT_EQ(statements.size(), 3u);
  // 注释必须已经被剥掉：残留的 "--" 会被 MySQL 当成减号运算符而报错
  for (const std::string& statement : statements) {
    EXPECT_EQ(statement.find("--"), std::string::npos) << statement;
    EXPECT_EQ(statement.find("/*"), std::string::npos) << statement;
  }
  EXPECT_EQ(statements[0].rfind("CREATE TABLE IF NOT EXISTS", 0), 0u);

  Lease lease = pool_->acquire();
  ASSERT_TRUE(lease.valid());
  std::vector<std::string> failed;
  EXPECT_EQ(lease->exec_script(script, &failed), statements.size());
  EXPECT_TRUE(failed.empty()) << (failed.empty() ? "" : failed[0]);
}

TEST_F(PoolTest, SplitStatementsReportsUnterminatedQuoteInsteadOfGuessing) {
  DbError error;
  EXPECT_TRUE(split_statements("SELECT '没闭合", &error).empty());
  EXPECT_FALSE(error.ok());
  EXPECT_NE(error.message.find("未闭合"), std::string::npos);
  EXPECT_TRUE(split_statements("SELECT 1 /* 没闭合", &error).empty());
  EXPECT_FALSE(error.ok());
}

TEST_F(PoolTest, SplitStatementsOfCommentOnlyScriptIsEmpty) {
  DbError error;
  EXPECT_TRUE(split_statements("-- 只有注释\n/* 和块注释 */\n", &error).empty());
  EXPECT_TRUE(error.ok()) << error.format();
}

// gtest 在一个进程里跑完所有用例，而 common::config() 是【进程级单例】。
// 下面这个守卫保证：无论断言在哪一步失败、还是测试被中断，config() 都会被还原 —
// 清理代码写在断言之后是坏模式（一旦中途失败，脏状态会泄漏给后续所有用例）。
class ConfigGuard {
 public:
  ConfigGuard() { oj::common::reset_config(); }
  ~ConfigGuard() { oj::common::reset_config(); }
  ConfigGuard(const ConfigGuard&) = delete;
  ConfigGuard& operator=(const ConfigGuard&) = delete;
};

TEST_F(PoolTest, InitFromConfigReadsTheEnvKeys) {
  // init_from_config 是 main() 真正会用到的入口（§2.3：load_config 之后），
  // 它与 .env 键名之间的映射必须被覆盖，否则一条拼错的键名要到 T0.5 接线时才暴露。
  const std::string env_path = "/tmp/oj_pool_selftest.env";
  {
    std::ofstream out(env_path);
    ASSERT_TRUE(out.is_open());
    out << "MYSQL_HOST=" << options_.host << "\n"
        << "MYSQL_PORT=" << options_.port << "\n"
        << "MYSQL_USER=" << options_.user << "\n"
        << "MYSQL_PASSWORD=" << options_.password << "\n"
        << "MYSQL_DB=" << options_.database << "\n";
  }
  const ConfigGuard guard;
  const oj::common::LoadResult loaded = oj::common::load_config(env_path);
  ASSERT_TRUE(loaded.ok) << oj::common::format_diagnostics(loaded);
  EXPECT_EQ(oj::common::config().mysql_host, options_.host);
  EXPECT_EQ(oj::common::config().mysql_port, static_cast<int>(options_.port));
  EXPECT_EQ(oj::common::config().mysql_db, options_.database);

  Pool from_config;
  std::vector<std::string> problems;
  EXPECT_TRUE(from_config.init_from_config(&problems))
      << (problems.empty() ? "" : problems[0]);
  EXPECT_TRUE(from_config.ready());
  {
    Lease lease = from_config.acquire();
    ASSERT_TRUE(lease.valid());
    EXPECT_EQ(lease.session().host(), options_.host);
    EXPECT_EQ(lease.session().port(), options_.port);
    EXPECT_EQ(lease.session().database(), options_.database);
    EXPECT_TRUE(lease->run("SELECT 1 AS one").ok());
  }
  from_config.shutdown();
  ::unlink(env_path.c_str());
}

TEST_F(PoolTest, NestedAcquireOnSameThreadIsRejectedNotHung) {
  // [D-21] 的全局互斥量 + RAII 组合下最容易写出来的错：持有租约时又取一条。
  // 它的天然表现是"服务无日志地卡死"，因此 acquire() 把它变成一条 ERROR + 无效租约。
  // 注意这条用例在加防护之前会直接把测试进程挂住 —— 这正是它值得存在的理由。
  Lease outer = pool_->acquire();
  ASSERT_TRUE(outer.valid());
  EXPECT_TRUE(outer->run("SELECT 1 AS one").ok());
  const auto start = std::chrono::steady_clock::now();
  Lease nested = pool_->acquire();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_FALSE(nested.valid()) << "应当被拒绝，而不是让调用方无限期等待";
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 500);
  // 拒绝之后外层租约必须毫发无损
  EXPECT_TRUE(outer->run("SELECT 2 AS two").ok());
}

TEST_F(PoolTest, AcquireIsUsableAgainAfterReturningTheLease) {
  // 上一条用例的"补偿"：租约正常归还后，同一线程立刻再取必须立刻成功。
  // 若自死锁防护把 lease_owner_ 漏清，这里会一直失败。
  {
    Lease first = pool_->acquire();
    ASSERT_TRUE(first.valid());
  }
  Lease second = pool_->acquire();
  ASSERT_TRUE(second.valid());
  EXPECT_TRUE(second->run("SELECT 1 AS one").ok());
}

TEST_F(PoolTest, LeaseMoveConstructionTransfersOwnershipExactlyOnce) {
  // Lease 会被 DAO 大量写成 `auto lease = pool.acquire();` 与 `l = std::move(l)`。
  // 移动语义的 bug 后果极重：若源租约没被清空，同一条连接会被【归还两次】——
  // 空闲表里出现重复指针 → 两个租约拿到同一条连接 → 共享连接上的数据竞争，
  // 而且只在并发下显形。所以这条用例的核心断言是【计数】，不是"能不能用"。
  pool_->reset_stats();
  {
    Lease first = pool_->acquire();
    ASSERT_TRUE(first.valid());
    ASSERT_TRUE(first->run("SELECT 1 AS one").ok());

    Lease moved(std::move(first));
    EXPECT_TRUE(moved.valid());
    EXPECT_FALSE(first.valid()) << "被移动走的租约必须失效，否则连接会被归还两次";
    EXPECT_FALSE(static_cast<bool>(first));
    EXPECT_TRUE(moved->run("SELECT 2 AS two").ok());
    EXPECT_EQ(pool_->stats().active_leases, 1) << "移动不应增减在途租约数";
    EXPECT_EQ(pool_->stats().acquires, 1);
  }
  EXPECT_EQ(pool_->stats().active_leases, 0);
  EXPECT_EQ(pool_->stats().acquires, 1) << "归还不得再次计一次 acquire";

  // 空闲表里必须仍然【恰好有一条】连接（重复归还会让这里变成 2）
  Lease again = pool_->acquire();
  ASSERT_TRUE(again.valid());
  EXPECT_EQ(pool_->stats().active_leases, 1);
  EXPECT_TRUE(again->run("SELECT 3 AS three").ok());
  EXPECT_EQ(pool_->stats().active_leases, 1) << "反复取还不得让在途租约数漂移";
}

TEST_F(PoolTest, LeaseMoveAssignmentTransfersAndSelfAssignIsNoOp) {
  pool_->reset_stats();
  {
    Lease source = pool_->acquire();
    ASSERT_TRUE(source.valid());
    ASSERT_TRUE(source->run("SELECT 1 AS one").ok());

    Lease target;  // 空租约（拿不到第二个：D-21 的自死锁防护会拒掉嵌套 acquire）
    target = std::move(source);
    EXPECT_TRUE(target.valid());
    EXPECT_FALSE(source.valid());
    EXPECT_TRUE(target->run("SELECT 2 AS two").ok());
    EXPECT_EQ(pool_->stats().active_leases, 1);

    // 自赋值必须是【无操作】：不能把连接归还真了、又继续用已归还的句柄。
    // operator= 里的 `this != &other` 守卫就是为这一条存在的。
    Lease& alias = target;
    target = std::move(alias);
    EXPECT_TRUE(target.valid()) << "自赋值不应作废租约";
    EXPECT_TRUE(target->run("SELECT 3 AS three").ok());
    EXPECT_EQ(pool_->stats().active_leases, 1) << "自赋值不得重复归还连接";

    // 再移动一次：链式的所有权传递，每次都只能有一份
    Lease third(std::move(target));
    EXPECT_FALSE(target.valid());
    EXPECT_TRUE(third.valid());
    EXPECT_TRUE(third->run("SELECT 4 AS four").ok());
    EXPECT_EQ(pool_->stats().active_leases, 1);
  }
  EXPECT_EQ(pool_->stats().active_leases, 0);
  Lease again = pool_->acquire();
  ASSERT_TRUE(again.valid());
  EXPECT_TRUE(again->run("SELECT 5 AS five").ok());
}

TEST_F(PoolTest, MoveAssigningFromAMovedFromLeaseIsHarmless) {
  // 上面那条链式移动留下的源租约是"空的"，再拿它去移动赋值应当什么也不发生。
  // 漏掉这一步的话，悬空租约的 conn_ 可能被当成有效句柄交出去。
  pool_->reset_stats();
  Lease original = pool_->acquire();
  ASSERT_TRUE(original.valid());
  Lease empty;
  empty = std::move(original);
  ASSERT_TRUE(empty.valid());
  Lease ghost;
  ghost = std::move(original);  // original 早已被移空
  EXPECT_FALSE(ghost.valid());
  EXPECT_TRUE(empty->run("SELECT 1 AS one").ok()) << "不该影响仍然有效的那个租约";
  EXPECT_EQ(pool_->stats().active_leases, 1);
}

TEST_F(PoolTest, PingOnLiveConnectionSucceeds) {
  Lease lease = pool_->acquire();
  EXPECT_TRUE(lease->ping());
  EXPECT_TRUE(lease->ping());
}

// =============================================================================
// I. 生命周期与失效路径
// =============================================================================

TEST_F(PoolTest, ShutdownMakesSubsequentAcquireInvalid) {
  Pool dead;
  ASSERT_TRUE(dead.init(options_));
  {
    Lease lease = dead.acquire();
    ASSERT_TRUE(lease.valid());
  }
  dead.shutdown();
  EXPECT_FALSE(dead.ready());
  Lease lease = dead.acquire();
  EXPECT_FALSE(lease.valid());
  EXPECT_FALSE(lease.session().valid());
}

TEST_F(PoolTest, InvalidLeaseIsInertNotACrash) {
  Lease none;
  EXPECT_FALSE(none.valid());
  EXPECT_FALSE(static_cast<bool>(none));
  const ResultSet rs = none->run("SELECT 1");
  EXPECT_TRUE(rs.failed());
  EXPECT_FALSE(rs.error().server_side());
  EXPECT_FALSE(none->exec("SELECT 1"));

  Pool uninitialized;  // 从未 init
  Lease lease = uninitialized.acquire();
  EXPECT_FALSE(lease.valid());
  EXPECT_TRUE(lease->run("SELECT 1").failed());
}

TEST_F(PoolTest, SessionFromReturnedLeaseFailsInsteadOfUsingFreedState) {
  // 租约析构后 session() 指向的 Session 仍在（它只被清空了 conn_），任何语句都必须
  // 返回错误 —— 共享连接下"误用已归还的连接"是极难查的一类错，这里让它响亮地失败。
  Session* escaped = nullptr;
  {
    Lease lease = pool_->acquire();
    escaped = &lease.session();
    ASSERT_TRUE(escaped->run("SELECT 1").ok());
  }
  EXPECT_TRUE(escaped->run("SELECT 1").failed());
  EXPECT_FALSE(escaped->exec("SET SESSION time_zone = '+00:00'"));
}

TEST_F(PoolTest, PoolOfSizeTwoWorksButIsStillSerialized) {
  // size>1 不提高吞吐（[D-21] 的全局互斥量才是瓶颈），但必须能正确建立、归还、
  // 并且仍然串行 —— 断言 max_in_flight == 1 就是在守住这条语义。
  Pool dual;
  PoolOptions o = options_;
  o.size = 2;
  ASSERT_TRUE(dual.init(o)) << dual.describe();
  EXPECT_NE(dual.describe().find("size=2"), std::string::npos);

  std::atomic<int> in_flight{0};
  std::atomic<int> max_in_flight{0};
  std::vector<std::thread> workers;
  for (int t = 0; t < 4; ++t) {
    workers.emplace_back([&] {
      for (int i = 0; i < 20; ++i) {
        Lease lease = dual.acquire();
        if (!lease.valid()) continue;
        const int now = in_flight.fetch_add(1) + 1;
        int expected = max_in_flight.load();
        while (now > expected && !max_in_flight.compare_exchange_weak(expected, now)) {
        }
        lease->run("SELECT 1 AS one");
        in_flight.fetch_sub(1);
      }
    });
  }
  for (std::thread& w : workers) w.join();
  EXPECT_EQ(max_in_flight.load(), 1);
  dual.shutdown();
}

TEST_F(PoolTest, OptionsRoundTripsInitValues) {
  // options() 这轮从"无锁返回引用"改成了"返回副本"（原写法与 init() 并发时是数据竞争）。
  // 改了签名就得有测试兜住，否则下一次再改回引用没人会发现。
  Pool custom;
  PoolOptions o = options_;
  o.read_timeout_s = 11;
  o.write_timeout_s = 12;
  o.lock_wait_timeout_s = 3;
  o.connect_timeout_s = 4;
  o.stmt_cache_capacity = 7;
  o.idle_ping_ms = 999;
  ASSERT_TRUE(custom.init(o)) << custom.describe();

  const PoolOptions got = custom.options();
  EXPECT_EQ(got.host, o.host);
  EXPECT_EQ(got.port, o.port);
  EXPECT_EQ(got.user, o.user);
  EXPECT_EQ(got.database, o.database);
  EXPECT_EQ(got.connect_timeout_s, 4u);
  EXPECT_EQ(got.read_timeout_s, 11u);
  EXPECT_EQ(got.write_timeout_s, 12u);
  EXPECT_EQ(got.lock_wait_timeout_s, 3u);
  EXPECT_EQ(got.stmt_cache_capacity, 7u);
  EXPECT_EQ(got.idle_ping_ms, 999u);
  // options() 是【内存里的配置快照】，与 describe() 不同：它本来就含密码。
  // 断言写在这里是为了把这个区别钉死 —— 想要"可安全打印的一行"应该用 describe()。
  EXPECT_EQ(got.password, o.password);
  custom.shutdown();
}

TEST_F(PoolTest, DescribeOmitsPasswordAndListsTimeouts) {
  // 用例名原先是 DescribeMasksNothingButListsTimeouts，但它【从不断言密码】——
  // 而头文件注释这轮已从"密码已掩码"改成"压根不输出密码"，名字就成了一句空承诺。
  //
  // 【为什么不用 `desc.find(password) == npos` 来断言】
  //   本机默认密码恰好也是 "oj"，而 host / db / user 三处都含 "oj"，那个断言
  //   第一版就写出来过，必然失败；而改成"换个有辨识度的密码"也不行 ——
  //   `oj` 这个账号没有 CREATE USER 权限（实测 1227），造不出这样一个可连上的账号。
  //   值依赖的断言在这里天然不可靠，所以改成【与取值无关】的断言：
  //   把 describe() 里所有 "key=" 的 key 抠出来，与期望集合逐一比对。
  //   这样无论将来往里加的是 password / pw / secret 还是别的字段名，都会红。
  const std::string desc = pool_->describe();
  EXPECT_NE(desc.find(options_.host), std::string::npos);
  EXPECT_NE(desc.find(options_.database), std::string::npos);
  EXPECT_NE(desc.find("read"), std::string::npos);
  EXPECT_NE(desc.find("lock_wait"), std::string::npos);

  std::vector<std::string> keys;
  for (std::size_t i = 0; i < desc.size(); ++i) {
    if (desc[i] != '=' || i == 0) continue;
    std::size_t begin = i;
    while (begin > 0 && (std::isalnum(static_cast<unsigned char>(desc[begin - 1])) != 0 ||
                         desc[begin - 1] == '_')) {
      --begin;
    }
    if (begin == i) continue;  // "）=" 这种：前一个字符不是标识符，跳过
    keys.push_back(desc.substr(begin, i - begin));
  }
  // describe() 的全部 "key=" 就是这五个 —— 多一个就是有人往里塞了不该进日志的东西
  const std::vector<std::string> expected = {"user", "size", "lock_wait", "max_exec",
                                             "stmt_cache"};
  EXPECT_EQ(keys, expected);
  EXPECT_EQ(desc.find("pass"), std::string::npos) << "连 'password' 这个字段名都不该出现";
}

// =============================================================================
// J. DDL 脚本切分（§2.1-A 第 1 条的启动幂等建表靠它）
// =============================================================================

TEST_F(PoolTest, ExecScriptSplitsCommentsQuotesAndSemicolons) {
  const std::string script =
      "-- 建表；这一行注释里也有分号\n"
      "/* 块注释 ; 也不算数 */\n"
      "CREATE TABLE IF NOT EXISTS oj_pool_script_probe ("
      "  id INT PRIMARY KEY,"
      "  name VARCHAR(32) NOT NULL DEFAULT 'a;b'"
      ") ENGINE=InnoDB;\n"
      "INSERT INTO oj_pool_script_probe (id, name) VALUES (1, 'x;y');\n"
      "DROP TABLE oj_pool_script_probe;\n";
  std::vector<std::string> failed;
  {
    // 必须在【自己的作用域】里跑完脚本：还着租约时再去取第二条租约会被
    // acquire() 的自死锁防护拒掉（见下面 NestedAcquireOnSameThreadIsRejected）
    Lease script_lease = pool_->acquire();
    const std::size_t done = script_lease->exec_script(script, &failed);
    EXPECT_EQ(done, 3u) << "应当切成 3 条";
    EXPECT_TRUE(failed.empty()) << (failed.empty() ? "" : failed[0]);
  }

  Lease check = pool_->acquire();
  const ResultSet rs = check->run(
      "SELECT COUNT(*) FROM information_schema.tables WHERE table_name = 'oj_pool_script_probe'");
  ASSERT_TRUE(rs.ok());
  EXPECT_EQ(rs.at(0).i64(0), 0) << "脚本最后一条应已把表删掉";
}

TEST_F(PoolTest, ExecScriptRejectsUnterminatedQuote) {
  Lease lease = pool_->acquire();
  std::vector<std::string> failed;
  EXPECT_EQ(lease->exec_script("INSERT INTO t VALUES ('未闭合", &failed), 0u);
  EXPECT_FALSE(failed.empty());
  EXPECT_NE(lease->error().message.find("未闭合"), std::string::npos);
}

}  // namespace