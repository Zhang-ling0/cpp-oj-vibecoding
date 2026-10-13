// =============================================================================
// CppOJ 入口
//
// 【当前推进到 §2.3 启动顺序的哪一步】
//   ✅ 1. load_config()          —— 读 .env + 进程环境并做交叉校验（T0.4）
//   ✅ 2. init_logging()         —— 双文件按天切割（T0.4）
//   ✅ 3. 连接池就绪              —— init_from_config()，失败即拒绝启动（T3.1）
//   ✅ 4. 幂等建表               —— 执行 db/init/01-schema.sql 并核对 11 张表（T0.3）
//   ✅ 5. 题库同步               —— 读 problems/*.json → 校验 → 入库 → 灌只读缓存（T1.2）
//   ⏳ 6. 角色同步（T3.11）→ 7. judge 调度器（T2.7）→ 8. listen（T0.5）
//   后三步都还没接线，因此本次进程在同步完题库、回显完配置后即退出。
//
// 【为什么每一步失败都直接退出，而不是打个 WARNING 继续】
//   后面每一步都以前一步为地基：没有连接池，建表无处执行；没有表，题库同步无处落库；
//   没有题库，judge 无从判题。带着半份地基继续跑，产出的错误信息会在几百行之后
//   才浮现，而且指向的都不是真正的病因 —— 这与 config 模块"配置自相矛盾就拒绝启动"
//   是同一条判断，不是两套哲学。
//
// 【建表为什么放在这里而不是 T1.2】
//   §2.1-A 第 1 条：路线 A 不走 /docker-entrypoint-initdb.d（那是容器机制），
//   建表是【应用启动时幂等执行】的。因此它必须排在连接池之后、题库同步之前 ——
//   题库同步要往 problem_cases / problem_params 里写行（§4.3）。
//
// 【题库同步失败为什么算致命，而"某几道题被跳过"不算】
//   两者的性质不同，见 §5.6.1：
//     · 目录读不到 / 表不存在 / 某道题写库失败 = 地基问题，继续跑会让 judge 拿着
//       与 DB 不一致的题库判题，故拒绝启动；
//     · 某道题校验不通过 = 题库自己的问题，决策明确要求"跳过该题、其余照常"，
//       因此只打 ERROR + WARN，不阻止服务起来（50 道题里 1 道写错不该让服务起不来）。
// =============================================================================
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "common/config.h"
#include "common/log.h"
#include "db/mysql_pool.h"
#include "problem/loader.h"

