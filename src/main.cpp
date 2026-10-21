// =============================================================================
// CppOJ 入口
//
// 【当前推进到 §2.3 启动顺序的哪一步】
//   ✅ 1. load_config()          —— 读 .env + 进程环境并做交叉校验（T0.4）
//   ✅ 2. init_logging()         —— 双文件按天切割（T0.4）
//   ✅ 3. 连接池就绪              —— init_from_config()，失败即拒绝启动（T3.1）
//   ✅ 4. 幂等建表               —— 执行 db/init/01-schema.sql 并核对 11 张表（T0.3）
//   ✅ 5. 题库同步               —— 读 problems/*.json → 校验 → 入库 → 灌只读缓存（T1.2）
//   ✅ 6. 工作区扫残             —— 清 /tmp/oj-judge/* 残留，必须排在 judge 之前（T2.9）
//   ✅ 7. 过期会话回收           —— 启动时一次（§6.3.2 / T3.10）
//   ✅ 8. 角色同步               —— T3.11（.env 名单 → users.role）
//   ✅ 9. 判题调度器             —— T2.7（队列 + workers + 闸门）
//   ✅ 10. HTTP 起服务并 listen  —— 静态挂载 + §6.3 的全部端点（T0.5 / T3.5 + V2.7 接线）
//   ⚠️ 第 8 步把本进程变成【常驻阻塞】——在此之前 `make run` 跑完就退出，
//      那正是 §9 T0.5 验收里"浏览器访问 localhost:8080 看到页面"的前提。
//
// 【信号处理：为什么只接 SIGINT/SIGTERM 两个】
//   listen() 阻塞在 accept 上，靠信号打断才是干净的退出路径。
//   stop_server() 只做一件事：让 httplib 从 accept 返回。**它不做资源回收** ——
//   DB 连接池与日志的关闭仍在 main 的正常流程里（listen 返回后顺序执行），
//   于是"收到信号"与"进程退出"之间仍然只有一条收尾路径。
//   SIGHUP 不接：SPEC 全篇只有「改配置 + 重启」这一条运维模式（§6.3.1），
//   给 SIGHUP 挂一个"重新读 .env"的语义等于开第二条（且未被文档定义的）路径。
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
#include <csignal>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "common/config.h"
#include "common/log.h"
#include "db/mysql_pool.h"
#include "judge/scheduler.h"
#include "judge/workspace.h"
#include "problem/loader.h"
#include "web/routes.h"
#include "web/session.h"

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

