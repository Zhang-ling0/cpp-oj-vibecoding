// =============================================================================
// 题库加载器的单元测试（gtest）
// 被测：include/problem/loader.h + src/problem/loader.cpp（SPEC.md §9 M1 T1.2）
// 运行：tests/run_unit_tests.sh（即 ctest）
//
// 【为什么这批用例要真库】本模块的价值全在【与 MySQL 的真实交互】上：delete-then-insert
//   的同步语义、case_count 的同事务回填、tags 的 ON DUPLICATE KEY 取 id、幂等二次启动。
//   这些都不是 mock 能验证的 —— mock 掉的正是"最容易错"的那一层（§3.2 的外键、ENUM、
//   STRICT 模式下的列宽）。连不上库时【整体跳过】而不是失败，理由与 mysql_pool_test 相同。
//
// 【每条用例自建自销】题库文件写进 mkdtemp 出来的临时目录，slug 一律以 oj-selftest- 开头，
//   TearDown 时 DELETE 掉（外键 CASCADE 连带清子表）。为什么不复用仓库里的 problems/：
//   那是 §4.3 的题目录入工作流的操作对象，用例往里塞"故意写错"的题会把录题现场搞脏。
//   为什么不用随机名：崩溃后残留的行会被下一次 DELETE 顺带清掉，而固定名能让人一眼看出
//   "这是测试数据"，比一堆随机后缀的题好排查。
// =============================================================================
#include <gtest/gtest.h>

#include <dirent.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "common/config.h"
#include "db/mysql_pool.h"
#include "model/problem.h"
#include "problem/loader.h"

namespace {

using oj::db::Lease;
using oj::db::Pool;
using oj::db::PoolOptions;
using oj::db::ResultSet;
using oj::db::Value;
using oj::problem::LoadOptions;
using oj::problem::LoadReport;

constexpr const char* kSlugPrefix = "oj-selftest-";

// 把题库文本里的某一段换成另一段（只换第一处）。用于"造一道曾经合法、后来写坏的题"：
// 这正是 §4.3 第 6 步会发生的事 —— 改用例内容/数量后重启，DB 与缓存都要跟着刷新。
std::string replace_first(std::string text, const std::string& from, const std::string& to) {
  const std::size_t at = text.find(from);
  if (at == std::string::npos) return text;
  text.replace(at, from.size(), to);
  return text;
}

std::string env_or(const char* key, const char* fallback) {
  const char* value = std::getenv(key);
  return (value != nullptr && *value != '\0') ? std::string(value) : std::string(fallback);
}

PoolOptions base_options() {
  PoolOptions options;
  options.host = env_or("MYSQL_HOST", "127.0.0.1");
  options.port = static_cast<unsigned int>(std::atoi(env_or("MYSQL_PORT", "3306").c_str()));
  options.user = env_or("MYSQL_USER", "oj");
  options.password = env_or("MYSQL_PASSWORD", "oj");
  options.database = env_or("MYSQL_DB", "oj");
  return options;
}

class LoaderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::vector<std::string> problems;
    if (!Pool::instance().init(base_options(), &problems)) {
      std::string reason;
      for (const std::string& problem : problems) reason += problem + "; ";
      GTEST_SKIP() << "无法连接 MySQL，跳过题库加载器的用例（" << reason << "）";
    }
    {
      Lease lease = Pool::instance().acquire();
      ASSERT_TRUE(lease.valid());
      // 题库相关的 5 张表是 T0.3 建的；空库上直接跳过，而不是报一堆 unknown table
      const ResultSet existing = lease->run(
          "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = DATABASE() "
          "AND table_name IN (?,?,?,?,?)",
          {Value::from_text("problems"), Value::from_text("problem_cases"),
           Value::from_text("problem_params"), Value::from_text("tags"),
           Value::from_text("problem_tags")});
      if (existing.failed() || existing.at(0).u64(0) != 5) {
        GTEST_SKIP() << "题库相关的 5 张表不存在，请先跑一次 build/oj 完成建表（T0.3）";
      }
    }

