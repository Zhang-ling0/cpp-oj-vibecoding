// =============================================================================
// 题库加载器的实现（SPEC.md §9 M1 T1.2）
//
// 【实现要点】
//   1. 一题一个租约 + 一个事务（理由见 loader.h【一题一个事务，不是全库一个事务】）。
//      租约不在循环外持有，是因为 [D-21] 的全局互斥量会让整库同步期间的所有 HTTP 请求
//      排队；虽然现在只在启动期调用（本模块跑在 listen 之前），但把持锁范围压到
//      "一道题"是零成本的正确写法，不必赌"将来不会在运行期调它"。
//   2. tags 的 upsert 用 ON DUPLICATE KEY UPDATE id = LAST_INSERT_ID(id) —— 这是 MySQL
//      取"已存在行的 id"的唯一惯用手法（否则 ON DUPLICATE 之后 LAST_INSERT_ID() 返回 0）。
//      依赖 uk_name_cat 存在（§3.2 有），因此标签名 + 类别是天然幂等的。
//   3. 全部 SQL 走预处理语句占位符（§1.4「所有 SQL 走预处理语句」）。连接池还有两道
//      本地护栏：占位符个数与参数个数不符会直接失败 —— 本文件里每条语句的参数都在
//      同一处构造（make_params / 行内 vector），因此这类失配在开发期就会被拦住。
//   4. 时间列一律 NOW()：连接建立时 time_zone 被锁成 '+00:00'（mysql_pool.h【语句级细节】），
//      故 NOW() 存进去的就是 UTC（[D-17]）。DAO 侧不许用 CURDATE()/CONVERT_TZ。
// =============================================================================
#include "problem/loader.h"

#include <dirent.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <utility>

#include "common/config.h"
#include "common/log.h"
#include "db/mysql_pool.h"
#include "problem/json_schema.h"

