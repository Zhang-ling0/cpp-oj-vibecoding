// =============================================================================
// config 模块单元测试（gtest 1.14）
// 被测：include/common/config.h + src/common/config.cpp（SPEC.md §9 M0 T0.4）
// 运行：tests/run_unit_tests.sh
//
// 【测什么】
//   A. 三层来源与优先级（内置默认 / .env / 进程环境）
//   B. .env 语法解析（行内注释、引号、export、重复键、CRLF、坏行）
//   C. 类型与范围回落（非法整数、越端口、布尔写法）
//   D. D-37 角色名单语义 —— 重点是「未设置」与「显式空串」必须可区分（AC-18 前置）
//   E. 量化链硬校验（闸门 = 2×workers、兜底墙钟不等式，SPEC §2.3 / §5.4.2.1）
//   F. 失败不发布配置、诊断与启动回显
//
// 【不测】角色同步 SQL、注册端点 1010 —— 分属 T3.11 / T3.3，需要 DB，不在本文件范围。
//
// 【为什么每个用例都要清环境】被测模块是【进程级单例】：启动时加载一次，之后只读。
//   若不清理，CI 上只要 export 过 OJ_JUDGE_GATE=8，半数用例会以与代码无关的原因变红。
// =============================================================================
#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "common/config.h"

// 进程环境表。与 config.cpp 一样必须声明在【全局作用域】——放进匿名 namespace
// 会变成另一个符号，链接时报 undefined reference
extern char** environ;

namespace {

using namespace oj::common;

// 配置键清单（键 + 一个【合法且自洽】的取值）。两个用途：
//   ① ConfigKeySurfaceMatchesImplementation 用它做「文档/测试与代码漂移」检测
//   ② 让该用例顺带成为一份"全键齐备的合法 .env"的冒烟样本
// ⚠️ 它【不是】环境清理的依据 —— 清理按前缀扫，见 ConfigTest::ClearProjectEnv。
struct KeyValue {
  const char* key;
  const char* value;
};

constexpr KeyValue kConfigKeys[] = {
    {"MYSQL_HOST", "db.example"},
    {"MYSQL_PORT", "3306"},
    {"MYSQL_USER", "oj"},
    {"MYSQL_PASSWORD", "pw"},
    {"MYSQL_DB", "oj"},
    {"OJ_LISTEN", "127.0.0.1:8080"},
    {"OJ_HTTP_THREADS", "8"},
    {"OJ_JUDGE_WORKERS", "2"},
    {"OJ_JUDGE_GATE", "4"},
    {"OJ_JUDGE_USER_GATE", "2"},
    {"OJ_FENCE_MULTIPLIER", "10"},
    {"OJ_JUDGE_WORKDIR", "/tmp/judge"},
    {"OJ_PROBLEMS_DIR", "./problems"},
    {"OJ_STATIC_DIR", "./static"},
    {"OJ_LOG_DIR", "./logs"},
    {"OJ_LOG_RETENTION_DAYS", "7"},
    {"OJ_SESSION_TTL_DAYS", "7"},
    {"OJ_MAX_CODE_BYTES", "65536"},
    {"OJ_RATE_WINDOW_SECONDS", "60"},
    {"OJ_RATE_MAX_SUBMITS", "10"},
    {"OJ_ALLOW_REGISTER", "1"},
    {"OJ_ADMIN_USERNAMES", "alice,carol"},  // 闸门 4 = 2 × workers 2，三者互不冲突
};

// ---- RAII 辅助 ---------------------------------------------------------------

// 作用域内设置/清除一个进程环境变量，析构时恢复原状（避免用例之间互相污染）
class ScopedEnv {
 public:
  ScopedEnv(std::string key, const char* value) : key_(std::move(key)) {
    if (const char* old = std::getenv(key_.c_str())) {
      had_old_ = true;
      old_ = old;
    }
    if (value != nullptr) {
      ::setenv(key_.c_str(), value, 1);
    } else {
      ::unsetenv(key_.c_str());
    }
  }
  ~ScopedEnv() {
    if (had_old_) {
      ::setenv(key_.c_str(), old_.c_str(), 1);
    } else {
      ::unsetenv(key_.c_str());
    }
  }
  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

