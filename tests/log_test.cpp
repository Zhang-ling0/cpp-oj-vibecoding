// =============================================================================
// log 模块单元测试（gtest 1.14）
// 被测：include/common/log.h + src/common/log.cpp（SPEC.md §9 M0 T0.4 / M2 T2.10）
// 运行：tests/run_unit_tests.sh（ctest 会把每个 TEST 展开成一条用例）
//
// 【测什么】
//   A. 初始化：目录自建、双文件同时创建、报告内容、无参版读 config().log_dir
//   B. 行不变量：每行带前缀、单行 API 转义 '\n'、UTF-8 原样透传
//   C. 双通道隔离 [D-34] + 等级阈值（app=INFO / judge=DEBUG，NF-5 的前提）
//   D. 多行：log_text 逐行加前缀、log_block 缩进 + 边界标记（T2.10 生成代码）
//   E. 结构化字段 log_kv（含空格的取值加引号）
//   F. 按天切割：惰性触发、两个通道各自独立、同名目标不覆盖（追加 .1）
//   G. 保留 N 天：只删本模块形状的文件、边界日保留、非日志文件不动
//   H. 并发：8 线程 × N 条，条数与行数一致（§2.3 的 11 个写者里最忙的那批）
//   I. 失败与降级：未初始化不建文件、写失败不崩、shutdown 幂等、超长截断
//
// 【怎么测"按天"】靠 log_test_set_time() 覆盖时钟（与 config::reset_config() 同一性质）。
//   真的等一天不现实，而切割逻辑里唯一与时间有关的输入就是"当前本地日期"。
//   所有断言里的日期都由 epoch → 本地日期换算得到，因此本用例在任意 TZ 下都成立。
//
// 【不测】Docker volume 路径（路线 B）、与 judge 的联动（T2.10 由 worker 调用，
//   属集成范围）、日志内容的中文可读性（由 §4.3 的人工流程验收）。
// =============================================================================
#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "common/config.h"
#include "common/log.h"

namespace {

using namespace oj::common;
namespace fs = std::filesystem;

// 固定的"今天"，避免断言里出现随运行时刻漂移的字符串
constexpr int kYear = 2026;
constexpr int kMonth = 10;
constexpr int kDay = 11;

// 进程本地时区下的 epoch 秒（不用 UTC 假设：日志按本地日期切割）
std::int64_t local_epoch(int year, int month, int day, int hour = 12, int minute = 0) {
  std::tm tm_value{};
  tm_value.tm_year = year - 1900;
  tm_value.tm_mon = month - 1;
  tm_value.tm_mday = day;
  tm_value.tm_hour = hour;
  tm_value.tm_min = minute;
  tm_value.tm_sec = 0;
  tm_value.tm_isdst = -1;
  return static_cast<std::int64_t>(::mktime(&tm_value));
}

std::int64_t base_day() { return local_epoch(kYear, kMonth, kDay); }

// 以"日"为单位偏移。用 mktime 归一化而不是简单加减 86400 秒 ——
// 夏令时切换日那一天不等于 24 小时，直接减秒数在有 DST 的时区会偏一天。
std::int64_t day_offset(int days) {
  const std::time_t raw = static_cast<std::time_t>(base_day());
  std::tm tm_value{};
  ::localtime_r(&raw, &tm_value);
  tm_value.tm_mday += days;
  tm_value.tm_isdst = -1;
  return static_cast<std::int64_t>(::mktime(&tm_value));
}

std::string date_of(std::int64_t epoch) {
  const std::time_t raw = static_cast<std::time_t>(epoch);
  std::tm tm_value{};
  ::localtime_r(&raw, &tm_value);
  char buf[16];
  std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm_value);
  return buf;
}

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::ostringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

std::vector<std::string> read_lines(const std::string& path) {
  std::vector<std::string> lines;
  const std::string content = read_file(path);
  std::string current;
  for (const char c : content) {
    if (c == '\n') {
      lines.push_back(current);
      current.clear();
    } else if (c != '\r') {
      current += c;
    }
  }
  if (!current.empty()) lines.push_back(current);  // 末尾无换行也算一行
  return lines;
}