namespace {

// 路线 A 的 DDL 路径（§6.1）。相对【当前工作目录】—— 与 config 的 .env、
// log 的 ./logs、题库的 ./problems 是同一条前提：必须从项目根目录启动（make run 保证）。
constexpr const char* kSchemaPath = "db/init/01-schema.sql";

bool read_schema(std::string* out, std::string* error) {
  std::ifstream in(kSchemaPath, std::ios::binary);
  if (!in) {
    *error = std::string("无法读取 ") + kSchemaPath +
             "（相对当前工作目录）。请从项目根目录启动（make run 会这么做）；"
             "路线 B 不需要这一步，mysqld 会自己执行 /docker-entrypoint-initdb.d";
    return false;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  if (!in && !in.eof()) {
    *error = std::string("读取 ") + kSchemaPath + " 出错";
    return false;
  }
  *out = buffer.str();
  return true;
}

// 从【已拆好、注释已剥掉】的语句里取出表名，【不另存一份表名清单】。
//   为什么不写死 11 个名字：那样这份清单和 SQL 文件就成了一对必须手工同步的副本，
//   漂移了没人会发现（"多加一张表忘了改清单"→ 核对静默失准）。
//   直接扫语句，SQL 文件永远是唯一真源。
//   同时兼容 CREATE TABLE x 与 CREATE TABLE IF NOT EXISTS x 两种写法。
std::vector<std::string> table_names_of(const std::vector<std::string>& statements) {
  static const std::string kMarker = "CREATE TABLE";
  std::vector<std::string> names;
  for (const std::string& statement : statements) {
    const std::size_t found = statement.find(kMarker);
    if (found == std::string::npos) continue;
    // 去掉可选的 IF NOT EXISTS（大小写与空格数都不固定，按 token 跳）
    std::istringstream tokens(statement.substr(found + kMarker.size()));
    std::string token;
    if (tokens >> token && token == "IF") {
      tokens >> token;  // NOT
      tokens >> token;  // EXISTS
    }
    if (tokens >> token && !token.empty() && token != "(") {
      names.push_back(token);
    }
  }
  return names;
}

// 核对建表结果：DDL 跑了 N 条语句 ≠ N 张表真的在。
//   IF NOT EXISTS 会静默跳过已存在的表，而 exec_script 只在"某条语句报错"时才算失败 ——
//   于是"连错了库 / 表被手工删了一半"这类问题会一路溜到 T1.2 才炸，届时报错指向
//   "unknown column"之类完全无关的地方。这里查一次 information_schema，代价一个往返。
bool verify_tables(oj::db::Session& session,
                   const std::vector<std::string>& expected, std::string* missing) {
  std::vector<oj::db::Value> params;
  std::string sql =
      "SELECT table_name FROM information_schema.tables "
      "WHERE table_schema = DATABASE() AND table_name IN (";
  for (std::size_t i = 0; i < expected.size(); ++i) {
    if (i != 0) sql += ",";
    sql += "?";
    params.emplace_back(oj::db::Value::from_text(expected[i]));
  }
  sql += ")";

  const oj::db::ResultSet existing = session.run(sql, params);
  if (existing.failed()) {
    *missing = "核对失败（读 information_schema 报错）：" + existing.error().format();
    return false;
  }
  for (const std::string& want : expected) {
    if (existing.find(0, want) == nullptr) {
      if (!missing->empty()) *missing += "、";
      *missing += want;
    }
  }
  return missing->empty();
}

// 幂等建表（§2.1-A 第 1 条 / T0.3）。返回 false 时 error 里有人可读的原因。
bool apply_schema(std::string* error) {
  std::string sql;
  if (!read_schema(&sql, error)) return false;

  // 先拆一遍再执行：表名核对要在执行之前知道"这份脚本里有几张表"。
  // 切分规则直接借用 db::split_statements（与 exec_script 同一套实现，且注释已被剥掉）
  // —— 否则本文件头部注释里出现的 "CREATE TABLE" 字样会被当成一条建表语句，
  // 核对就会误报"脚本被截断"。这正是本函数不自己扫原文、而是复用池的切分器的原因。
  oj::db::DbError parse_error;
  const std::vector<std::string> statements = oj::db::split_statements(sql, &parse_error);
  if (!parse_error.ok()) {
    *error = parse_error.format();
    return false;
  }
  const std::vector<std::string> tables = table_names_of(statements);
  if (tables.empty()) {
    *error = std::string(kSchemaPath) +
             " 里没有切出任何 CREATE TABLE —— 文件为空、被截断，或路径指错了文件";
    return false;
  }

  oj::db::Lease lease = oj::db::Pool::instance().acquire();
  if (!lease.valid()) {
    *error = "连接池未就绪，取不到租约（见上一条 ERROR）";
    return false;
  }
  std::vector<std::string> failed;
  const std::size_t executed = lease->exec_script(sql, &failed);
  if (!failed.empty()) {
    *error = std::string("执行 ") + kSchemaPath + " 在第 " + std::to_string(executed + 1) +
             " 条语句处失败：" + failed[0];
    return false;
  }
  if (executed != statements.size()) {
    *error = std::string("脚本切出 ") + std::to_string(statements.size()) +
             " 条语句，实际执行 " + std::to_string(executed) + " 条";
    return false;
  }

  std::string missing;
  if (!verify_tables(lease.session(), tables, &missing)) {
    *error = "建表语句执行完毕，但以下表并不存在于当前库：" + missing;
    return false;
  }
  oj::common::log_info(oj::common::LogChannel::kApp,
                       "建表完成 " + std::string(kSchemaPath) + "：执行 " +
                           std::to_string(executed) + " 条语句，" +
                           std::to_string(tables.size()) + " 张表均已存在（§3.2）");
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  // 允许 `./oj /path/to/.env` 覆盖默认路径，便于临时验证另一份配置
  const std::string env_path = (argc > 1) ? argv[1] : ".env";

  // ---- 1. 配置 ----
  const oj::common::LoadResult result = oj::common::load_config(env_path);
  std::printf("%s\n", oj::common::format_diagnostics(result).c_str());

  // 日志必须在配置之后、线程池之前初始化（log.h 的【启动契约】）：
  // log_dir / retention_days 都来自配置；反过来，"连配置都没读到"这种
  // 启动期失败会无处可记 —— init_logging() 在配置未加载时用内置默认值。
  const oj::common::LogInitReport log_report = oj::common::init_logging();
  for (const std::string& problem : log_report.problems) {
    std::fprintf(stderr, "[oj] %s\n", problem.c_str());
  }

  if (!result.ok) {
    // 配置自相矛盾时拒绝启动：带着半份配置跑起来比跑不起来危险得多
    oj::common::log_error(oj::common::LogChannel::kApp,
                          "配置校验未通过（" +
                              std::to_string(result.error_count()) +
                              " 条 ERROR），拒绝启动");
    oj::common::shutdown_logging();
    std::fprintf(stderr, "[oj] 配置校验未通过（%zu 条 ERROR），拒绝启动\n",
                 result.error_count());
    return 1;
  }

  // ---- 3. 连接池就绪 ----
  std::vector<std::string> db_problems;
  if (!oj::db::Pool::instance().init_from_config(&db_problems)) {
    for (const std::string& problem : db_problems) {
      std::fprintf(stderr, "[oj] %s\n", problem.c_str());
    }
    oj::common::log_error(oj::common::LogChannel::kApp,
                          "数据库连接失败（" + std::to_string(db_problems.size()) +
                              " 条），拒绝启动。路线 A 下先确认 mysqld 已启动（make run 会拉起它）");
    oj::common::shutdown_logging();
    return 1;
  }

  // ---- 4. 幂等建表 ----
  std::string schema_error;
  if (!apply_schema(&schema_error)) {
    std::fprintf(stderr, "[oj] %s\n", schema_error.c_str());
    oj::common::log_error(oj::common::LogChannel::kApp, "建表失败：" + schema_error);
    oj::db::Pool::instance().shutdown();
    oj::common::shutdown_logging();
    return 1;
  }

  // ---- 5. 题库同步（T1.2 / §4.3）----
  // 这一步做三件事：读目录 → 两层校验（json_schema）→ 单事务幂等入库 → 灌只读缓存。
  // 被跳过的题已经由加载器逐条打进 app.log 的 ERROR，这里只把汇总打到 stdout，
  // 因为"启动回显"是录题时最常看的一段（§4.3 第 5 步：按日志修正 JSON）。
  oj::problem::LoadOptions load_options;
  load_options.dir = oj::common::config().problems_dir;
  const oj::problem::LoadReport load_report = oj::problem::load_all(load_options);
  if (!load_report.ok) {
    std::fprintf(stderr, "[oj] %s\n", load_report.format().c_str());
    oj::common::log_error(oj::common::LogChannel::kApp,
                          "题库同步失败，拒绝启动（详见 app.log 与上方 stderr）");
    oj::db::Pool::instance().shutdown();
    oj::common::shutdown_logging();
    return 1;
  }
  if (load_report.skipped > 0) {
    std::printf("[oj] 注意：%zu 道题被跳过（校验不通过），它们不会出现在列表页。逐条原因见 app.log。\n",
                load_report.skipped);
  }

  // ---- 启动回显 ----
  const std::string config_echo = oj::common::dump_config();
  std::printf("\n%s\n", config_echo.c_str());
  std::printf("\n[oj] 数据库就绪：%s\n",
              oj::db::Pool::instance().describe().c_str());
  std::printf("%s\n", load_report.summary().c_str());
  std::printf("[oj] 判题缓存已就绪：%zu 题。角色同步 / judge / HTTP 均待接线"
              "（T3.11 / T2.7 / T0.5），本次进程即将退出。\n",
              oj::problem::cache_size());

  oj::common::log_text(oj::common::LogChannel::kApp, oj::common::LogLevel::kInfo,
                       config_echo);
  oj::common::log_info(oj::common::LogChannel::kApp,
                       "数据库就绪 " + oj::db::Pool::instance().describe());
  oj::common::log_info(oj::common::LogChannel::kApp,
                       load_report.summary() + "；判题缓存 " +
                           std::to_string(oj::problem::cache_size()) + " 题");
  oj::common::log_info(oj::common::LogChannel::kApp,
                       "启动顺序推进到「DB 就绪 + 建表完成 + 题库同步完成」，后续步骤待接线");

  oj::db::Pool::instance().shutdown();
  oj::common::shutdown_logging();
  return 0;
}