    char template_dir[] = "/tmp/oj-loader-test-XXXXXX";
    const char* created = ::mkdtemp(template_dir);
    ASSERT_NE(created, nullptr);
    dir_ = created;
    cleanup_problems();
    oj::problem::clear_cache();
  }

  void TearDown() override {
    cleanup_problems();
    oj::problem::clear_cache();
    remove_dir();
    Pool::instance().shutdown();
  }

  // ---- 临时目录 ---------------------------------------------------------------
  void write_file(const std::string& file_name, const std::string& content) const {
    std::ofstream out(dir_ + "/" + file_name, std::ios::binary | std::ios::trunc);
    out << content;
  }
  void remove_dir() const {
    DIR* handle = ::opendir(dir_.c_str());
    if (handle == nullptr) return;
    while (const struct dirent* entry = ::readdir(handle)) {
      const std::string name = entry->d_name;
      if (name == "." || name == "..") continue;
      ::unlink((dir_ + "/" + name).c_str());
    }
    ::closedir(handle);
    ::rmdir(dir_.c_str());
  }

  // ---- 题库数据 ---------------------------------------------------------------
  void cleanup_problems() const {
    Lease lease = Pool::instance().acquire();
    if (!lease.valid()) return;
    // 触发器必须清：它会挡住后面所有用例对 problem_cases 的正常写入
    lease->exec("DROP TRIGGER IF EXISTS oj_selftest_block_cases");
    lease->run("DELETE FROM problems WHERE slug LIKE ?",
               {Value::from_text(std::string(kSlugPrefix) + "%")});
    lease->run("DELETE FROM tags WHERE name LIKE ?",
               {Value::from_text(std::string(kSlugPrefix) + "%")});
    // users 走 CASCADE 连带清掉 submissions / submission_case_results（§3.2）
    lease->run("DELETE FROM users WHERE username LIKE ?",
               {Value::from_text(std::string(kSlugPrefix) + "%")});
  }

  // 一道合法题（用例数可变）。slug 固定以 kSlugPrefix 开头，便于 TearDown 清理。
  //
  // 【为什么这里逐段拼字符串而不是写一段 R"(...)" 模板】
  //   裸 raw string 以 )" 收尾，而 JSON 文本里成对的 `":["` / `":"` 会让"内容"多吞一段 ——
  //   拼出来少一个引号，表现为"JSON 语法错误"，报错位置离真正的原因（我写错字面量）隔着
  //   两层，本轮就踩过一次。逐段拼虽然啰嗦，但错了一眼能看见。
  std::string problem_json(const std::string& slug, const std::string& method, int case_count) {
    // 三个标签都带 kSlugPrefix 前缀：TearDown 只删前缀匹配的行，漏掉前缀的标签会
    // 变成跨用例残留 —— 那正是"tags 表是全局的"这条事实最容易咬人的地方
    const std::string prefix(kSlugPrefix);
    const std::string tags = "{\"algorithm\":[\"" + prefix + "算法\",\"" + prefix +
                             "哈希表\"],\"source\":[\"" + prefix + "来源\"]}";
    std::ostringstream out;
    out << "{\"slug\":\"" << slug << "\",\"title\":\"自测题\",\"difficulty\":\"easy\","
        << "\"statement_md\":\"## 题面\\n\\n字符画：\\n```\\n  o\\n / \\\\\\n```\","
        << "\"hint_md\":\"提示\","
        << "\"signature\":\"class Solution { public: std::vector<int> " << method
        << "(std::vector<int>& nums);};\","
        << "\"method_name\":\"" << method << "\",\"return_type\":\"std::vector<int>\","
        << "\"code_template\":\"class Solution {};\",\"time_limit_ms\":3000,"
        << "\"mem_limit_kb\":131072,\"harness_prelude\":\"\",\"tags\":" << tags << ","
        << "\"params\":[{\"name\":\"nums\",\"cpp_type\":\"std::vector<int>&\"}],\"cases\":[";
    for (int i = 0; i < case_count; ++i) {
      if (i != 0) out << ",";
      out << "{\"args\":[[" << i << "]],\"expected\":[" << i << "]";
      if (i == 0) out << ",\"brief\":\"基础用例\"";
      out << "}";
    }
    out << "]}";
    return out.str();
  }

  LoadReport load() {
    LoadOptions options;
    options.dir = dir_;
    return oj::problem::load_all(options);
  }

  // ---- 查询辅助 ---------------------------------------------------------------
  unsigned long long scalar(const std::string& sql,
                            const std::vector<Value>& params = {}) {
    Lease lease = Pool::instance().acquire();
    if (!lease.valid()) return 0;
    const ResultSet result = lease->run(sql, params);
    if (result.failed() || result.empty()) return 0;
    return result.at(0).u64(0);
  }
  std::string column_of(const std::string& table, const std::string& column,
                       const std::string& where) {
    Lease lease = Pool::instance().acquire();
    if (!lease.valid()) return "<no-lease>";
    const ResultSet result =
        lease->run("SELECT " + column + " FROM " + table + " WHERE " + where, {});
    if (result.failed() || result.empty()) return "<missing>";
    return result.at(0).is_null(0) ? "<NULL>" : result.at(0).str(0);
  }

  std::string problem_column(const std::string& slug, const std::string& column) {
    Lease lease = Pool::instance().acquire();
    if (!lease.valid()) return "<no-lease>";
    const ResultSet result = lease->run("SELECT " + column + " FROM problems WHERE slug = ?",
                                        {Value::from_text(slug)});
    if (result.failed() || result.empty()) return "<missing>";
    return result.at(0).is_null(0) ? "<NULL>" : result.at(0).str(0);
  }

  std::string dir_;
};