 private:
  std::string key_;
  std::string old_;
  bool had_old_ = false;
};

// 临时 .env 文件：构造时写入，析构时删除
class TempEnvFile {
 public:
  TempEnvFile(std::string content, std::string tag) {
    static int seq = 0;
    path_ = "/tmp/oj-config-test-" + std::to_string(::getpid()) + "-" +
            std::to_string(++seq) + "-" + tag + ".env";
    std::ofstream out(path_);
    out << content;
  }
  ~TempEnvFile() { std::remove(path_.c_str()); }
  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

// 固定一个必然不存在的路径，用于「无 .env」场景
std::string MissingPath() {
  return "/tmp/oj-config-test-missing-" + std::to_string(::getpid()) + ".env";
}

const char* LevelTag(DiagLevel l) {
  switch (l) {
    case DiagLevel::kInfo:
      return "INFO";
    case DiagLevel::kWarn:
      return "WARN";
    case DiagLevel::kError:
      return "ERROR";
  }
  return "??";
}

std::size_t CountDiag(const LoadResult& r, DiagLevel level) {
  std::size_t n = 0;
  for (const auto& d : r.diagnostics) {
    if (d.level == level) ++n;
  }
  return n;
}

// 断言「存在某级别的诊断且其文本含 needle」；失败时把全部诊断打出来便于定位
::testing::AssertionResult HasDiag(const LoadResult& r, DiagLevel level,
                                   const std::string& needle) {
  for (const auto& d : r.diagnostics) {
    if (d.level == level && d.message.find(needle) != std::string::npos) {
      return ::testing::AssertionSuccess();
    }
  }
  return ::testing::AssertionFailure()
         << "诊断里找不到 [" << LevelTag(level) << "] 含 \"" << needle
         << "\" 的条目。实际诊断：\n" << format_diagnostics(r);
}

// ---- 测试夹具 ----------------------------------------------------------------

class ConfigTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // 被测模块是进程级单例，有【三处】可变全局态：g_config / g_loaded / g_kv。
    // reset_config() 三处一起复位，缺一不可（漏 g_loaded 会让 config_loaded() 断言
    // 读到上一个用例的值，漏 g_kv 会让 lookup_raw() 读到上一个用例的键）。
    reset_config();
    ClearProjectEnv();
  }
  void TearDown() override { reset_config(); }

  // 按【前缀】扫掉本项目命名空间，而不是只清 kConfigKeys 那份列表。两个原因：
  //   ① 只清列表的话，将来 config.cpp 新增一个键而测试没同步列表，shell 里同名变量
  //      就会渗进用例；
  //   ② 更现实：开发者 shell / CI 里任何一个无关的 OJ_* 变量都会被配置模块吸进
  //      配置表并产生一条「未知配置项」告警，让断言告警条数的用例随机变红
  //      （实测：OJ_SOMEONE_ELSE_VAR=1 会让 2 个用例失败）。
  // 因此 config.h 里也把「所有配置键必须带 OJ_ / MYSQL_ 前缀」写成了不变量。
  static void ClearProjectEnv() {
    std::vector<std::string> victims;  // 先收集再 unset：unsetenv 会压缩环境数组，边遍历边删会漏项
    for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
      const std::string entry(*e);
      if (entry.rfind("OJ_", 0) == 0 || entry.rfind("MYSQL_", 0) == 0) {
        victims.push_back(entry.substr(0, entry.find('=')));
      }
    }
for (const std::string& key : victims) ::unsetenv(key.c_str());
    // 兜底：万一将来引入一个不带前缀的键，至少清单里的那些仍会被清掉
    for (const KeyValue& kv : kConfigKeys) ::unsetenv(kv.key);
  }

  static LoadResult Load(const TempEnvFile& file) {
    return load_config(file.path());
  }
};

// =============================================================================
// A. 三层来源与优先级
// =============================================================================

TEST_F(ConfigTest, EnvFileMissing_KeepsDefaultsAndWarns) {
  const LoadResult r = load_config(MissingPath());

  EXPECT_TRUE(r.ok);
  EXPECT_FALSE(r.env_file_loaded);
  EXPECT_TRUE(HasDiag(r, DiagLevel::kWarn, "未找到配置文件"));

  // 缺 .env 不是致命错误：全部落回实测过的内置默认值
  const Config& c = config();
  EXPECT_EQ(c.mysql_host, "127.0.0.1");
  EXPECT_EQ(c.mysql_port, 3306);
  EXPECT_EQ(c.listen_port, 8080);
  EXPECT_EQ(c.judge_workers, 2);
  EXPECT_EQ(c.judge_global_gate, 4);
  EXPECT_EQ(c.fence_multiplier, 10);
  EXPECT_TRUE(config_loaded());
}