// 阶段 2 的新增列迁移（problems.archived / users.disabled）。
// CREATE TABLE IF NOT EXISTS 对【已存在的表】什么都不做 —— 01-schema.sql 加了列之后，
// 老库不会自动长出这一列，于是"新代码已在跑、老库还在按旧结构存"会在第一次读该列时报
// unknown column（且报错点离根因极远）。故建表后再补一次幂等 ALTER：有则跳过，无则加。
// 与建表同一套"可重复执行"的语义，不引入第二个迁移文件。
bool ensure_columns(std::string* error) {
  struct ColumnSpec {
    const char* table;
    const char* column;
    const char* ddl;
  };
  static const ColumnSpec kColumns[] = {
      {"users", "disabled",
       "ALTER TABLE users ADD COLUMN disabled TINYINT(1) NOT NULL DEFAULT 0"},
      {"problems", "archived",
       "ALTER TABLE problems ADD COLUMN archived TINYINT(1) NOT NULL DEFAULT 0"},
  };

  oj::db::Lease lease = oj::db::Pool::instance().acquire();
  if (!lease.valid()) {
    *error = "补列：连接池未就绪，取不到租约（见上一条 ERROR）";
    return false;
  }
  std::vector<std::string> added;
  for (const ColumnSpec& spec : kColumns) {
    const oj::db::ResultSet existing = lease->run(
        "SELECT COUNT(*) FROM information_schema.columns"
        " WHERE table_schema = DATABASE() AND table_name = ? AND column_name = ?",
        {oj::db::Value::from_text(spec.table), oj::db::Value::from_text(spec.column)});
    if (existing.failed()) {
      *error = std::string("补列：查 information_schema（") + spec.table + "." + spec.column +
               "）失败：" + existing.error().format();
      return false;
    }
    if (!existing.empty() && existing.at(0).i64(0) > 0) continue;   // 已存在（幂等）
    const oj::db::ResultSet altered = lease->run(spec.ddl);
    if (altered.failed()) {
      *error = std::string("补列：执行 ALTER（") + spec.table + "." + spec.column +
               "）失败：" + altered.error().format();
      return false;
    }
    added.push_back(std::string(spec.table) + "." + spec.column);
  }
  if (!added.empty()) {
    std::string names;
    for (const std::string& name : added) {
      if (!names.empty()) names += "、";
      names += name;
    }
    oj::common::log_info(oj::common::LogChannel::kApp,
                         "已为老库补上阶段 2 新增列：" + names);
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  // 本进程自 T0.5 起转为常驻（listen 阻塞），而启动回显是【启动期诊断的唯一出口】
  // —— 配置错误、端口被占、题库同步失败都在退出前打到 stdout。
  // stdout 默认在【非 TTY】（重定向到文件、nohup、systemd）时是全缓冲：
  // 于是 `make run` 看得到回显，而 `./build/oj > log 2>&1 &` 的日志文件在
  // 出错那一刻是空的 —— 恰恰是最需要它的时刻。一行改成行缓冲即可。
  std::setvbuf(stdout, nullptr, _IOLBF, 0);

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

  // ---- 4b. 阶段 2 新增列的幂等补列（老库长出 problems.archived / users.disabled）----
  std::string column_error;
  if (!ensure_columns(&column_error)) {
    std::fprintf(stderr, "[oj] %s\n", column_error.c_str());
    oj::common::log_error(oj::common::LogChannel::kApp, "补列失败：" + column_error);
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

  // ---- 6. 工作区扫残（T2.9 / §5.2 末句）----
  // 排在 judge 调度器启动【之前】是硬要求：worker 正在编译时目录被删，表现为随机的
  // CE/JE 且极难复现（judge/workspace.h【调用时机】）。现在 judge 还没接线，正是
  // 该做的时机 —— 等接线后这一步的顺序就没有"看起来可以挪一挪"的余地了。
  // 失败只 WARN 不拒绝启动：清不掉残留目录不影响判题，而"起不来"比"留个垃圾目录"更糟。
  // 汇总与逐条原因分两条记：log.h 的行不变量要求每行自带前缀，多行文本必须走
  // log_text —— 把 \n 塞进 log_info 的单行里，日志里会出现一行没有前缀的字面 \n。
  const oj::judge::SweepReport sweep = oj::judge::sweep_workspace(oj::judge::workspace_root());
  if (sweep.failed > 0) {
    oj::common::log_warn(oj::common::LogChannel::kApp,
                         sweep.summary() + "；残留目录将留到下次启动再清");
  } else if (sweep.removed > 0 || sweep.skipped > 0) {
    oj::common::log_info(oj::common::LogChannel::kApp,
                         sweep.summary() + "（上次进程被 kill -9 留下的工作区，§5.2）");
  }
  if (!sweep.notes.empty()) {
    oj::common::log_text(oj::common::LogChannel::kApp,
                         sweep.failed > 0 ? oj::common::LogLevel::kWarn
                                          : oj::common::LogLevel::kInfo,
                         "工作区扫残逐条：\n" + sweep.notes);
  }
  if (!sweep.notes.empty()) std::printf("[oj] %s\n", sweep.notes.c_str());

  // ---- 7. 过期会话回收（T3.10 / §6.3.2）----
  // §6.3.2 要求「启动时一次 + 每次登录时一次」。登录那一次已并入
  // session::issue 的同一事务（无需另调），这里的调用补上"启动时一次"，
  // 它管的是「服务停了三天再起来」这一段 —— 那些行没人会去登录，也就没人会触发回收。
  //
  // 【失败只记 WARN，不拒绝启动】：回收失败不影响任何在线功能（查会话的 SQL
  // 自己带 `expires_at > UTC_TIMESTAMP()`，过期会话本来就查不出来），
  // 而"起不来"比"多留几行过期会话"严重得多。
  long purged_sessions = 0;
  {
    oj::db::Lease lease = oj::db::Pool::instance().acquire();
    std::string purge_error;
    if (!lease.valid()) {
      oj::common::log_warn(oj::common::LogChannel::kApp,
                           "过期会话回收未执行：连接池取不到租约（见上一条 ERROR）");
    } else if (!oj::web::purge_expired(lease.session(), &purged_sessions, &purge_error)) {
      oj::common::log_warn(oj::common::LogChannel::kApp,
                           "过期会话回收失败（不影响启动，过期会话本就查不出来）：" + purge_error);
    } else if (purged_sessions > 0) {
      oj::common::log_info(oj::common::LogChannel::kApp,
                           "已回收过期会话 " + std::to_string(purged_sessions) +
                               " 条（§6.3.2，expires_at < UTC_TIMESTAMP()）");
    }
  }

  // ---- 启动回显 ----
  const std::string config_echo = oj::common::dump_config();
  std::printf("\n%s\n", config_echo.c_str());
  std::printf("\n[oj] 数据库就绪：%s\n",
              oj::db::Pool::instance().describe().c_str());
  std::printf("%s\n", load_report.summary().c_str());
  std::printf("[oj] %s（根目录 %s）\n", sweep.summary().c_str(),
              oj::judge::workspace_root().c_str());
  std::printf("[oj] 判题缓存已就绪：%zu 题；已回收过期会话 %ld 条\n",
              oj::problem::cache_size(), purged_sessions);
  const std::string admin_echo =
      oj::common::config().admin_usernames_configured
          ? "共 " + std::to_string(oj::common::config().admin_usernames.size()) + " 个（T3.11 已接线，启动时同步）"
          : "未设置（保持库中既有 role 不变）";
  std::printf("[oj] 注册开关 OJ_ALLOW_REGISTER=%s；管理员名单%s\n",
              oj::common::config().allow_register ? "1（开放注册）" : "0（已关闭注册）",
              admin_echo.c_str());
  oj::common::log_text(oj::common::LogChannel::kApp, oj::common::LogLevel::kInfo, config_echo);
  oj::common::log_info(oj::common::LogChannel::kApp,
                       "数据库就绪 " + oj::db::Pool::instance().describe());
  oj::common::log_info(oj::common::LogChannel::kApp,
                       load_report.summary() + "；判题缓存 " +
                           std::to_string(oj::problem::cache_size()) + " 题");
  // ---- 角色同步 T3.11 / §6.3.1 ----
  // D-37：OJ_ADMIN_USERNAMES 是 role 唯一真源。configured=false 完全跳过（零变化）。
  if (oj::common::config().admin_usernames_configured) {
    oj::db::Lease role_lease = oj::db::Pool::instance().acquire();
    if (role_lease.valid()) {
      // 先全部降为 user，再按名单提权（名单外降权是 D-37 的语义）
      role_lease->run("UPDATE users SET role='user' WHERE role='admin'");
      std::size_t promoted = 0;
      for (const std::string& name : oj::common::config().admin_usernames) {
        const oj::db::ResultSet r = role_lease->run(
            "UPDATE users SET role='admin' WHERE username = ?",
            {oj::db::Value::from_text(name)});
        if (!r.failed() && r.affected_rows() > 0) {
          ++promoted;
        } else if (!r.failed()) {
          std::printf("[oj] 警告：OJ_ADMIN_USERNAMES 中的 %s 不存在，已忽略\n", name.c_str());
          oj::common::log_warn(oj::common::LogChannel::kApp,
                               "角色同步：用户名不存在 " + name);
        }
      }
      std::printf("[oj] 角色同步完成：提权 %zu 人（T3.11）\n", promoted);
      oj::common::log_info(oj::common::LogChannel::kJudge,
                           "角色同步完成：提权 " + std::to_string(promoted) + " 人");
    }
  }

  // ---- 判题调度器（T2.7 / 阶段 1）----
  // 必须在工作区扫残之后、HTTP listen 之前启动（§2.3）。
  if (!oj::judge::scheduler().start()) {
    std::fprintf(stderr, "[oj] 判题调度器启动失败，拒绝启动\n");
    oj::common::log_error(oj::common::LogChannel::kJudge, "调度器启动失败");
    oj::db::Pool::instance().shutdown();
    oj::common::shutdown_logging();
    return 1;
  }
  std::printf("[oj] 判题调度器已启动（workers=%d gate=%d）\n",
              oj::common::config().judge_workers, oj::common::config().judge_global_gate);

  // ---- 8. HTTP 起服务并 listen（T0.5 / T3.5 认证组）----
  // 排在最后：listen() 之前的所有失败都以"拒绝启动"的方式响亮失败，
  // 而 listen 之后本进程转为常驻，此时再出的问题只能靠 app.log 事后查。
  //
  // 【信号处理器里只调用 async-signal-safe 的那一个函数】
  //   web::stop_server() 内部只有一次 httplib 的 self_pipe 写入（它自己保证
  //   async-signal-safe），而 std::signal 的平台实现决定了：这里【不能】写日志、
  //   不能加锁、不能分配内存 —— 曾经的日志调用会在信号与主流程交错时死锁。
  //   故 stop_server 的实现里没有一行日志，需要的上下文由 listen 返回后的正常
  //   流程补记。
  std::signal(SIGINT, [](int) { oj::web::stop_server(); });
  std::signal(SIGTERM, [](int) { oj::web::stop_server(); });

  oj::web::ServerOptions server_options;
  server_options.static_dir = oj::common::config().static_dir;
  server_options.listen_host = oj::common::config().listen_host;
  server_options.listen_port = oj::common::config().listen_port;

  // 线程池数必须打【config 的值】而不是写死：阶段 2 之后 OJ_HTTP_THREADS 是运行期生效的
  // （listen_and_serve 用它建固定 ThreadPool），dump_config 与这行启动日志若各说各话，
  // 排查"改了 .env 为什么没生效"时会先被这行误导。
  std::printf("[oj] HTTP 监听 http://%s:%d（静态根 %s，线程池 %d）；"
              "Ctrl-C 停止。登录页 http://127.0.0.1:%d/login.html\n",
              server_options.listen_host.c_str(), server_options.listen_port,
              server_options.static_dir.c_str(), oj::common::config().http_threads,
              server_options.listen_port);

  if (!oj::web::listen_and_serve(server_options)) {
    oj::common::log_error(oj::common::LogChannel::kApp,
                          "HTTP 服务未能监听 " + server_options.listen_host + ":" +
                              std::to_string(server_options.listen_port) +
                              "（端口被占用是最常见的一种）");
    std::fprintf(stderr,
                 "[oj] HTTP 服务未能监听 %s:%d —— 请检查端口是否被占用"
                 "（OJ_HTTP_PORT 可改端口，见 .env）\n",
                 server_options.listen_host.c_str(), server_options.listen_port);
    oj::judge::scheduler().stop();
    oj::db::Pool::instance().shutdown();
    oj::common::shutdown_logging();
    return 1;
  }

  // listen 返回 = 收到 SIGINT/SIGTERM（唯一能走到这里的路径，routes.h 已说明）。
  oj::common::log_info(oj::common::LogChannel::kApp, "收到停止信号，HTTP 已停止接收新请求");
  std::printf("\n[oj] 已停止。\n");
  oj::judge::scheduler().stop();
  oj::db::Pool::instance().shutdown();
  oj::common::shutdown_logging();
  return 0;
}