bool starts_with(const std::string& s, const std::string& prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// 取一行的正文部分（剥掉 `<ts> <等级> [tid=N] [<通道>] ` 前缀）。
// 前缀里等级是定宽、通道名长短不等，所以按 "[<tag>] " 这个标记切，
// 不能写死偏移 —— 写死过，写错了三个用例一起红。
std::string body_of(const std::string& line, const char* tag) {
  const std::string marker = std::string("[") + tag + "] ";
  const std::size_t pos = line.find(marker);
  if (pos == std::string::npos) return "<前缀缺失>";
  return line.substr(pos + marker.size());
}

class LogTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char tmpl[] = "/tmp/oj-log-test-XXXXXX";
    const char* made = ::mkdtemp(tmpl);
    ASSERT_NE(made, nullptr) << "无法创建临时目录";
    dir_ = made;
    log_test_reset();
    log_test_set_time(base_day());
  }

  void TearDown() override {
    log_test_reset();
    std::error_code ec;
    fs::remove_all(dir_, ec);
    log_test_set_time(0);
  }

  // 默认档：保留 7 天，judge 全开
  LogInitReport init_default(int retention_days = 7) {
    LogOptions options;
    options.dir = dir_;
    options.retention_days = retention_days;
    return init_logging(options);
  }

  std::string app_path() const { return dir_ + "/app.log"; }
  std::string judge_path() const { return dir_ + "/judge.log"; }

  // 造一个"看起来像切割产物"的历史文件，用于验证保留期清理
  void touch(const std::string& name, const std::string& content = "old") {
    std::ofstream out(dir_ + "/" + name, std::ios::app);
    out << content;
  }

  std::string dir_;
};

// =============================================================================
// A. 初始化
// =============================================================================

TEST_F(LogTest, InitCreatesBothChannels) {
  const LogInitReport report = init_default();

  EXPECT_TRUE(report.ok);
  EXPECT_EQ(report.dir, dir_);
  EXPECT_EQ(report.opened_files.size(), 2u);
  EXPECT_TRUE(report.problems.empty()) << report.problems.front();
  EXPECT_TRUE(fs::exists(app_path()));
  EXPECT_TRUE(fs::exists(judge_path()));
  EXPECT_TRUE(log_initialized());
  EXPECT_EQ(log_path(LogChannel::kApp), app_path());
  EXPECT_EQ(log_path(LogChannel::kJudge), judge_path());
  EXPECT_EQ(log_current_date(LogChannel::kApp), date_of(base_day()));
}

TEST_F(LogTest, InitCreatesMissingDirectory) {
  const std::string nested = dir_ + "/a/b/c";
  LogOptions options;
  options.dir = nested;

  const LogInitReport report = init_logging(options);

  EXPECT_TRUE(report.ok);
  EXPECT_TRUE(fs::exists(nested + "/app.log"));
  EXPECT_TRUE(fs::exists(nested + "/judge.log"));
}

TEST_F(LogTest, InitReportsProblemWhenDirIsNotADirectory) {
  // 用"已存在的普通文件"当日志目录：create_directories 必然失败，
  // 且失败与运行身份（root / 普通用户）无关，断言稳定
  const std::string blocker = dir_ + "/not-a-dir";
  { std::ofstream out(blocker); out << "x"; }
  LogOptions options;
  options.dir = blocker;

  const LogInitReport report = init_logging(options);

  EXPECT_FALSE(report.ok);
  EXPECT_FALSE(report.problems.empty());
  EXPECT_FALSE(log_initialized());
  // 降级到 stderr：日志调用不应崩溃（这条由 InitReportsProblem 之后的写入隐式覆盖）
  log_info(LogChannel::kApp, "降级路径也必须不崩");
}

TEST_F(LogTest, NoArgInitUsesConfigLogDir) {
  // 不去断言 config 的具体值（那是 config 模块的职责），只断言"无参版确实取自 config"
  ::setenv("OJ_LOG_DIR", dir_.c_str(), 1);
  ::setenv("OJ_LOG_RETENTION_DAYS", "3", 1);
  const std::string env_path = dir_ + "/does-not-exist.env";
  const bool loaded = load_config(env_path).ok;
  ::unsetenv("OJ_LOG_DIR");
  ::unsetenv("OJ_LOG_RETENTION_DAYS");
  if (!loaded) {
    GTEST_SKIP() << "进程环境里存在互相矛盾的 OJ_* 变量，load_config 失败，跳过";
  }
  EXPECT_EQ(config().log_dir, dir_);
  EXPECT_EQ(config().log_retention_days, 3);

  const LogInitReport report = init_logging();

  EXPECT_TRUE(report.ok);
  EXPECT_EQ(log_path(LogChannel::kApp), app_path());
  EXPECT_TRUE(fs::exists(app_path()));
}