TEST_F(ConfigTest, ValuesComeFromEnvFile) {
  TempEnvFile f("MYSQL_HOST=10.0.0.5\nMYSQL_PORT=3307\nOJ_SESSION_TTL_DAYS=14\n",
                "values");
  const LoadResult r = Load(f);

  EXPECT_TRUE(r.ok);
  EXPECT_TRUE(r.env_file_loaded);
  EXPECT_EQ(config().mysql_host, "10.0.0.5");
  EXPECT_EQ(config().mysql_port, 3307);
  EXPECT_EQ(config().session_ttl_days, 14);
}

TEST_F(ConfigTest, ProcessEnvOverridesEnvFile) {
  TempEnvFile f("OJ_JUDGE_WORKERS=2\nOJ_JUDGE_GATE=4\n", "override");
  const ScopedEnv workers("OJ_JUDGE_WORKERS", "3");
  const ScopedEnv gate("OJ_JUDGE_GATE", "6");

  const LoadResult r = Load(f);

  EXPECT_TRUE(r.ok);
  EXPECT_EQ(config().judge_workers, 3);
  EXPECT_EQ(config().judge_global_gate, 6);
  // 覆盖是正常现象（路线 B 就是靠它），记 INFO 而不是 WARN
  EXPECT_TRUE(HasDiag(r, DiagLevel::kInfo, "进程环境变量覆盖"));
}

TEST_F(ConfigTest, EnvOnlyDeploymentWorksWithoutEnvFile) {
  const ScopedEnv host("MYSQL_HOST", "db");
  const ScopedEnv listen("OJ_LISTEN", "0.0.0.0:9999");

  const LoadResult r = load_config(MissingPath());

  EXPECT_TRUE(r.ok);
  EXPECT_EQ(config().mysql_host, "db");
  EXPECT_EQ(config().listen_port, 9999);
}

TEST_F(ConfigTest, UnrelatedSystemEnvVarIsNotTreatedAsConfig) {
  // 镜像/宿主机里普遍存在 MYSQL_HOME 之类变量，它不是本项目的配置，不该产生告警
  const ScopedEnv home("MYSQL_HOME", "/usr");

  const LoadResult r = load_config(MissingPath());

  EXPECT_TRUE(r.ok);
  EXPECT_EQ(CountDiag(r, DiagLevel::kWarn), 1U) << format_diagnostics(r);
  EXPECT_EQ(HasDiag(r, DiagLevel::kWarn, "未找到配置文件") ? 1 : 0, 1);
}

TEST_F(ConfigTest, EnvFileKeysAreReportedSortedWithoutDuplicates) {
  TempEnvFile f("MYSQL_PORT=3306\nMYSQL_HOST=h\nMYSQL_PORT=3307\n", "keys");
  const LoadResult r = Load(f);

  ASSERT_EQ(r.env_file_keys.size(), 2U);
  EXPECT_EQ(r.env_file_keys[0], "MYSQL_HOST");
  EXPECT_EQ(r.env_file_keys[1], "MYSQL_PORT");
}

// =============================================================================
// B. .env 语法解析
// =============================================================================

TEST_F(ConfigTest, CommentsAndBlankLinesAreIgnored) {
  TempEnvFile f("# 顶部注释\n"
                "\n"
                "   \n"
                "MYSQL_HOST=1.2.3.4   # 行尾注释\n"
                "\t# 缩进后的注释\n"
                "MYSQL_DB=ojdb\n",
                "comments");
  const LoadResult r = Load(f);

  EXPECT_TRUE(r.ok);
  EXPECT_EQ(CountDiag(r, DiagLevel::kWarn), 0U) << format_diagnostics(r);
  EXPECT_EQ(config().mysql_host, "1.2.3.4");
  EXPECT_EQ(config().mysql_db, "ojdb");
}

TEST_F(ConfigTest, InlineCommentIsStrippedOnlyAfterWhitespace) {
  // SPEC §6.7.3 的 .env 里就有 `KEY=value   # 说明` 这种写法：值不能被注释吞掉
  TempEnvFile f("OJ_JUDGE_WORKERS=4        # 4 worker\n"
                "OJ_JUDGE_GATE=8\n"
                "MYSQL_PASSWORD=p#ss\n",
                "inline-comment");
  const LoadResult r = Load(f);

  EXPECT_TRUE(r.ok) << format_diagnostics(r);
  EXPECT_EQ(config().judge_workers, 4);
  EXPECT_EQ(config().judge_global_gate, 8);
  // 而「空白之前的 #」是值的一部分，必须保留
  EXPECT_EQ(config().mysql_password, "p#ss");
}