// ============================================================================
// 入库：五张表的列映射（§4.1 的映射表逐行验证）
// ============================================================================

TEST_F(LoaderTest, WritesEveryMappedColumnIntoEveryTable) {
  write_file(std::string(kSlugPrefix) + "basic.json",
             problem_json(std::string(kSlugPrefix) + "basic", "basic", 3));

  const LoadReport report = load();
  ASSERT_TRUE(report.ok) << report.format();
  EXPECT_EQ(report.files_scanned, 1u);
  EXPECT_EQ(report.loaded, 1u);
  EXPECT_EQ(report.skipped, 0u);
  EXPECT_EQ(report.cases_written, 3u);
  EXPECT_EQ(report.params_written, 1u);
  EXPECT_EQ(report.tag_links_written, 3u);
  EXPECT_TRUE(report.problems.empty()) << report.format();

  const std::string slug = std::string(kSlugPrefix) + "basic";
  // problems：scalar 列 + 空字符串映射为 NULL（§4.1 的两条脚注）
  EXPECT_EQ(problem_column(slug, "title"), "自测题");
  EXPECT_EQ(problem_column(slug, "difficulty"), "easy");
  EXPECT_EQ(problem_column(slug, "method_name"), "basic");
  EXPECT_EQ(problem_column(slug, "return_type"), "std::vector<int>");
  EXPECT_EQ(problem_column(slug, "case_count"), "3");
  EXPECT_EQ(problem_column(slug, "time_limit_ms"), "3000");
  EXPECT_EQ(problem_column(slug, "mem_limit_kb"), "131072");
  EXPECT_EQ(problem_column(slug, "harness_prelude"), "<NULL>");   // 空串 → NULL
  EXPECT_NE(problem_column(slug, "hint_md"), "<NULL>");

  // problem_cases：case_idx 与数组下标对齐，文本列是 dump() 的结果
  EXPECT_EQ(scalar("SELECT COUNT(*) FROM problem_cases c JOIN problems p ON p.id = c.problem_id"
                   " WHERE p.slug = ?",
                   {Value::from_text(slug)}),
            3u);
  EXPECT_EQ(scalar("SELECT COUNT(*) FROM problem_cases c JOIN problems p ON p.id = c.problem_id"
                   " WHERE p.slug = ? AND c.case_idx = 0 AND c.brief = '基础用例'",
                   {Value::from_text(slug)}),
            1u);
  EXPECT_EQ(scalar("SELECT COUNT(*) FROM problem_cases c JOIN problems p ON p.id = c.problem_id"
                   " WHERE p.slug = ? AND c.case_idx = 1 AND c.brief IS NULL",
                   {Value::from_text(slug)}),
            1u);
  {
    Lease lease = Pool::instance().acquire();
    const ResultSet row = lease->run(
        "SELECT c.args_json, c.expected_json FROM problem_cases c "
        "JOIN problems p ON p.id = c.problem_id WHERE p.slug = ? AND c.case_idx = 1",
        {Value::from_text(slug)});
    ASSERT_EQ(row.size(), 1u) << row.error().format();
    EXPECT_EQ(row.at(0).str(0), "[[1]]");
    EXPECT_EQ(row.at(0).str(1), "[1]");
  }

  // problem_params：param_idx = 数组下标
  EXPECT_EQ(scalar("SELECT COUNT(*) FROM problem_params pm JOIN problems p ON p.id = pm.problem_id"
                   " WHERE p.slug = ? AND pm.param_idx = 0 AND pm.name = 'nums'"
                   " AND pm.cpp_type = 'std::vector<int>&'",
                   {Value::from_text(slug)}),
            1u);

  // tags + problem_tags：两个 algorithm + 一个 source
  EXPECT_EQ(scalar("SELECT COUNT(*) FROM problem_tags pt JOIN problems p ON p.id = pt.problem_id"
                   " JOIN tags t ON t.id = pt.tag_id WHERE p.slug = ?",
                   {Value::from_text(slug)}),
            3u);
  EXPECT_EQ(scalar("SELECT COUNT(*) FROM problem_tags pt JOIN problems p ON p.id = pt.problem_id"
                   " JOIN tags t ON t.id = pt.tag_id WHERE p.slug = ? AND t.category = 'source'",
                   {Value::from_text(slug)}),
            1u);

  // 缓存：judge worker 拿到的就是这份（§2.1-A「取题 + 读用例(缓存)」）
  EXPECT_EQ(oj::problem::cache_size(), 1u);
  const auto cached = oj::problem::find_by_slug(slug);
  ASSERT_NE(cached, nullptr);
  EXPECT_GT(cached->id, 0);                       // 落库后才有 id
  EXPECT_EQ(cached->case_count(), 3u);
  EXPECT_EQ(cached->params[0].cpp_type, "std::vector<int>&");
}