// =============================================================================
// B. 行不变量
// =============================================================================

TEST_F(LogTest, EveryLineCarriesFullPrefix) {
  init_default();

  log_info(LogChannel::kApp, "服务启动");

  const std::vector<std::string> lines = read_lines(app_path());
  ASSERT_EQ(lines.size(), 1u);
  // 格式：<日期> <时间.毫秒> <等级5> [tid=N] [通道5] <正文>
  EXPECT_TRUE(starts_with(lines[0], date_of(base_day()) + " ")) << lines[0];
  EXPECT_NE(lines[0].find(" INFO  [tid="), std::string::npos) << lines[0];
  EXPECT_NE(lines[0].find("[app] "), std::string::npos) << lines[0];
  EXPECT_NE(lines[0].size() >= 7 ? lines[0].substr(lines[0].size() - 7) : "",
            "服务启动");
}

TEST_F(LogTest, NewlineInSingleLineApiIsEscaped) {
  init_default();

  log_info(LogChannel::kApp, "第一行\n第二行\r第三行");

  const std::vector<std::string> lines = read_lines(app_path());
  ASSERT_EQ(lines.size(), 1u) << "带换行的单行消息不得产生第二个物理行";
  EXPECT_NE(lines[0].find("第一行\\n第二行\\r第三行"), std::string::npos) << lines[0];
}

TEST_F(LogTest, Utf8AndTabsSurvive) {
  init_default();

  log_info(LogChannel::kApp, "管理员名单同步完成：alice、carol");
  log_info(LogChannel::kApp, "缩进\there");

  const std::vector<std::string> lines = read_lines(app_path());
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_NE(lines[0].find("alice、carol"), std::string::npos) << lines[0];
  EXPECT_NE(lines[1].find("缩进\\there"), std::string::npos) << lines[1];
}

// =============================================================================
// C. 双通道与等级阈值
// =============================================================================

TEST_F(LogTest, ChannelsAreSeparateFiles) {
  init_default();

  log_info(LogChannel::kApp, "只有应用通道能看到这句");
  log_info(LogChannel::kJudge, "只有 judge 通道能看到这句");

  EXPECT_NE(read_file(app_path()).find("只有应用通道"), std::string::npos);
  EXPECT_EQ(read_file(app_path()).find("只有 judge 通道"), std::string::npos);
  EXPECT_NE(read_file(judge_path()).find("只有 judge 通道"), std::string::npos);
  EXPECT_EQ(read_file(judge_path()).find("只有应用通道"), std::string::npos);
}

TEST_F(LogTest, DefaultLevelsAreAppInfoJudgeDebug) {
  init_default();

  // NF-5：judge 日志默认就要含生成代码与编译命令 → judge 必须默认收 DEBUG
  EXPECT_EQ(min_level(LogChannel::kApp), LogLevel::kInfo);
  EXPECT_EQ(min_level(LogChannel::kJudge), LogLevel::kDebug);

  log_debug(LogChannel::kApp, "app 的 DEBUG 默认被过滤");
  log_debug(LogChannel::kJudge, "judge 的 DEBUG 必须落盘");

  const LogStats stats = log_stats();
  EXPECT_EQ(stats.written[0], 0);
  EXPECT_EQ(stats.filtered[0], 1);
  EXPECT_EQ(stats.written[1], 1);
  EXPECT_EQ(read_file(app_path()).find("被过滤"), std::string::npos);
  EXPECT_NE(read_file(judge_path()).find("judge 的 DEBUG 必须落盘"), std::string::npos);
}

TEST_F(LogTest, SetMinLevelTakesEffectImmediately) {
  init_default();

  set_min_level(LogChannel::kApp, LogLevel::kDebug);
  log_debug(LogChannel::kApp, "临时打开 DEBUG");

  EXPECT_NE(read_file(app_path()).find("临时打开 DEBUG"), std::string::npos);
  set_min_level(LogChannel::kApp, LogLevel::kError);
  log_warn(LogChannel::kApp, "WARN 现在低于阈值");
  EXPECT_EQ(read_file(app_path()).find("WARN 现在低于阈值"), std::string::npos);
}