TEST_F(ConfigTest, QuotedValueKeepsSpacesAndHash) {
  TempEnvFile f("MYSQL_PASSWORD=\"  p@ss#word  \"\nMYSQL_USER='a b'\n", "quoted");
  const LoadResult r = Load(f);

  EXPECT_TRUE(r.ok) << format_diagnostics(r);
  EXPECT_EQ(config().mysql_password, "  p@ss#word  ");
  EXPECT_EQ(config().mysql_user, "a b");
}

TEST_F(ConfigTest, ExportPrefixIsAccepted) {
  TempEnvFile f("export MYSQL_HOST=exported.example\n", "export");
  const LoadResult r = Load(f);

  EXPECT_TRUE(r.ok) << format_diagnostics(r);
  EXPECT_EQ(config().mysql_host, "exported.example");
}

TEST_F(ConfigTest, DuplicateKeyLastWinsWithWarning) {
  TempEnvFile f("MYSQL_HOST=first\nMYSQL_HOST=second\n", "dup");
  const LoadResult r = Load(f);

  EXPECT_TRUE(r.ok);
  EXPECT_EQ(config().mysql_host, "second");
  EXPECT_TRUE(HasDiag(r, DiagLevel::kWarn, "重复定义"));
}

TEST_F(ConfigTest, WindowsLineEndingsAreTolerated) {
  TempEnvFile f("MYSQL_HOST=crlf.example\r\nOJ_JUDGE_WORKERS=2\r\nOJ_JUDGE_GATE=4\r\n",
                "crlf");
  const LoadResult r = Load(f);

  EXPECT_TRUE(r.ok) << format_diagnostics(r);
  EXPECT_EQ(config().mysql_host, "crlf.example");
  EXPECT_EQ(config().judge_workers, 2);
}

TEST_F(ConfigTest, LineWithoutEqualsIsSkippedWithWarning) {
  TempEnvFile f("MYSQL_HOST=ok.example\nthis line has no equals\n", "no-equals");
  const LoadResult r = Load(f);

  EXPECT_TRUE(r.ok);
  EXPECT_EQ(config().mysql_host, "ok.example");
  EXPECT_TRUE(HasDiag(r, DiagLevel::kWarn, "缺少 '='"));
}

TEST_F(ConfigTest, UnterminatedQuoteIsReportedAndFallsBackToRawText) {
  TempEnvFile f("MYSQL_PASSWORD=\"unclosed\n", "bad-quote");
  const LoadResult r = Load(f);

  EXPECT_TRUE(r.ok) << format_diagnostics(r);
  EXPECT_TRUE(HasDiag(r, DiagLevel::kWarn, "值格式异常"));
}

TEST_F(ConfigTest, UnknownKeyIsNamedAfterBeingIgnored) {
  // 拼错的键若被静默忽略，D-37 的角色名单就等于没配，且没有任何线索
  TempEnvFile f("OJ_ADMIN_USERNAME=bob\n", "unknown");
  const LoadResult r = Load(f);

  EXPECT_TRUE(r.ok);
  EXPECT_TRUE(HasDiag(r, DiagLevel::kWarn, "OJ_ADMIN_USERNAME"));
  EXPECT_TRUE(HasDiag(r, DiagLevel::kWarn, "不会被读取"));
}

TEST_F(ConfigTest, ConfigKeySurfaceMatchesImplementation) {
  // 「全键齐备的合法 .env」冒烟 + 有限的漂移检测。
  // ⚠️ 老实说清它【抓不到什么】，免得后来人误以为覆盖了全部漂移：
  //   · 能抓到：清单里的键在实现里被删除或改名（且 `known_keys()` 未同步）→ 报未知键；
  //             某个键的合法取值被实现拒绝（取值范围/校验规则变了）。
  //   · 抓不到：实现把 `OJ_LISTEN` 改成 `OJ_LISTEN_ADDR` 而 `known_keys()` 忘了同步 ——
  //             这时该键变成「认识但没人读」，不产生任何告警，此处照样绿。
  //             实测（把代码里的键改名后编译本文件）：抓它的是 `ListenParses…` 与
  //             `ListenWithBadPort…` 两条用例，不是本条。**每个键的取值行为由它自己的
  //             用例守着**，本条只负责"没有键名层面的错位"。
  std::string content;
  for (const KeyValue& kv : kConfigKeys) {
    content += std::string(kv.key) + "=" + kv.value + "\n";
  }

  TempEnvFile f(content, "key-surface");
  const LoadResult r = Load(f);

  ASSERT_TRUE(r.ok) << format_diagnostics(r);
  EXPECT_EQ(CountDiag(r, DiagLevel::kWarn), 0U)
      << "键清单与实现不一致（删除/改名/拼错），或某个键的取值被拒：\n"
      << format_diagnostics(r);
  EXPECT_EQ(r.env_file_keys.size(), std::size(kConfigKeys));
  EXPECT_EQ(defined_keys().size(), std::size(kConfigKeys));
}