TEST_F(LoaderTest, ReloadingIsIdempotentAndKeepsTheSameProblemId) {
  const std::string slug = std::string(kSlugPrefix) + "idem";
  write_file(slug + ".json", problem_json(slug, "idem", 2));

  ASSERT_TRUE(load().ok);
  const std::string first_id = problem_column(slug, "id");
  const std::string first_created = problem_column(slug, "created_at");
  ASSERT_TRUE(load().ok);
  EXPECT_EQ(problem_column(slug, "id"), first_id) << "第二次启动不应产生新行";
  // created_at 不动：证明走的是 UPDATE 分支而不是"删了重插"（后者会让所有以 problems
  // 为参照的时间排序出现一次无意义的跳变）。updated_at 必须被刷新（§4.3 第 6 步）。
  EXPECT_EQ(problem_column(slug, "created_at"), first_created);
  EXPECT_NE(problem_column(slug, "updated_at"), "<NULL>");

  // 行数不变：delete-then-insert 的意义就在这里（§4.1 同步语义）
  EXPECT_EQ(scalar("SELECT COUNT(*) FROM problems WHERE slug = ?", {Value::from_text(slug)}), 1u);
  EXPECT_EQ(scalar("SELECT COUNT(*) FROM problem_cases c JOIN problems p ON p.id = c.problem_id"
                   " WHERE p.slug = ?",
                   {Value::from_text(slug)}),
            2u);
  // 标签行是【全局】的：重复同步不能把 tags 表撑大（ON DUPLICATE KEY UPDATE id=LAST_INSERT_ID）
  EXPECT_EQ(scalar("SELECT COUNT(*) FROM tags WHERE name LIKE ?",
                   {Value::from_text(std::string(kSlugPrefix) + "%")}),
            3u);
}

TEST_F(LoaderTest, DeletingACaseInJsonPropagatesToDatabase) {
  // SPEC §9 T1.2 的验收第 2 条：改 JSON 删掉第 3 个用例后重启，只剩 2 行且 case_count=2
  const std::string slug = std::string(kSlugPrefix) + "shrink";
  write_file(slug + ".json", problem_json(slug, "shrink", 3));
  ASSERT_TRUE(load().ok);
  ASSERT_EQ(scalar("SELECT COUNT(*) FROM problem_cases c JOIN problems p ON p.id = c.problem_id"
                   " WHERE p.slug = ?",
                   {Value::from_text(slug)}),
            3u);

  write_file(slug + ".json", problem_json(slug, "shrink", 2));
  const LoadReport report = load();
  ASSERT_TRUE(report.ok) << report.format();
  EXPECT_EQ(report.cases_written, 2u);
  EXPECT_EQ(scalar("SELECT COUNT(*) FROM problem_cases c JOIN problems p ON p.id = c.problem_id"
                   " WHERE p.slug = ?",
                   {Value::from_text(slug)}),
            2u);
  EXPECT_EQ(problem_column(slug, "case_count"), "2");
  EXPECT_EQ(oj::problem::cache()->front().case_count(), 2u);
}

// ============================================================================
// 跳过与失效（§5.6.1：不阻塞其他题 + 保留历史）
// ============================================================================

TEST_F(LoaderTest, InvalidProblemIsSkippedWithoutBlockingTheRest) {
  const std::string good = std::string(kSlugPrefix) + "good";
  const std::string bad = std::string(kSlugPrefix) + "bad";
  write_file(good + ".json", problem_json(good, "good", 2));
  // return_type 是 vector<int>，expected 却是 int → 层 ② 第 1 条
  write_file(bad + ".json", replace_first(problem_json(bad, "bad", 2), R"("expected":[0])",
                                          R"("expected":0)"));

  const LoadReport report = load();
  ASSERT_TRUE(report.ok) << report.format();   // 跳过不算失败：50 道题里 1 道写错不该起不来
  EXPECT_EQ(report.loaded, 1u);
  EXPECT_EQ(report.skipped, 1u);
  ASSERT_EQ(report.skipped_problems.size(), 1u);
  EXPECT_EQ(report.skipped_problems[0].file, bad + ".json");
  EXPECT_EQ(report.skipped_problems[0].slug, bad);
  ASSERT_FALSE(report.skipped_problems[0].reasons.empty());
  EXPECT_NE(report.format().find(bad + ".json"), std::string::npos);

  // 坏题不入库、不进缓存
  EXPECT_EQ(problem_column(bad, "id"), "<missing>");
  EXPECT_EQ(oj::problem::cache_size(), 1u);
  EXPECT_NE(oj::problem::find_by_slug(good), nullptr);
  EXPECT_EQ(oj::problem::find_by_slug(bad), nullptr);
}