namespace oj {
namespace problem {
namespace {

using db::Session;
using db::Value;

constexpr const char* kJsonSuffix = ".json";

// 本模块只碰这 5 张表（§3.2 的题库同步链）。写库前先确认它们都在 ——
// 报错要指向"没建表"，而不是指向某条 INSERT 里的 unknown table。
const std::vector<std::string>& required_tables() {
  static const std::vector<std::string> tables = {"problems", "problem_cases", "problem_params",
                                                  "tags", "problem_tags"};
  return tables;
}

// ---- 目录枚举 -----------------------------------------------------------------

bool list_problem_files(const std::string& dir, std::vector<std::string>* files,
                        std::string* error) {
  DIR* handle = ::opendir(dir.c_str());
  if (handle == nullptr) {
    *error = "打不开题库目录 \"" + dir + "\"：" + std::strerror(errno) +
             "。题库目录由 OJ_PROBLEMS_DIR 指定（§6.7.3），且必须从项目根目录启动";
    return false;
  }
  while (const struct dirent* entry = ::readdir(handle)) {
    const std::string name = entry->d_name;
    if (name == "." || name == "..") continue;
    // 跳过隐藏文件：.gitkeep / .DS_Store / 编辑器临时文件（.two-sum.json.swp）
    if (name[0] == '.') continue;
    const std::size_t suffix_len = std::strlen(kJsonSuffix);
    if (name.size() <= suffix_len) continue;
    if (name.compare(name.size() - suffix_len, suffix_len, kJsonSuffix) != 0) continue;
    files->push_back(name);
  }
  ::closedir(handle);
  // 排序：让"哪几道题被跳过""先插入了哪个 tag"这类输出在多次启动之间保持一致。
  // 不排序时 readdir 的顺序取决于目录项的哈希布局，同一份题库两次启动日志会不同，
  // 而"日志不可复现"会让排查多花一倍时间。
  std::sort(files->begin(), files->end());
  return true;
}

std::string stem_of(const std::string& file_name) {
  return file_name.substr(0, file_name.size() - std::strlen(kJsonSuffix));
}

// ---- 建表存在性核对 -----------------------------------------------------------

bool ensure_tables(Session& session, std::string* missing) {
  const std::vector<std::string>& tables = required_tables();
  std::string sql =
      "SELECT table_name FROM information_schema.tables "
      "WHERE table_schema = DATABASE() AND table_name IN (";
  std::vector<Value> params;
  for (std::size_t i = 0; i < tables.size(); ++i) {
    if (i != 0) sql += ",";
    sql += "?";
    params.push_back(Value::from_text(tables[i]));
  }
  sql += ")";

  const db::ResultSet existing = session.run(sql, params);
  if (existing.failed()) {
    *missing = "读 information_schema 失败：" + existing.error().format();
    return false;
  }
  for (const std::string& want : tables) {
    if (existing.find(0, want) != nullptr) continue;
    if (!missing->empty()) *missing += "、";
    *missing += want;
  }
  return missing->empty();
}

// ---- 单题同步 -----------------------------------------------------------------

struct SyncOutcome {
  bool ok = false;
  int id = 0;
  std::size_t cases = 0;
  std::size_t params = 0;
  std::size_t tag_links = 0;
  std::string error;
};

// problems 主表的 14 个业务列（slug 单独处理：它是 upsert 的键）。
// 注意 case_count 在【这一次】写入时先置 0，等用例行插完后再单独刷新（§3.4 的维护契约：
// "只由题库加载器在 upsert 用例行后同事务刷新"）。这里两次赋值都在同一事务里，
// 中间态不会对别的连接可见（未提交的行对其他连接不可见）。
constexpr const char* kProblemColumns =
    "title, difficulty, statement_md, hint_md, signature, method_name, return_type, "
    "harness_prelude, code_template, time_limit_ms, mem_limit_kb, case_count, is_featured, sort_no";

// 生成 "?,?,?…" 占位符串。不用手写一长串问号的原因很实际：手写的那一长串与
// problem_params() 的元素个数必须在【列清单、VALUES 的问号、params 的 push 顺序】
// 三处保持一致，而连接池的"占位符个数 != 参数个数"护栏只能在【运行时】发现 ——
// 那是【第二次启动】才发生的事（首启动走 INSERT 分支，压根不拼 UPDATE 的 SQL），
// 也就是说一个只在 UPDATE 分支里的语法错误可以完整地活过一轮开发与手工验证。
std::string placeholders(std::size_t count) {
  std::string text;
  for (std::size_t i = 0; i < count; ++i) {
    if (i != 0) text += ",";
    text += "?";
  }
  return text;
}

// "title, difficulty, …" → "title = ?, difficulty = ?, …"，给 UPDATE 用。
// 必须由列清单【派生】而不是另抄一份：另抄的那份一旦与列清单错开一位，
// 症状是"某个字段的值写进了隔壁字段"—— 不报错、长度也对，只是数据错了。
std::string assignments(const std::string& column_list) {
  std::string text;
  std::istringstream stream(column_list);
  std::string column;
  while (std::getline(stream, column, ',')) {
    const std::size_t begin = column.find_first_not_of(" \t");
    if (begin == std::string::npos) continue;
    const std::size_t end = column.find_last_not_of(" \t");
    if (!text.empty()) text += ", ";
    text += column.substr(begin, end - begin + 1) + " = ?";
  }
  return text;
}

// 与 kProblemColumns 的列数一一对应（title … sort_no 共 14 个）
std::vector<Value> problem_params(const model::ProblemBundle& problem, int case_count) {
  std::vector<Value> params;
  params.reserve(14);
  params.push_back(Value::from_text(problem.title));
  params.push_back(Value::from_text(model::to_string(problem.difficulty)));
  params.push_back(Value::from_text(problem.statement_md));
  // 空串 = NULL：§4.1 两条脚注（hint_md 空则前端不渲染提示 tab；harness_prelude 空即无逃生舱）
  params.push_back(problem.hint_md.empty() ? Value::null() : Value::from_text(problem.hint_md));
  params.push_back(Value::from_text(problem.signature));
  params.push_back(Value::from_text(problem.method_name));
  params.push_back(Value::from_text(problem.return_type));
  params.push_back(problem.harness_prelude.empty() ? Value::null()
                                                   : Value::from_text(problem.harness_prelude));
  params.push_back(Value::from_text(problem.code_template));
  params.push_back(Value::from_u64(problem.time_limit_ms));
  params.push_back(Value::from_u64(problem.mem_limit_kb));
  params.push_back(Value::from_u64(static_cast<unsigned long long>(case_count)));
  params.push_back(Value::from_bool(problem.is_featured));
  params.push_back(Value::from_i64(problem.sort_no));
  return params;
}

SyncOutcome sync_problem(Session& session, const model::ProblemBundle& problem) {
  SyncOutcome outcome;
  db::Transaction transaction(session);
  if (!transaction.ok()) {
    outcome.error = "开启事务失败：" + session.error().format();
    return outcome;
  }

  // ---- 1. problems 主表：按 slug upsert --------------------------------------
  const db::ResultSet found =
      session.run("SELECT id FROM problems WHERE slug = ?", {Value::from_text(problem.slug)});
  if (found.failed()) {
    outcome.error = "查询 problems 失败：" + found.error().format();
    return outcome;
  }
  if (!found.empty()) {
    const int id = static_cast<int>(found.at(0).i64(0));
    std::vector<Value> params = problem_params(problem, 0);
    params.push_back(Value::from_i64(id));
    const db::ResultSet updated =
        session.run(std::string("UPDATE problems SET ") + assignments(kProblemColumns) +
                        ", updated_at = NOW() WHERE id = ?",
                    params);
    if (updated.failed()) {
      outcome.error = "更新 problems 失败：" + updated.error().format();
      return outcome;
    }
    outcome.id = id;
  } else {
    std::vector<Value> params;
    params.push_back(Value::from_text(problem.slug));  // 第 1 个参数：upsert 的键
    const std::vector<Value> columns = problem_params(problem, 0);
    params.insert(params.end(), columns.begin(), columns.end());
    // 列 = slug + kProblemColumns（14）+ created_at + updated_at；
    // 参数 = 15 个占位符 + 两个 NOW()（[D-17] 存 UTC，连接已锁 time_zone='+00:00'）
    const db::ResultSet inserted =
        session.run(std::string("INSERT INTO problems (slug, ") + kProblemColumns +
                        ", created_at, updated_at) VALUES (" + placeholders(params.size()) +
                        ", NOW(), NOW())",
                    params);
    if (inserted.failed()) {
      outcome.error = "插入 problems 失败：" + inserted.error().format();
      return outcome;
    }
    outcome.id = static_cast<int>(inserted.insert_id());
  }

  // ---- 2. delete-then-insert（§4.1 同步语义）--------------------------------
  const std::vector<Value> id_param = {Value::from_i64(outcome.id)};
  for (const char* table : {"problem_cases", "problem_params", "problem_tags"}) {
    const db::ResultSet deleted =
        session.run(std::string("DELETE FROM ") + table + " WHERE problem_id = ?", id_param);
    if (deleted.failed()) {
      outcome.error = std::string("清空 ") + table + " 失败：" + deleted.error().format();
      return outcome;
    }
  }

  // ---- 3. 形参行（problem_params）--------------------------------------------
  for (const model::ProblemParam& param : problem.params) {
    const db::ResultSet row =
        session.run("INSERT INTO problem_params (problem_id, param_idx, name, cpp_type) "
                    "VALUES (?, ?, ?, ?)",
                    {Value::from_i64(outcome.id), Value::from_u64(param.param_idx),
                     Value::from_text(param.name), Value::from_text(param.cpp_type)});
    if (row.failed()) {
      outcome.error = "插入 problem_params 失败：" + row.error().format();
      return outcome;
    }
    outcome.params += 1;
  }

  // ---- 4. 用例行（problem_cases）----------------------------------------------
  for (const model::ProblemCase& test_case : problem.cases) {
    const db::ResultSet row = session.run(
        "INSERT INTO problem_cases (problem_id, case_idx, brief, args_json, expected_json) "
        "VALUES (?, ?, ?, ?, ?)",
        {Value::from_i64(outcome.id), Value::from_u64(test_case.case_idx),
         test_case.brief.empty() ? Value::null() : Value::from_text(test_case.brief),
         Value::from_text(test_case.args_json()), Value::from_text(test_case.expected_json())});
    if (row.failed()) {
      outcome.error = "插入 problem_cases 失败：" + row.error().format();
      return outcome;
    }
    outcome.cases += 1;
  }

  // ---- 5. 标签（tags + problem_tags）------------------------------------------
  for (const model::TagCategory category :
       {model::TagCategory::kAlgorithm, model::TagCategory::kSource}) {
    for (const std::string& name : problem.tags_of(category)) {
      const db::ResultSet tag =
          session.run("INSERT INTO tags (name, category) VALUES (?, ?) "
                      "ON DUPLICATE KEY UPDATE id = LAST_INSERT_ID(id)",
                      {Value::from_text(name), Value::from_text(model::to_string(category))});
      if (tag.failed()) {
        outcome.error = "写入 tags 失败：" + tag.error().format();
        return outcome;
      }
      const db::ResultSet link = session.run(
          "INSERT IGNORE INTO problem_tags (problem_id, tag_id) VALUES (?, ?)",
          {Value::from_i64(outcome.id), Value::from_u64(tag.insert_id())});
      if (link.failed()) {
        outcome.error = "写入 problem_tags 失败：" + link.error().format();
        return outcome;
      }
      outcome.tag_links += static_cast<std::size_t>(link.affected_rows());
    }
  }

  // ---- 6. 回填 case_count（同一事务内，§3.4 的维护契约）------------------------
  const db::ResultSet refreshed =
      session.run("UPDATE problems SET case_count = ? WHERE id = ?",
                  {Value::from_u64(static_cast<unsigned long long>(problem.case_count())),
                   Value::from_i64(outcome.id)});
  if (refreshed.failed()) {
    outcome.error = "回填 case_count 失败：" + refreshed.error().format();
    return outcome;
  }

  outcome.ok = transaction.commit();
  if (!outcome.ok) {
    outcome.error = "提交事务失败：" + session.error().format();
  }
  return outcome;
}

// 校验不通过、但库里有旧行时的处置：清空子表 + case_count 置 0，保留 problems 行。
// 理由见 loader.h【为什么不删"文件已删除"的题】：删 problems 行会 CASCADE 掉用户历史。
// had_row 用来让调用方区分"新题（库里本来就没有）"与"改坏了的旧题（已清空）"——
// 报告里这两个数不是一回事：前者什么也没发生，后者发生了需要人工知道的事。
bool deactivate_problem(Session& session, const std::string& slug, bool* had_row,
                        std::string* error) {
  *had_row = false;
  db::Transaction transaction(session);
  if (!transaction.ok()) {
    *error = "开启事务失败：" + session.error().format();
    return false;
  }
  const db::ResultSet found =
      session.run("SELECT id FROM problems WHERE slug = ?", {Value::from_text(slug)});
  if (found.failed()) {
    *error = "查询 problems 失败：" + found.error().format();
    return false;
  }
  if (found.empty()) {
    transaction.commit();
    return true;  // 从未入库过的新题：什么都不用做
  }
  *had_row = true;

  const std::vector<Value> id_param = {Value::from_i64(found.at(0).i64(0))};
  for (const char* table : {"problem_cases", "problem_params", "problem_tags"}) {
    const db::ResultSet deleted =
        session.run(std::string("DELETE FROM ") + table + " WHERE problem_id = ?", id_param);
    if (deleted.failed()) {
      *error = std::string("清空 ") + table + " 失败：" + deleted.error().format();
      return false;
    }
  }
  const db::ResultSet cleared =
      session.run("UPDATE problems SET case_count = 0, updated_at = NOW() WHERE id = ?", id_param);
  if (cleared.failed()) {
    *error = "置 case_count=0 失败：" + cleared.error().format();
    return false;
  }
  if (!transaction.commit()) {
    *error = "提交事务失败：" + session.error().format();
    return false;
  }
  return true;
}

// ---- 缓存 ---------------------------------------------------------------------
// 只在 load_all() 末尾发布一次；读侧拿 shared_ptr，见 loader.h【缓存为什么是 shared_ptr】。
std::mutex& cache_mutex() {
  static std::mutex mutex;
  return mutex;
}

CachePtr& cache_slot() {
  static CachePtr slot = std::make_shared<const Cache>();
  return slot;
}

}  // namespace

std::string LoadReport::summary() const {
  std::ostringstream out;
  out << "题库同步 " << (ok ? "完成" : "失败") << "（" << dir << "）：扫描 " << files_scanned
      << " 个文件，入库 " << loaded << " 题（用例 " << cases_written << " / 形参 "
      << params_written << " / 标签关联 " << tag_links_written << "），跳过 " << skipped
      << " 题，标记失效 " << deactivated << " 题，写库失败 " << db_failed << " 题";
  if (!problems.empty()) out << "；整体级问题 " << problems.size() << " 条";
  return out.str();
}

std::string LoadReport::format() const {
  std::ostringstream out;
  out << summary() << "\n";
  for (const std::string& problem_text : problems) {
    out << "  · " << problem_text << "\n";
  }
  for (const SkippedProblem& skipped_problem : skipped_problems) {
    out << "  · 跳过 " << skipped_problem.file;
    if (!skipped_problem.slug.empty()) out << "（slug=" << skipped_problem.slug << "）";
    out << "：\n";
    for (const std::string& reason : skipped_problem.reasons) {
      out << "      " << reason << "\n";
    }
  }
  return out.str();
}

LoadReport load_all(const LoadOptions& options) {
  LoadReport report;
  report.dir = options.dir.empty() ? common::config().problems_dir : options.dir;

  std::vector<std::string> files;
  std::string dir_error;
  if (!list_problem_files(report.dir, &files, &dir_error)) {
    report.problems.push_back(dir_error);
    common::log_error(common::LogChannel::kApp, dir_error);
    return report;  // ok 保持 false：题库目录读不到，后面每一步都无从谈起
  }
  report.files_scanned = files.size();

  // 缓存先发布一份空的：万一中途出错，调用方拿到的是一个【合法】的空缓存，
  // 而不是上一轮加载的旧内容。用空缓存表达"没加载成功"优于用旧内容表达 ——
  // 后者会让 judge 拿着一份已经不存在的题库继续判题，而题库与 DB 已经不一致了。
  {
    std::lock_guard<std::mutex> guard(cache_mutex());
    cache_slot() = std::make_shared<const Cache>();
  }

  // 写库模式下先确认表在：把"没建表"与"某条 INSERT 报错"区分开。
  if (options.write_to_db) {
    db::Lease lease = db::Pool::instance().acquire();
    if (!lease.valid()) {
      report.problems.push_back("连接池未就绪，无法同步题库（见上一条 ERROR）");
      common::log_error(common::LogChannel::kApp, report.problems.back());
      return report;
    }
    std::string missing;
    if (!ensure_tables(lease.session(), &missing)) {
      report.problems.push_back("以下表不存在，题库同步中止：" + missing +
                                "（建表是启动第 4 步 T0.3 的职责）");
      common::log_error(common::LogChannel::kApp, report.problems.back());
      return report;
    }
  }

  Cache building;
  for (const std::string& file : files) {
    const std::string path = report.dir + "/" + file;
    const std::string stem = stem_of(file);

    ParseResult parsed = parse_problem_file(path, stem);
    if (!parsed.ok) {
      report.skipped += 1;
      SkippedProblem skipped_problem;
      skipped_problem.file = file;
      skipped_problem.slug = parsed.slug;
      for (const ValidationIssue& issue : parsed.issues) {
        skipped_problem.reasons.push_back(render_issue(issue));
      }
      report.skipped_problems.push_back(skipped_problem);
      // §5.6.1：跳过该题 + ERROR 点名。用 log_text 而非 log_write —— 每个物理行都带
      // 时间戳与等级，50 道题里挑出哪一道有问题时按 [ERROR] grep 就能定位到行。
      common::log_text(common::LogChannel::kApp, common::LogLevel::kError,
                       "题库跳过 " + file + "（" + std::to_string(parsed.issues.size()) +
                           " 条问题，见下方逐条；该题不会入库、不会进缓存，也不会出现在列表页）：\n" +
                           format_issues(parsed.issues, file));

      // 库里有旧行时把它标记为失效（不清空 problems 行 —— 那会 CASCADE 掉用户历史，
      // 见 loader.h【为什么不删"文件已删除"的题】）。slug 读不到时无从判断该动哪一行，
      // 只能跳过：宁可库里留一道"坏题"（它会在列表页显示为 case_count=0 的空题，
      // 用户点进去才发现用例为空），也不能猜一个 slug 去改别人的行。
      if (options.write_to_db && !parsed.slug.empty()) {
        db::Lease lease = db::Pool::instance().acquire();
        bool had_row = false;
        std::string deactivate_error;
        const bool cleaned = lease.valid() &&
                             deactivate_problem(lease.session(), parsed.slug, &had_row,
                                                &deactivate_error);
        if (!cleaned) {
          report.db_failed += 1;
          report.problems.push_back("把 " + file + "（slug=" + parsed.slug +
                                    "）的旧数据标记为失效失败：" +
                                    (lease.valid() ? deactivate_error : std::string("取不到租约")));
        } else if (had_row) {
          report.deactivated += 1;
          common::log_warn(common::LogChannel::kApp,
                           "题库 " + file + " 校验不通过，库中旧行已清空用例并置 case_count=0"
                           "（problems 行保留：它挂着用户的提交历史，删行会级联删掉）");
        }
      }
      continue;
    }

    model::ProblemBundle problem = std::move(parsed.problem);
    if (options.write_to_db) {
      db::Lease lease = db::Pool::instance().acquire();
      if (!lease.valid()) {
        report.problems.push_back("同步 " + file + " 时取不到租约");
        report.db_failed += 1;
        continue;
      }
      const SyncOutcome outcome = sync_problem(lease.session(), problem);
      if (!outcome.ok) {
        report.db_failed += 1;
        report.problems.push_back("同步 " + file + "（slug=" + problem.slug + "）失败：" + outcome.error);
        common::log_error(common::LogChannel::kApp, report.problems.back());
        continue;
      }
      problem.id = outcome.id;
      report.cases_written += outcome.cases;
      report.params_written += outcome.params;
      report.tag_links_written += outcome.tag_links;
    }
    building.push_back(std::move(problem));
    report.loaded += 1;
  }

  {
    std::lock_guard<std::mutex> guard(cache_mutex());
    cache_slot() = std::make_shared<const Cache>(std::move(building));
  }
  report.ok = report.db_failed == 0;

  common::log_info(common::LogChannel::kApp, report.summary());
  if (!report.skipped_problems.empty()) {
    // §5.6.1 明确要求"启动日志汇总所有被跳过的题"。逐条 ERROR 已经在上面打过了，
    // 这里是可 grep 的一行索引，录题的人只看这一行就知道今天要修哪几道。
    std::string names = "题库中被跳过的题（不会出现在列表页，也不会进判题缓存）：";
    for (const SkippedProblem& skipped_problem : report.skipped_problems) {
      names += " " + skipped_problem.file;
    }
    common::log_warn(common::LogChannel::kApp, names);
  }
  for (const std::string& problem_text : report.problems) {
    common::log_error(common::LogChannel::kApp, "题库同步：" + problem_text);
  }
  return report;
}

CachePtr cache() {
  std::lock_guard<std::mutex> guard(cache_mutex());
  return cache_slot();
}

std::shared_ptr<const model::ProblemBundle> find_by_slug(const std::string& slug) {
  const CachePtr snapshot = cache();
  for (const model::ProblemBundle& problem : *snapshot) {
    if (problem.slug == slug) return std::make_shared<const model::ProblemBundle>(problem);
  }
  return nullptr;
}

std::shared_ptr<const model::ProblemBundle> find_by_id(int id) {
  if (id <= 0) return nullptr;
  const CachePtr snapshot = cache();
  for (const model::ProblemBundle& problem : *snapshot) {
    if (problem.id == id) return std::make_shared<const model::ProblemBundle>(problem);
  }
  return nullptr;
}

std::size_t cache_size() {
  return cache()->size();
}

void clear_cache() {
  std::lock_guard<std::mutex> guard(cache_mutex());
  cache_slot() = std::make_shared<const Cache>();
}

}  // namespace problem
}  // namespace oj