// =============================================================================
// C. 类型与范围
// =============================================================================

TEST_F(ConfigTest, NonIntegerValueFallsBackToDefaultWithWarning) {
  TempEnvFile f("OJ_JUDGE_WORKERS=abc\n", "non-int");
  const LoadResult r = Load(f);

  EXPECT_TRUE(r.ok);
  EXPECT_EQ(config().judge_workers, 2);
  EXPECT_TRUE(HasDiag(r, DiagLevel::kWarn, "不是整数"));
}

TEST_F(ConfigTest, OutOfRangeValueFallsBackToDefaultWithWarning) {
  TempEnvFile f("MYSQL_PORT=99999\nOJ_SESSION_TTL_DAYS=0\n", "range");
  const LoadResult r = Load(f);

  EXPECT_TRUE(r.ok);
  EXPECT_EQ(config().mysql_port, 3306);
  EXPECT_EQ(config().session_ttl_days, 7);
  EXPECT_TRUE(HasDiag(r, DiagLevel::kWarn, "超出允许范围"));
}

TEST_F(ConfigTest, RangeBoundariesAreInclusive) {
  TempEnvFile f("MYSQL_PORT=1\nOJ_LOG_RETENTION_DAYS=3650\n", "bounds");
  const LoadResult r = Load(f);

  EXPECT_TRUE(r.ok) << format_diagnostics(r);
  EXPECT_EQ(config().mysql_port, 1);
  EXPECT_EQ(config().log_retention_days, 3650);
}

TEST_F(ConfigTest, BoolAcceptsCommonSpellings) {
  for (const char* yes : {"1", "true", "TRUE", "yes", "on", "开"}) {
    SCOPED_TRACE(yes);
    reset_config();
    TempEnvFile f(std::string("OJ_ALLOW_REGISTER=") + yes, "bool-yes");
    EXPECT_TRUE(Load(f).ok);
    EXPECT_TRUE(config().allow_register) << yes;
  }
  for (const char* no : {"0", "false", "FALSE", "no", "off", "关"}) {
    SCOPED_TRACE(no);
    reset_config();
    TempEnvFile f(std::string("OJ_ALLOW_REGISTER=") + no, "bool-no");
    EXPECT_TRUE(Load(f).ok);
    EXPECT_FALSE(config().allow_register) << no;
  }
}

TEST_F(ConfigTest, InvalidBoolFallsBackToDefaultWithWarning) {
  TempEnvFile f("OJ_ALLOW_REGISTER=maybe\n", "bool-bad");
  const LoadResult r = Load(f);

  EXPECT_TRUE(r.ok);
  EXPECT_TRUE(config().allow_register);  // 默认开放
  EXPECT_TRUE(HasDiag(r, DiagLevel::kWarn, "不是布尔值"));
}

TEST_F(ConfigTest, ListenParsesHostPortBarePortAndIPv6) {
  struct Case {
    const char* value;
    const char* host;
    int port;
  };
  const Case cases[] = {
      {"0.0.0.0:8080", "0.0.0.0", 8080},
      {"127.0.0.1:9000", "127.0.0.1", 9000},
      {"9100", "0.0.0.0", 9100},          // 只给端口时保留默认 host
      {"[::]:9200", "::", 9200},
      {"example.local:1", "example.local", 1},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.value);
    reset_config();
    TempEnvFile f(std::string("OJ_LISTEN=") + c.value, "listen");
    EXPECT_TRUE(Load(f).ok);
    EXPECT_EQ(config().listen_port, c.port) << c.value;
    EXPECT_EQ(config().listen_host, c.host) << c.value;
  }
}

TEST_F(ConfigTest, ListenWithBadPortFallsBackWithWarning) {
  TempEnvFile f("OJ_LISTEN=0.0.0.0:not-a-port\n", "listen-bad");
  const LoadResult r = Load(f);

  EXPECT_TRUE(r.ok);
  EXPECT_EQ(config().listen_port, 8080);
  EXPECT_TRUE(HasDiag(r, DiagLevel::kWarn, "非法"));
}