TEST_F(LoaderTest, BrokenFilesAreSkippedWithAReason) {
  write_file(std::string(kSlugPrefix) + "empty.json", "");     // 0 字节：新建后忘了填
  write_file(std::string(kSlugPrefix) + "torn.json", "{\"slug\":");
  const std::string good = std::string(kSlugPrefix) + "good";
  write_file(good + ".json", problem_json(good, "good", 1));

  const LoadReport report = load();
  ASSERT_TRUE(report.ok) << report.format();
  EXPECT_EQ(report.files_scanned, 3u);
  EXPECT_EQ(report.loaded, 1u);
  EXPECT_EQ(report.skipped, 2u);
  for (const oj::problem::SkippedProblem& skipped : report.skipped_problems) {
    EXPECT_FALSE(skipped.reasons.empty()) << skipped.file;
  }
  EXPECT_EQ(oj::problem::cache_size(), 1u);
}

TEST_F(LoaderTest, AlreadySyncedProblemThatTurnsInvalidIsDeactivatedNotDeleted) {
  // 这是"不能删 problems 行"的直接验证：删行会 CASCADE 掉用户在那道题上的全部提交历史
  const std::string slug = std::string(kSlugPrefix) + "broken-later";
  write_file(slug + ".json", problem_json(slug, "brokenLater", 2));
  ASSERT_TRUE(load().ok);
  ASSERT_EQ(problem_column(slug, "case_count"), "2");

  // 同一个 slug，用例类型写坏了 → 第二次启动校验不通过
  write_file(slug + ".json", replace_first(problem_json(slug, "brokenLater", 2),
                                           R"("expected":[0])", R"("expected":"x")"));
  const LoadReport report = load();
  ASSERT_TRUE(report.ok) << report.format();
  EXPECT_EQ(report.skipped, 1u);
  EXPECT_EQ(report.deactivated, 1u);

  // 行还在（历史挂在上头），但用例被清空、case_count 归零 → 列表页不再出现它
  EXPECT_NE(problem_column(slug, "id"), "<missing>");
  EXPECT_EQ(problem_column(slug, "case_count"), "0");
  EXPECT_EQ(scalar("SELECT COUNT(*) FROM problem_cases c JOIN problems p ON p.id = c.problem_id"
                   " WHERE p.slug = ?",
                   {Value::from_text(slug)}),
            0u);
  EXPECT_EQ(oj::problem::cache_size(), 0u);
}

// ============================================================================
// 失败模式与只读模式
// ============================================================================

TEST_F(LoaderTest, ValidationOnlyModeTouchesNeitherDatabaseNorCacheContract) {
  const std::string slug = std::string(kSlugPrefix) + "dry";
  write_file(slug + ".json", problem_json(slug, "dry", 2));

  LoadOptions options;
  options.dir = dir_;
  options.write_to_db = false;   // 只解析校验并灌缓存（录题自检 / 单测）
  const LoadReport report = oj::problem::load_all(options);
  ASSERT_TRUE(report.ok) << report.format();
  EXPECT_EQ(report.loaded, 1u);
  EXPECT_EQ(report.cases_written, 0u);          // 没写库就没写进去
  EXPECT_EQ(oj::problem::cache_size(), 1u);     // 但缓存照灌（judge 只需要它）
  EXPECT_EQ(problem_column(slug, "id"), "<missing>");
}

TEST_F(LoaderTest, MissingDirectoryFailsLoudlyAndLeavesCacheEmpty) {
  oj::problem::clear_cache();
  LoadOptions options;
  options.dir = dir_ + "/不存在";
  const LoadReport report = oj::problem::load_all(options);
  EXPECT_FALSE(report.ok);
  ASSERT_FALSE(report.problems.empty());
  EXPECT_NE(report.problems[0].find("题库目录"), std::string::npos) << report.problems[0];
  EXPECT_EQ(oj::problem::cache_size(), 0u);
}

TEST_F(LoaderTest, EmptyDirectoryYieldsEmptyLibraryAndSuccess) {
  const LoadReport report = load();
  ASSERT_TRUE(report.ok) << report.format();
  EXPECT_EQ(report.files_scanned, 0u);
  EXPECT_EQ(report.loaded, 0u);
  EXPECT_EQ(oj::problem::cache_size(), 0u);
  EXPECT_NE(report.summary().find("扫描 0 个文件"), std::string::npos);
}