// =============================================================================
// D. 多行（启动回显 / 生成代码）
// =============================================================================

TEST_F(LogTest, LogTextPrefixesEveryLine) {
  init_default();

  log_text(LogChannel::kApp, LogLevel::kInfo, "生效配置:\n  HTTP : 0.0.0.0:8080\n\n  日志 : ./logs");

  const std::vector<std::string> lines = read_lines(app_path());
  ASSERT_EQ(lines.size(), 4u);
  for (const std::string& line : lines) {
    EXPECT_NE(line.find("[app] "), std::string::npos) << line;
  }
  EXPECT_NE(lines[0].find("生效配置:"), std::string::npos);
  EXPECT_NE(lines[1].find("0.0.0.0:8080"), std::string::npos);
  // 空行保留前缀（段落分隔），正文为空
  EXPECT_EQ(body_of(lines[2], "app"), "") << lines[2];
  EXPECT_NE(lines[3].find("日志 : ./logs"), std::string::npos);
}

TEST_F(LogTest, LogBlockKeepsBannersAndIndentsBody) {
  init_default();
  const std::string code = "int main() {\n\treturn 0;\n}";

  log_block(LogChannel::kJudge, LogLevel::kDebug, "tester.cpp", code);

  const std::vector<std::string> lines = read_lines(judge_path());
  ASSERT_EQ(lines.size(), 5u);
  EXPECT_EQ(body_of(lines[0], "judge"),
            "===== tester.cpp（3 行 / 25 字节）=====") << lines[0];
  EXPECT_EQ(body_of(lines[1], "judge"), "    int main() {");
  EXPECT_EQ(body_of(lines[2], "judge"), "    \treturn 0;")
      << "块内制表符必须保留 —— 生成的 tester.cpp 靠它表达缩进";
  EXPECT_EQ(body_of(lines[3], "judge"), "    }");
  EXPECT_EQ(body_of(lines[4], "judge"), "===== tester.cpp 结束 =====");
  // 行不变量：块内每一行同样带前缀，否则按天切割后无法判断归属
  for (const std::string& line : lines) {
    EXPECT_NE(line.find("[judge] "), std::string::npos) << line;
  }
}

// =============================================================================
// E. 结构化字段
// =============================================================================

TEST_F(LogTest, KvFieldsAreRenderedInOrder) {
  init_default();

  log_kv(LogChannel::kJudge, LogLevel::kInfo, "compile",
         {{"submissionId", "42"}, {"cmd", "g++ -std=c++17 -O2 -c user.cpp -o user.o"}});

  const std::string line = read_lines(judge_path()).front();
  EXPECT_NE(line.find("compile submissionId=42 cmd=\"g++ -std=c++17 -O2 -c user.cpp -o user.o\""),
            std::string::npos)
      << line;
}

TEST_F(LogTest, KvValuesAreEscapedLikeSingleLine) {
  init_default();

  log_kv(LogChannel::kJudge, LogLevel::kWarn, "case",
         {{"exit", "0"}, {"reason", "line1\nline2"}, {"signal", "SIGXCPU 之后"}});

  const std::vector<std::string> lines = read_lines(judge_path());
  ASSERT_EQ(lines.size(), 1u);
  // 换行被转义 → 无需加引号；含空格的取值加引号，便于按字段切分
  EXPECT_NE(lines[0].find("reason=line1\\nline2"), std::string::npos) << lines[0];
  EXPECT_NE(lines[0].find("signal=\"SIGXCPU 之后\""), std::string::npos) << lines[0];
}

// =============================================================================
// F. 按天切割 [D-34] / §1.4
// =============================================================================