TEST_F(ConfigTest, QuotaAndPathKeysAreParsed) {
  TempEnvFile f("OJ_RATE_WINDOW_SECONDS=30\n"
                "OJ_RATE_MAX_SUBMITS=5\n"
                "OJ_MAX_CODE_BYTES=2048\n"
                "OJ_JUDGE_WORKDIR=/tmp/custom-judge\n"
                "OJ_PROBLEMS_DIR=/srv/problems\n"
                "OJ_HTTP_THREADS=16\n",
                "misc");
  const LoadResult r = Load(f);

  EXPECT_TRUE(r.ok) << format_diagnostics(r);
  EXPECT_EQ(config().rate_window_seconds, 30);
  EXPECT_EQ(config().rate_max_submits_per_window, 5);
  EXPECT_EQ(config().max_code_bytes, 2048U);
  EXPECT_EQ(config().judge_workdir, "/tmp/custom-judge");
  EXPECT_EQ(config().problems_dir, "/srv/problems");
  EXPECT_EQ(config().http_threads, 16);
}

// =============================================================================
// D. D-37 角色名单：未设置 vs 显式空串（AC-18 的前置，漏了就是静默全员降权）
// =============================================================================

TEST_F(ConfigTest, AdminUnsetMeansNotConfigured) {
  TempEnvFile f("MYSQL_HOST=h\n", "admin-unset");
  const LoadResult r = Load(f);

  EXPECT_TRUE(r.ok);
  EXPECT_FALSE(config().admin_usernames_configured);
  EXPECT_TRUE(config().admin_usernames.empty());
  EXPECT_EQ(CountDiag(r, DiagLevel::kWarn), 0U) << format_diagnostics(r);
}

TEST_F(ConfigTest, AdminExplicitEmptyMeansConfiguredButEmpty) {
  TempEnvFile f("OJ_ADMIN_USERNAMES=\n", "admin-empty");
  const LoadResult r = Load(f);

  EXPECT_TRUE(r.ok);
  EXPECT_TRUE(config().admin_usernames_configured) << "空串必须算「已配置」";
  EXPECT_TRUE(config().admin_usernames.empty());
  // 降权是不可见操作，必须在启动日志里留下痕迹（§6.3.1 约束 4）
  EXPECT_TRUE(HasDiag(r, DiagLevel::kWarn, "降为普通用户"));
}

TEST_F(ConfigTest, AdminBlankValueAlsoCountsAsExplicitEmpty) {
  TempEnvFile f("OJ_ADMIN_USERNAMES=    \n", "admin-blank");
  const LoadResult r = Load(f);

  EXPECT_TRUE(r.ok);
  EXPECT_TRUE(config().admin_usernames_configured);
  EXPECT_TRUE(config().admin_usernames.empty());
}

TEST_F(ConfigTest, AdminListIsTrimmedDeduplicatedAndOrdered) {
  TempEnvFile f("OJ_ADMIN_USERNAMES= alice , carol ,alice,,  \n", "admin-list");
  const LoadResult r = Load(f);

  EXPECT_TRUE(r.ok);
  EXPECT_TRUE(config().admin_usernames_configured);
  ASSERT_EQ(config().admin_usernames.size(), 2U);
  EXPECT_EQ(config().admin_usernames[0], "alice");
  EXPECT_EQ(config().admin_usernames[1], "carol");
  EXPECT_TRUE(HasDiag(r, DiagLevel::kWarn, "重复出现"));
}

TEST_F(ConfigTest, AdminNameWithImplausibleLengthIsWarned) {
  TempEnvFile f("OJ_ADMIN_USERNAMES=ab,alicedehavyveryveryverylongusernamethatexceeds32\n",
                "admin-len");
  const LoadResult r = Load(f);

  EXPECT_TRUE(r.ok) << format_diagnostics(r);
  EXPECT_EQ(config().admin_usernames.size(), 2U);  // 仍然保留，只是提示匹配不上
  EXPECT_TRUE(HasDiag(r, DiagLevel::kWarn, "3–32"));
}

TEST_F(ConfigTest, AdminListFromProcessEnvIsAlsoHonored) {
  const ScopedEnv admins("OJ_ADMIN_USERNAMES", "alice");
  const LoadResult r = load_config(MissingPath());

  EXPECT_TRUE(r.ok);
  EXPECT_TRUE(config().admin_usernames_configured);
  ASSERT_EQ(config().admin_usernames.size(), 1U);
  EXPECT_EQ(config().admin_usernames[0], "alice");
}

// =============================================================================
// E. 量化链硬校验（SPEC §2.3 / §5.4.2.1）
// =============================================================================

TEST_F(ConfigTest, GateMustEqualTwiceWorkers) {
  TempEnvFile f("OJ_JUDGE_WORKERS=4\nOJ_JUDGE_GATE=4\n", "gate-eq");
  const LoadResult r = Load(f);

  EXPECT_FALSE(r.ok);
  EXPECT_TRUE(HasDiag(r, DiagLevel::kError, "2 × OJ_JUDGE_WORKERS"));
  EXPECT_EQ(r.error_count(), 1U);
}