TEST_F(LoaderTest, NonJsonFilesInTheDirectoryAreIgnored) {
  const std::string slug = std::string(kSlugPrefix) + "only";
  write_file(slug + ".json", problem_json(slug, "only", 1));
  write_file("README.md", "题库说明");
  write_file(".hidden.json", "{}");            // 隐藏文件（.gitkeep 那类）
  write_file("backup.json.bak", "{}");

  const LoadReport report = load();
  ASSERT_TRUE(report.ok) << report.format();
  EXPECT_EQ(report.files_scanned, 1u);
  EXPECT_EQ(report.loaded, 1u);
}

// ============================================================================
// 缓存查询
// ============================================================================

TEST_F(LoaderTest, CacheLookupWorksBySlugAndId) {
  const std::string slug = std::string(kSlugPrefix) + "lookup";
  write_file(slug + ".json", problem_json(slug, "lookup", 1));
  ASSERT_TRUE(load().ok);

  const auto by_slug = oj::problem::find_by_slug(slug);
  ASSERT_NE(by_slug, nullptr);
  const auto by_id = oj::problem::find_by_id(by_slug->id);
  ASSERT_NE(by_id, nullptr);
  EXPECT_EQ(by_id->slug, slug);
  EXPECT_EQ(oj::problem::find_by_slug("nope"), nullptr);
  EXPECT_EQ(oj::problem::find_by_id(0), nullptr);       // id=0 是"未落库"的标记，不是有效 id
  EXPECT_EQ(oj::problem::find_by_id(999999999), nullptr);

  oj::problem::clear_cache();
  EXPECT_EQ(oj::problem::cache_size(), 0u);
  EXPECT_EQ(oj::problem::find_by_slug(slug), nullptr);
}

TEST_F(LoaderTest, OmittedOptionalFieldsBecomeNullColumns) {
  // §4.1 的两条脚注：hint_md / harness_prelude 空串落库为 NULL，brief 省略同样为 NULL。
  // 这一条单独验证，是因为"空串"与"NULL"在 DDL 里是两列不同的语义，而本仓库的连接
  // 强制了 STRICT_TRANS_TABLES —— 真正危险的错法是既没给值又没给 NULL。
  const std::string slug = std::string(kSlugPrefix) + "nulls";
  std::string json = replace_first(problem_json(slug, "nulls", 2), "\"hint_md\":\"提示\",", "");
  write_file(slug + ".json", json);

  const LoadReport report = load();
  ASSERT_TRUE(report.ok) << report.format();
  EXPECT_EQ(problem_column(slug, "hint_md"), "<NULL>");
  EXPECT_EQ(problem_column(slug, "harness_prelude"), "<NULL>");
  EXPECT_EQ(scalar("SELECT COUNT(*) FROM problem_cases c JOIN problems p ON p.id = c.problem_id"
                   " WHERE p.slug = ? AND c.brief IS NULL",
                   {Value::from_text(slug)}),
            1u);   // 第 2 个用例没写 brief
  // 缓存侧同口径：hint 为空串，前端据此不渲染提示 tab（D-47）
  const auto cached = oj::problem::find_by_slug(slug);
  ASSERT_NE(cached, nullptr);
  EXPECT_TRUE(cached->hint_md.empty());
}

TEST_F(LoaderTest, TwoProblemsWithSharedTagsReuseOneTagRow) {
  const std::string first = std::string(kSlugPrefix) + "first";
  const std::string second = std::string(kSlugPrefix) + "second";
  write_file(first + ".json", problem_json(first, "first", 1));
  write_file(second + ".json", problem_json(second, "second", 1));

  const LoadReport report = load();
  ASSERT_TRUE(report.ok) << report.format();
  EXPECT_EQ(report.loaded, 2u);
  // 两道题共用同样三个标签：tags 表里仍然只有三行，problem_tags 六行
  EXPECT_EQ(scalar("SELECT COUNT(*) FROM tags WHERE name LIKE ?",
                   {Value::from_text(std::string(kSlugPrefix) + "%")}),
            3u);
  EXPECT_EQ(scalar("SELECT COUNT(*) FROM problem_tags pt JOIN problems p ON p.id = pt.problem_id"
                   " JOIN tags t ON t.id = pt.tag_id WHERE p.slug IN (?,?)",
                   {Value::from_text(first), Value::from_text(second)}),
            6u);
  // 缓存按文件名排序（加载器对目录项排序），因此顺序可复现
  ASSERT_EQ(oj::problem::cache_size(), 2u);
  EXPECT_EQ(oj::problem::cache()->at(0).slug, first);
  EXPECT_EQ(oj::problem::cache()->at(1).slug, second);
}