TEST_F(LogTest, RotationHappensLazilyOnFirstWriteAfterMidnight) {
  init_default();
  const std::int64_t day1 = local_epoch(kYear, kMonth, kDay, 23, 59);
  const std::int64_t day2 = local_epoch(kYear, kMonth, kDay + 1, 0, 1);
  const std::string day1_name = date_of(day1);
  const std::string day2_name = date_of(day2);
  ASSERT_NE(day1_name, day2_name);

  log_test_set_time(day1);
  log_info(LogChannel::kApp, "第一天的最后一条");
  EXPECT_EQ(log_current_date(LogChannel::kApp), day1_name);

  // 跨过零点，但【还没有任何写入】→ 不应产生任何切割文件
  log_test_set_time(day2);
  EXPECT_FALSE(fs::exists(app_path() + "." + day1_name));
  EXPECT_EQ(log_stats().rotations[0], 0);

  log_info(LogChannel::kApp, "第二天的第一条");

  const LogStats stats = log_stats();
  EXPECT_EQ(stats.rotations[0], 1);
  EXPECT_EQ(stats.rotations[1], 0) << "judge 通道本轮没有写入，惰性切割下不应动它的文件";

  const std::string rotated = app_path() + "." + day1_name;
  ASSERT_TRUE(fs::exists(rotated));
  EXPECT_NE(read_file(rotated).find("第一天的最后一条"), std::string::npos);
  EXPECT_EQ(read_file(app_path()).find("第一天的最后一条"), std::string::npos);
  EXPECT_NE(read_file(app_path()).find("第二天的第一条"), std::string::npos);
  EXPECT_EQ(log_current_date(LogChannel::kApp), day2_name);
}

TEST_F(LogTest, RotationKeepsBothChannelsIndependent) {
  init_default();
  log_info(LogChannel::kApp, "app-第一天");
  log_info(LogChannel::kJudge, "judge-第一天");

  log_test_set_time(day_offset(1));
  log_info(LogChannel::kApp, "app-第二天");  // 只有 app 通道写入

  EXPECT_NE(read_file(judge_path()).find("judge-第一天"), std::string::npos)
      << "judge 通道没有写入就不该被切割";
  EXPECT_FALSE(fs::exists(judge_path() + "." + date_of(base_day())));
  EXPECT_TRUE(fs::exists(app_path() + "." + date_of(base_day())));
  EXPECT_EQ(read_file(app_path()).find("app-第一天"), std::string::npos);
}

TEST_F(LogTest, RotationNeverOverwritesExistingHistory) {
  init_default();
  log_info(LogChannel::kApp, "本次进程的第一天内容");
  // 同一天内二次切割（前一个进程在同一日期已切过一次）
  touch("app.log." + date_of(base_day()), "上一次进程的历史");

  log_test_set_time(day_offset(1));
  log_info(LogChannel::kApp, "第二天");

  EXPECT_EQ(read_file(app_path() + "." + date_of(base_day())), "上一次进程的历史")
      << "同名切割文件必须保留，不得被覆盖";
  EXPECT_NE(read_file(app_path() + "." + date_of(base_day()) + ".1").find("本次进程的第一天内容"),
            std::string::npos);
}

// =============================================================================
// G. 保留 N 天（§1.4 默认 7 天）
// =============================================================================

TEST_F(LogTest, RetentionDeletesOnlyExpiredRotatedFiles) {
  init_default();
  const std::string d0 = date_of(base_day());
  const std::string expired = "app.log." + date_of(day_offset(-8));
  const std::string kept = "app.log." + date_of(day_offset(-3));
  touch(expired, "过期的 app");
  touch("judge.log." + date_of(day_offset(-9)), "过期的 judge");
  touch(kept, "还在保留期内");
  touch("app.log.not-a-date", "后缀非法 → 不是本模块的切割文件");
  touch("judge.log.2026-13-40", "后缀像日期但不合法 → 同样不动");
  touch("notes.txt", "别人的文件");

  const LogInitReport report = init_default();

  EXPECT_EQ(report.deleted_rotated_files, 2u);
  EXPECT_FALSE(fs::exists(dir_ + "/" + expired));
  EXPECT_FALSE(fs::exists(dir_ + "/" + "judge.log." + date_of(day_offset(-9))));
  EXPECT_TRUE(fs::exists(dir_ + "/" + kept));
  EXPECT_TRUE(fs::exists(dir_ + "/app.log.not-a-date"));
  EXPECT_TRUE(fs::exists(dir_ + "/judge.log.2026-13-40"));
  EXPECT_TRUE(fs::exists(dir_ + "/notes.txt"));
  EXPECT_TRUE(fs::exists(dir_ + "/app.log"));
  EXPECT_EQ(read_file(dir_ + "/" + kept), "还在保留期内");
  EXPECT_EQ(log_stats().deleted[0], 1);
  EXPECT_EQ(log_stats().deleted[1], 1);
  (void)d0;
}