TEST_F(ConfigTest, GateTwiceWorkersPasses) {
  for (const auto& pair : {std::make_pair(2, 4), std::make_pair(4, 8),
                           std::make_pair(5, 10)}) {
    SCOPED_TRACE(std::to_string(pair.first) + "×2");
    reset_config();
    TempEnvFile f("OJ_JUDGE_WORKERS=" + std::to_string(pair.first) +
                      "\nOJ_JUDGE_GATE=" + std::to_string(pair.second) + "\n",
                  "gate-ok");
    const LoadResult r = Load(f);
    EXPECT_TRUE(r.ok) << format_diagnostics(r);
    EXPECT_EQ(config().judge_global_gate, 2 * config().judge_workers);
  }
}

TEST_F(ConfigTest, UserGateCannotExceedGlobalGate) {
  TempEnvFile f("OJ_JUDGE_WORKERS=2\nOJ_JUDGE_GATE=4\nOJ_JUDGE_USER_GATE=8\n",
                "user-gate");
  const LoadResult r = Load(f);

  EXPECT_FALSE(r.ok);
  EXPECT_TRUE(HasDiag(r, DiagLevel::kError, "OJ_JUDGE_USER_GATE"));
}

TEST_F(ConfigTest, FenceInequalityRejectsOversizedGate) {
  // §5.4.2.1 表里的「12 → 0.97× 开始误杀」行：闸门 12 + 系数 10 必须拒绝启动
  TempEnvFile f("OJ_JUDGE_WORKERS=6\nOJ_JUDGE_GATE=12\n", "fence-bad");
  const LoadResult r = Load(f);

  EXPECT_FALSE(r.ok);
  EXPECT_TRUE(HasDiag(r, DiagLevel::kError, "兜底墙钟与闸门矛盾"));
  // 错误信息要给出确切的目标值，而不是只说「不合法」
  EXPECT_TRUE(HasDiag(r, DiagLevel::kError, "≥ 11"));
  EXPECT_TRUE(HasDiag(r, DiagLevel::kError, "≤ 11"));
}

TEST_F(ConfigTest, FenceInequalityPassesWithLargerMultiplier) {
  TempEnvFile f("OJ_JUDGE_WORKERS=6\nOJ_JUDGE_GATE=12\nOJ_FENCE_MULTIPLIER=12\n",
                "fence-ok");
  const LoadResult r = Load(f);

  EXPECT_TRUE(r.ok) << format_diagnostics(r);
}

TEST_F(ConfigTest, FenceFormulaMatchesSpec) {
  TempEnvFile f("OJ_JUDGE_WORKERS=2\nOJ_JUDGE_GATE=4\nOJ_FENCE_MULTIPLIER=10\n",
                "formula");
  ASSERT_TRUE(Load(f).ok);

  // time_limit_ms × 系数 + 5000；闸门下最坏墙钟 = 闸门 × time_limit_ms
  EXPECT_EQ(config().fence_wall_ms(3000), 35000);
  EXPECT_EQ(config().fence_budget_ms(3000), 12000);
  EXPECT_GT(config().fence_wall_ms(3000), config().fence_budget_ms(3000));
}

TEST_F(ConfigTest, FailedLoadDoesNotPublishConfig) {
  TempEnvFile good("MYSQL_HOST=good.example\nOJ_JUDGE_WORKERS=2\nOJ_JUDGE_GATE=4\n",
                   "publish-good");
  TempEnvFile bad("MYSQL_HOST=bad.example\nOJ_JUDGE_WORKERS=2\nOJ_JUDGE_GATE=8\n",
                  "publish-bad");

  ASSERT_TRUE(Load(good).ok);
  ASSERT_EQ(config().mysql_host, "good.example");

  EXPECT_FALSE(Load(bad).ok);
  // 半份配置比报错更危险：必须原封不动保留上一次的有效配置
  EXPECT_EQ(config().mysql_host, "good.example");
  EXPECT_EQ(config().judge_global_gate, 4);
}

TEST_F(ConfigTest, FailedFirstLoadLeavesBuiltInDefaults) {
  TempEnvFile bad("OJ_JUDGE_GATE=99\n", "publish-first-bad");
  EXPECT_FALSE(Load(bad).ok);

  EXPECT_FALSE(config_loaded());
  EXPECT_EQ(config().judge_global_gate, 4);
  EXPECT_EQ(config().judge_workers, 2);
}

// =============================================================================
// F. 诊断与启动回显
// =============================================================================