TEST_F(LoaderTest, MultiByteTextSurvivesTheRoundTrip) {
  // utf8mb4 + MEDIUMTEXT 是"看起来对、存进去变问号"最容易发生的地方：4 字节的 emoji
  // 在 utf8mb3 下根本存不进去，而在 STRICT 模式下会整条报错。这里逐字节比回来。
  const std::string slug = std::string(kSlugPrefix) + "utf8";
  const std::string emoji_title = "\u4e2d\u6587\u6807\u9898 \U0001F680 emoji";
  const std::string json = replace_first(problem_json(slug, "utf8", 1), "\"title\":\"\u81ea\u6d4b\u9898\"",
                                          "\"title\":\"" + emoji_title + "\"");
  write_file(slug + ".json", json);

  const LoadReport report = load();
  ASSERT_TRUE(report.ok) << report.format();
  EXPECT_EQ(problem_column(slug, "title"), emoji_title);

  Lease lease = Pool::instance().acquire();
  ASSERT_TRUE(lease.valid());
  const ResultSet stored =
      lease->run("SELECT p.statement_md FROM problems p WHERE p.slug = ?",
                 {Value::from_text(slug)});
  ASSERT_EQ(stored.size(), 1u) << stored.error().format();
  // statement_md 里带中文 + 反斜杠（字符画）与换行转义，三者都要原样回来
  const std::string expected_statement = std::string("## \u9898\u9762\n\n\u5b57\u7b26\u753b\uff1a\n```\n  o\n / \\\n```");
  EXPECT_EQ(stored.at(0).str(0), expected_statement) << stored.at(0).str(0);
}

TEST_F(LoaderTest, DeactivationKeepsSubmissionHistory) {
  // loader.h【为什么不删"文件已删除"的题】那句话的可执行版本：
  // problems.submissions 是 ON DELETE CASCADE，所以"删题"会连带删掉用户的全部提交。
  // 本用例先造一份真实的提交历史，再让题目校验失败，最后断言历史一条不少 ——
  // 删 problems 行这个"更干净"的写法，会被这一条挡住。
  const std::string slug = std::string(kSlugPrefix) + "history";
  write_file(slug + ".json", problem_json(slug, "history", 2));
  ASSERT_TRUE(load().ok);

  const std::string problem_id = problem_column(slug, "id");
  ASSERT_NE(problem_id, "<missing>");
  {
    // ⚠️ 租约必须【用完就还】：[D-21] 的全局互斥量在同一线程里被二次 acquire 会触发池的
    // 自死锁防护并直接返回无效租约（不是阻塞）。踩过一次之后顺手记在这里 ——
    // 症状是"下一句 load_all 报连接池未就绪"，而真正的原因是本用例自己攥着租约。
    Lease lease = Pool::instance().acquire();
    ASSERT_TRUE(lease.valid());
    ASSERT_TRUE(lease->run("INSERT INTO users (username, password_hash, role, created_at) "
                           "VALUES (?, 'x', 'user', NOW())",
                           {Value::from_text(std::string(kSlugPrefix) + "user")})
                    .ok());
    // 用 JOIN 取 id，而不是把 id 取回应用层再塞回 SQL：少一次"文本转数字"的往返，
    // 也少一个"取回来的值不对"的排查点（外键报错只会说 problem_id 不存在）。
    const ResultSet inserted = lease->run(
        "INSERT INTO submissions (user_id, problem_id, code, status, verdict, case_passed, "
        "case_total, total_ms, max_mem_kb, created_at, finished_at) "
        "SELECT u.id, p.id, 'int main(){}', 'done', 'AC', 2, 2, 0, 0, NOW(), NOW() "
        "FROM users u JOIN problems p ON p.slug = ? WHERE u.username = ?",
        {Value::from_text(slug), Value::from_text(std::string(kSlugPrefix) + "user")});
    ASSERT_TRUE(inserted.ok()) << inserted.error().format();
    ASSERT_TRUE(lease->run("INSERT INTO submission_case_results (submission_id, case_idx, verdict, "
                           "passed, time_ms, mem_kb) SELECT id, 0, 'AC', 1, 1, 1 FROM submissions "
                           "WHERE user_id = (SELECT id FROM users WHERE username = ?)",
                           {Value::from_text(std::string(kSlugPrefix) + "user")})
                    .ok());
  }

  // 题目被改坏 → 第二次启动把用例清空、case_count 归零
  write_file(slug + ".json", replace_first(problem_json(slug, "history", 2),
                                           R"("expected":[0])", R"("expected":null)"));
  const LoadReport report = load();
  ASSERT_TRUE(report.ok) << report.format();
  EXPECT_EQ(report.deactivated, 1u);
  EXPECT_EQ(problem_column(slug, "case_count"), "0");

  // 历史一条不少
  EXPECT_EQ(scalar("SELECT COUNT(*) FROM submissions s JOIN problems p ON p.id = s.problem_id"
                   " WHERE p.slug = ?",
                   {Value::from_text(slug)}),
            1u);
  EXPECT_EQ(scalar("SELECT COUNT(*) FROM submission_case_results r JOIN submissions s"
                   " ON s.id = r.submission_id WHERE s.problem_id = ?",
                   {Value::from_i64(std::atoll(problem_id.c_str()))}),
            1u);
  EXPECT_EQ(column_of("submissions", "verdict", "problem_id = " + problem_id), "AC");
}