TEST_F(LogTest, RetentionBoundaryKeepsExactlyNDates) {
  const std::string oldest_kept = date_of(day_offset(-6));   // 第 7 个日期
  const std::string oldest_gone = date_of(day_offset(-7));   // 第 8 个日期
  touch("app.log." + oldest_kept, "第7天");
  touch("app.log." + oldest_gone, "第8天");

  const LogInitReport report = init_default(7);

  EXPECT_TRUE(fs::exists(dir_ + "/app.log." + oldest_kept)) << "7 天 = 今天 + 前 6 天";
  EXPECT_FALSE(fs::exists(dir_ + "/app.log." + oldest_gone));
  EXPECT_EQ(report.deleted_rotated_files, 1u);
}

TEST_F(LogTest, RetentionZeroKeepsOnlyToday) {
  touch("app.log." + date_of(day_offset(-1)), "昨天");

  init_default(0);

  EXPECT_FALSE(fs::exists(dir_ + "/app.log." + date_of(day_offset(-1))));
  EXPECT_TRUE(fs::exists(app_path()));
}

TEST_F(LogTest, RetentionAlsoRunsOnRotation) {
  init_default();
  // init 时不存在、切割时才出现的超期文件：靠切割时的那次清理
  touch("app.log." + date_of(day_offset(-30)), "很久以前");

  log_test_set_time(day_offset(1));
  log_info(LogChannel::kApp, "跨天");

  EXPECT_FALSE(fs::exists(dir_ + "/app.log." + date_of(day_offset(-30))));
  EXPECT_EQ(log_stats().deleted[0], 1);
}

// =============================================================================
// H. 并发（§2.3：HTTP 8 + judge 2 + 调度器 1 + 主线程 = 11 个写者）
// =============================================================================

TEST_F(LogTest, ConcurrentWritesDoNotLoseOrInterleaveLines) {
  init_default();
  constexpr int kThreads = 8;
  constexpr int kPerThread = 60;

  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([t]() {
      for (int i = 0; i < kPerThread; ++i) {
        log_kv(LogChannel::kApp, LogLevel::kInfo, "hit",
               {{"thread", std::to_string(t)}, {"seq", std::to_string(i)}});
      }
    });
  }
  for (std::thread& worker : workers) worker.join();

  const std::vector<std::string> lines = read_lines(app_path());
  EXPECT_EQ(lines.size(), static_cast<std::size_t>(kThreads * kPerThread));
  EXPECT_EQ(log_stats().written[0], kThreads * kPerThread);
  for (const std::string& line : lines) {
    // 每行必须完整：既不能丢字段，也不能两行被拼成一行
    EXPECT_NE(line.find("hit thread="), std::string::npos) << line;
    EXPECT_NE(line.find("seq="), std::string::npos) << line;
    EXPECT_NE(line.find("[app] "), std::string::npos) << line;
  }
}

TEST_F(LogTest, ConcurrentWritesAcrossChannelsWithRotation) {
  init_default();
  log_test_set_time(day_offset(1));

  std::vector<std::thread> workers;
  for (int t = 0; t < 4; ++t) {
    workers.emplace_back([t]() {
      const LogChannel channel = (t % 2 == 0) ? LogChannel::kApp : LogChannel::kJudge;
      for (int i = 0; i < 40; ++i) log_info(channel, "并发条目 " + std::to_string(t));
    });
  }
  for (std::thread& worker : workers) worker.join();

  EXPECT_EQ(log_stats().written[0] + log_stats().written[1], 160);
  EXPECT_EQ(read_lines(app_path()).size() + read_lines(judge_path()).size(), 160u);
}

// =============================================================================
// I. 失败、降级与截断
// =============================================================================

TEST_F(LogTest, UninitializedLoggerWritesToStderrAndCreatesNoFiles) {
  // 未 init：只降级 stderr，且【不创建任何文件】（否则会在 CWD 意外落盘）
  log_info(LogChannel::kApp, "未初始化也要不丢消息");
  log_block(LogChannel::kJudge, LogLevel::kDebug, "x", "y");

  EXPECT_FALSE(fs::exists(app_path()));
  EXPECT_FALSE(fs::exists(judge_path()));
  EXPECT_EQ(log_stats().written[0], 0);
  EXPECT_EQ(log_stats().written[1], 0);
  EXPECT_FALSE(log_initialized());
}