TEST_F(ConfigTest, DumpConfigNeverPrintsThePassword) {
  TempEnvFile f("MYSQL_PASSWORD=s3cr3t-passphrase\n", "mask");
  ASSERT_TRUE(Load(f).ok);

  const std::string dump = dump_config();
  EXPECT_EQ(dump.find("s3cr3t"), std::string::npos) << dump;
  EXPECT_NE(dump.find("***"), std::string::npos) << dump;
}

TEST_F(ConfigTest, DumpConfigDistinguishesUnsetFromEmptyAdminList) {
  TempEnvFile unset("MYSQL_HOST=h\n", "dump-unset");
  ASSERT_TRUE(Load(unset).ok);
  const std::string unset_dump = dump_config();
  EXPECT_NE(unset_dump.find("变量未设置"), std::string::npos) << unset_dump;

  reset_config();
  TempEnvFile empty("OJ_ADMIN_USERNAMES=\n", "dump-empty");
  ASSERT_TRUE(Load(empty).ok);
  const std::string empty_dump = dump_config();
  EXPECT_NE(empty_dump.find("显式空串"), std::string::npos) << empty_dump;
}

TEST_F(ConfigTest, DumpConfigEchoesEffectiveNumbersAndMargin) {
  TempEnvFile f("OJ_JUDGE_WORKERS=2\nOJ_JUDGE_GATE=4\nOJ_FENCE_MULTIPLIER=10\n"
                "OJ_ADMIN_USERNAMES=alice,carol\n",
                "dump-echo");
  ASSERT_TRUE(Load(f).ok);

  const std::string dump = dump_config();
  EXPECT_NE(dump.find("workers=2"), std::string::npos) << dump;
  EXPECT_NE(dump.find("35000"), std::string::npos) << dump;
  EXPECT_NE(dump.find("2.9×"), std::string::npos) << dump;  // §5.4.2.1 的余量 2.9 倍
  EXPECT_NE(dump.find("alice, carol"), std::string::npos) << dump;
  EXPECT_NE(dump.find("降为 user"), std::string::npos) << dump;
}

TEST_F(ConfigTest, FormatDiagnosticsRendersSourceLevelsAndKeys) {
  TempEnvFile f("OJ_JUDGE_GATE=9\n", "format");
  const LoadResult r = Load(f);

  const std::string text = format_diagnostics(r);
  EXPECT_NE(text.find("ERROR"), std::string::npos) << text;
  EXPECT_NE(text.find("OJ_JUDGE_GATE"), std::string::npos) << text;
  EXPECT_NE(text.find("ERROR 1"), std::string::npos) << text;
}

TEST_F(ConfigTest, LookupRawAndDefinedKeysReflectLoadedState) {
  TempEnvFile f("MYSQL_HOST=lookup.example\nMYSQL_PORT=3308\n", "lookup");
  ASSERT_TRUE(Load(f).ok);

  const auto host = lookup_raw("MYSQL_HOST");
  ASSERT_TRUE(host.has_value());
  EXPECT_EQ(*host, "lookup.example");
  EXPECT_FALSE(lookup_raw("NOT_SET_ANYWHERE").has_value());

  const std::vector<std::string> keys = defined_keys();
  EXPECT_GE(keys.size(), 2U);
  // std::map 已按字典序
  EXPECT_EQ(keys.front(), "MYSQL_HOST");
  EXPECT_EQ(keys.at(1), "MYSQL_PORT");
}

TEST_F(ConfigTest, ConfigBeforeAnyLoadReturnsUsableDefaults) {
  reset_config();
  EXPECT_FALSE(config_loaded());
  EXPECT_EQ(config().judge_workers, 2);
  EXPECT_TRUE(config().allow_register);
  EXPECT_FALSE(config().admin_usernames_configured);
}

TEST_F(ConfigTest, ConcurrentReadsAreSafe) {
  // 配置加载后是只读的（§2.3：加载先于线程池启动），故读路径不需要加锁 ——
  // 但这条「不需要锁」的前提值得用并发读跑一遍钉住。
  TempEnvFile f("OJ_JUDGE_WORKERS=2\nOJ_JUDGE_GATE=4\n", "threads");
  ASSERT_TRUE(Load(f).ok);

  std::vector<std::thread> workers;
  workers.reserve(8);
  for (int i = 0; i < 8; ++i) {
    workers.emplace_back([] {
      for (int k = 0; k < 1000; ++k) {
        EXPECT_EQ(config().judge_workers, 2);
        EXPECT_EQ(config().fence_wall_ms(3000), 35000);
      }
    });
  }
  for (auto& t : workers) t.join();
}

}  // namespace