TEST_F(LoaderTest, DatabaseFailureRollsBackThatProblemAndFailsTheReport) {
  // 用触发器制造一条真实的写库失败（校验都过了，是 DB 侧拒绝）。三点要验：
  // ① 该题【整条回滚】——INSERT INTO problems 与 INSERT 案例行同事务，失败后不能留下
  //    一行 case_count=0 的孤儿题（那会让它出现在列表页却判不了题）；
  // ② 报告 ok=false、db_failed=1 ——main 据此拒绝启动（地基不一致时继续跑更危险）；
  // ③ 其他题照常写入 ——一题一个事务，不是全库一个事务。
  Lease lease = Pool::instance().acquire();
  ASSERT_TRUE(lease.valid());
  const bool trigger_created =
      lease->exec("CREATE TRIGGER oj_selftest_block_cases BEFORE INSERT ON problem_cases "
                  "FOR EACH ROW SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'oj-selftest: blocked'");
  if (!trigger_created) {
    // 本机 MySQL 开了 binlog，而 oj 用户没有 SUPER：ERROR 1419 要求 SUPER 或
    // log_bin_trust_function_creators=1。我们【不去改 mysqld 配置】来迁就一条测试 ——
    // 那是对这台机器的副作用，比这条用例本身更有风险。因此跳过并把原因写清楚。
    GTEST_SKIP() << "无法创建触发器（需要 SUPER 或 log_bin_trust_function_creators=1）："
                 << lease->error().format()
                 << "。本用例验证的是【写库失败时的回滚与报告语义】；"
                    "若要跑它，请用有 SUPER 的账号或临时打开该变量。";
  }

  const std::string blocked = std::string(kSlugPrefix) + "blocked";
  const std::string healthy = std::string(kSlugPrefix) + "healthy";
  write_file(blocked + ".json", problem_json(blocked, "blocked", 2));
  write_file(healthy + ".json", problem_json(healthy, "healthy", 1));

  const LoadReport report = load();
  EXPECT_FALSE(report.ok);
  EXPECT_EQ(report.db_failed, 1u);
  EXPECT_EQ(report.loaded, 1u);
  ASSERT_FALSE(report.problems.empty());
  EXPECT_NE(report.problems[0].find(blocked), std::string::npos) << report.problems[0];
  // ① 回滚干净：主表那行也不该留下
  EXPECT_EQ(problem_column(blocked, "id"), "<missing>");
  EXPECT_EQ(scalar("SELECT COUNT(*) FROM problem_cases c JOIN problems p ON p.id = c.problem_id"
                   " WHERE p.slug = ?",
                   {Value::from_text(blocked)}),
            0u);
  // ③ 另一道题不受影响
  EXPECT_NE(problem_column(healthy, "id"), "<missing>");
  // 失败的那题不进缓存（缓存里只应该有"库里有行"的题）
  EXPECT_EQ(oj::problem::find_by_slug(blocked), nullptr);
  EXPECT_NE(oj::problem::find_by_slug(healthy), nullptr);

  lease->exec("DROP TRIGGER IF EXISTS oj_selftest_block_cases");
}

// 这条【不能】用 LoaderTest：它需要在"池没就绪"的状态下调 load_all，而 fixture 每次
// 都会把池 init 好。写成独立用例，靠顺序无关的 shutdown() 制造那个状态。
TEST(LoaderWithoutPool, RefusesToWriteAndReportsWhy) {
  oj::problem::clear_cache();
  oj::db::Pool::instance().shutdown();   // 人为制造"池未就绪"

  LoadOptions options;
  // 用 "." 而不是 "./problems"：本用例要验的是"池没就绪时拒绝写库并说明原因"，
  // 而加载器的顺序是【先读目录、再碰 DB】。若这里写一个依赖当前工作目录的路径
  // （如 ./problems），在 ctest 的工作目录（build/tests）下会先以"目录读不到"失败，
  // 报出来的是另一条原因 —— 这个用例就测不到它想测的东西了。
  // "." 恒存在（任何工作目录下都是），目录内容无关紧要。
  options.dir = ".";
  const LoadReport report = oj::problem::load_all(options);
  EXPECT_FALSE(report.ok);
  ASSERT_FALSE(report.problems.empty());
  EXPECT_NE(report.problems[0].find("连接池未就绪"), std::string::npos) << report.problems[0];
  EXPECT_EQ(report.loaded, 0u);
  EXPECT_EQ(oj::problem::cache_size(), 0u) << "失败时缓存必须是空的，而不是上一轮的旧内容";
}

}  // namespace