TEST_F(LogTest, ShutdownIsIdempotentAndPostShutdownWritesAreSafe) {
  init_default();
  log_info(LogChannel::kApp, "关闭前");

  shutdown_logging();
  EXPECT_FALSE(log_initialized());
  shutdown_logging();  // 幂等

  log_info(LogChannel::kApp, "关闭后仍不崩");
  EXPECT_FALSE(log_initialized());
  // 关闭前的日志仍在文件里
  EXPECT_NE(read_file(app_path()).find("关闭前"), std::string::npos);
}

TEST_F(LogTest, OversizedEntryIsTruncatedWithNotice) {
  LogOptions options;
  options.dir = dir_;
  options.max_entry_bytes = 128;
  init_logging(options);

  log_info(LogChannel::kApp, std::string(5000, 'x'));

  const std::vector<std::string> lines = read_lines(app_path());
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_NE(lines[0].find("已截断：原文 5000 字节"), std::string::npos) << lines[0];
  EXPECT_LT(lines[0].size(), 400u);
}

TEST_F(LogTest, OversizedBlockIsTruncatedButKeepsBanners) {
  LogOptions options;
  options.dir = dir_;
  options.max_entry_bytes = 200;
  init_logging(options);

  std::string code;
  for (int i = 0; i < 500; ++i) code += "line" + std::to_string(i) + "\n";
  log_block(LogChannel::kJudge, LogLevel::kDebug, "tester.cpp", code);

  const std::vector<std::string> lines = read_lines(judge_path());
  ASSERT_GE(lines.size(), 3u);
  EXPECT_NE(lines.front().find("===== tester.cpp"), std::string::npos) << lines.front();
  EXPECT_NE(lines.back().find("结束 ====="), std::string::npos) << lines.back();
  std::string joined;
  for (const std::string& line : lines) joined += line + "\n";
  EXPECT_NE(joined.find("已截断"), std::string::npos) << "截断必须留痕，不能静默丢内容";
  EXPECT_LT(joined.size(), 4000u);
}

TEST_F(LogTest, TruncationNeverSplitsAUtf8Character) {
  LogOptions options;
  options.dir = dir_;
  options.max_entry_bytes = 40;
  init_logging(options);

  // 上限 40 字节必然落在某个三字节汉字中间（"错"前缀 + 中文串）
  std::string filler;  // 用字面量拼接而不是 std::string(n, '字')：后者是多字符常量
  for (int i = 0; i < 200; ++i) filler += "字";
  log_info(LogChannel::kApp, "错" + filler);

  const std::string line = read_lines(app_path()).front();
  const std::string body = body_of(line, "app");
  // 截断点在正文里，尾部以提示文字结束；提示之前不能有半个 UTF-8 序列
  const std::size_t notice = body.find("...[已截断");
  ASSERT_NE(notice, std::string::npos) << line;
  const std::string kept = body.substr(0, notice);
  for (std::size_t i = 0; i < kept.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(kept[i]);
    if ((c & 0xC0) == 0x80) continue;                 // 续字节合法
    const int width = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
    ASSERT_GE(kept.size(), i + static_cast<std::size_t>(width)) << "半个字符";
    i += static_cast<std::size_t>(width) - 1;
  }
}

TEST_F(LogTest, ReinitTruncatesCurrentFilesAndResetsCounters) {
  init_default();
  log_info(LogChannel::kApp, "第一次运行");
  EXPECT_EQ(log_stats().written[0], 1);

  const LogInitReport report = init_default();

  EXPECT_TRUE(report.ok);
  EXPECT_EQ(log_stats().written[0], 0) << "init 即重置统计，避免跨生命周期累计误读";
  EXPECT_EQ(read_file(app_path()), "") << "重新 init 截断当前文件（当天日志只留一份）";
}

TEST_F(LogTest, ToStringTagsAreStable) {
  EXPECT_STREQ(to_string(LogLevel::kError), "ERROR");
  EXPECT_STREQ(to_string(LogChannel::kApp), "app");
  EXPECT_STREQ(to_string(LogChannel::kJudge), "judge");
}

}  // namespace
