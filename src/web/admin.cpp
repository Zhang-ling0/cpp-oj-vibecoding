// =============================================================================
// 只读管理端点的实现（include/web/admin.h 的配套）
// 对应任务：SPEC.md §9 M3 T3.12 `[D-37]`；契约见 §6.3.3，口径见 §6.4 / §6.5
//
// 【为什么聚合放在【一条 SQL】里而不是八条】
//   §1.4 的性能要求与 §6.5 的"全部聚合在 SQL 内完成"是同一条判断：
//   串行化 DB（[D-21]）把最坏延迟从"最慢的那条查询"放大成"所有查询之和"，
//   八个标量子查询合成一条只多一次往返，而分成八条就是八次。
//   标量子查询（SELECT (SELECT COUNT(*) FROM ...) AS x, ...）不产生 N+1：
//   每个子查询与主查询无关，MySQL 各扫一遍自己的表并把结果当作常量。
//
// 【users[] 为什么【不是】一条 per-user 的相关子查询风暴】
//   solved / attempted / favorites 三项都可以写成 `WHERE user_id=u.id` 的标量子查询，
//   那样最直白，但代价是【每行用户】都要扫一遍 submissions —— 50 个用户就是 50 次扫描。
//   改为「先把 submissions / user_problems 各自 GROUP BY user_id 收敛成一张小表，
//   再 LEFT JOIN users」：两张聚合表的大小等于【有提交的 / 有收藏的用户数】，
//   与用户数无关。这是 §6.5「先 DISTINCT 收敛再 JOIN」的同一手法，只是这里收敛的维度是
//   user_id 而不是 problem_id。
//
// 【last_verdict 为什么不能用 GROUP_CONCAT(verdict ORDER BY id DESC)】
//   verdict 可为 NULL（提交还在 pending/judging 时，§3.2 DDL 允许）。而 GROUP_CONCAT
//   【跳过 NULL】—— 于是"最新一条还没出结论"的行会安静地返回上一条 AC，看起来像刚答对。
//   这类"静默失真"比报错难查得多，故这一列单独用标量子查询取，NULL 就是 NULL。
//
// 【lastSubmitAt 用 MAX(created_at) 而不是按 id 排序的标量子查询】
//   两者在本模型下必然一致（id 自增与 created_at 单调），但 MAX 能走 idx_user_time，
//   标量子查询只能走 idx_user_time + filesort。用便宜的。
// =============================================================================
#include "web/admin.h"

#include <nlohmann/json.hpp>

#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

#include "common/config.h"
#include "common/log.h"
#include "db/mysql_pool.h"
#include "problem/json_schema.h"
#include "problem/loader.h"

namespace oj {
namespace web {
namespace {

// 失败时统一的动作：把 DB 错误原文打进 app.log（服务端 errno/sqlstate 一并带上，
// 排查时不必再去翻 app.log），并把同一句话交给调用方回给上层。
bool fail(std::string* error, const std::string& what, const db::DbError& db_error) {
  const std::string detail = what + "：" + db_error.format();
  common::log_error(common::LogChannel::kApp, "管理端点 " + detail);
  if (error != nullptr) *error = detail;
  return false;
}

// 与 submission_api.cpp 的同名函数同一约定：不需要 DB 错误原文的失败也走同一条出口。
bool fail_text(std::string* error, const std::string& what) {
  common::log_error(common::LogChannel::kApp, "管理端点 " + what);
  if (error != nullptr) *error = what;
  return false;
}

long long as_count(const db::Row& row, std::size_t col) {
  return static_cast<long long>(row.u64(col, 0));
}

// 空串 / NULL → JSON null。两者在前端含义不同（"从未提交" vs "提交了但结论未知"），
// 序列化成 "" 会让前端把后者误判成前者。
nlohmann::json nullable_text(const std::string& value) {
  if (value.empty()) return nlohmann::json(nullptr);
  return nlohmann::json(value);
}

}  // namespace

// ---- 守卫 ---------------------------------------------------------------------

GuardResult require_admin(const std::optional<AuthUser>& user) {
  // 先判登录，再判角色（§6.3.3）：顺序反了会让"没登录"被报成"无权限"。
  if (!user.has_value()) return GuardResult::kUnauthenticated;
  if (!user->is_admin()) return GuardResult::kForbidden;
  return GuardResult::kAllowed;
}

int error_code_of(GuardResult result) {
  switch (result) {
    case GuardResult::kAllowed:
      return 0;
    case GuardResult::kUnauthenticated:
      return 1002;
    case GuardResult::kForbidden:
      return 1003;
  }
  return 1008;  // 不可达；真到了这里说明枚举被改过，宁可回 1008 也不回 0
}

const char* error_message_of(GuardResult result) {
  switch (result) {
    case GuardResult::kAllowed:
      return "ok";
    case GuardResult::kUnauthenticated:
      return "未登录或会话已过期";
    case GuardResult::kForbidden:
      return "无权限";
  }
  return "服务器内部错误";
}

// ---- 判题运行态 ---------------------------------------------------------------

JudgeRuntimeStatus unwired_status() {
  JudgeRuntimeStatus status;   // 三项的默认值就是"0 / 0 / 未存活"
  status.judge_alive = false;
  return status;
}

StatusProvider unwired_status_provider() { return []() { return unwired_status(); }; }

// ---- 聚合 ---------------------------------------------------------------------

bool load_system_counts(db::Session& session, SystemBlock* out, std::string* error) {
  // 无参数（不按用户过滤），因此 SQL 里没有任何 `?` —— 这是"全站统计"而不是
  // "某人的统计"，差别就在这里：T3.12 与 §6.5 的用户级聚合不是同一组查询。
  // 用 raw literal 而不是逐行字符串拼接：相邻字面量拼接【不会】自动补空格，
  // 少一个空格就是一条 1064 语法错误，而这种错误只在真库上才暴露（单测跑不到）。
  static const char* kSql = R"SQL(
SELECT
  (SELECT COUNT(*) FROM users)                                  AS user_count,
  (SELECT COUNT(*) FROM users WHERE role = 'admin')             AS admin_count,
  (SELECT COUNT(*) FROM problems)                               AS problem_count,
  (SELECT COUNT(*) FROM problem_cases)                          AS case_count,
  (SELECT COUNT(*) FROM submissions)                            AS submission_count,
  (SELECT COUNT(*) FROM submissions WHERE verdict = 'AC')       AS ac_count,
  (SELECT COUNT(*) FROM submissions WHERE status = 'failed')    AS failed_count,
  -- judging_count 的口径见 SystemBlock 的注释；改成只数 judging 只需改这一行
  (SELECT COUNT(*) FROM submissions
    WHERE status IN ('pending','judging'))                      AS judging_count
)SQL";

  const db::ResultSet result = session.run(kSql);
  if (result.failed()) return fail(error, "统计 system 失败", result.error());
  if (result.empty()) {
    const std::string detail = "统计 system 失败：查询返回 0 行（聚合查询本应恰好 1 行）";
    common::log_error(common::LogChannel::kApp, "管理端点 " + detail);
    if (error != nullptr) *error = detail;
    return false;
  }

  const db::Row& row = result.at(0);
  SystemBlock counts;   // 先写局部变量，成功后再整体赋给 *out（不变式：不失败就改动 out）
  counts.user_count = as_count(row, 0);
  counts.admin_count = as_count(row, 1);
  counts.problem_count = as_count(row, 2);
  counts.case_count = as_count(row, 3);
  counts.submission_count = as_count(row, 4);
  counts.ac_count = as_count(row, 5);
  counts.failed_count = as_count(row, 6);
  counts.judging_count = as_count(row, 7);
  *out = counts;
  return true;
}

bool load_users(db::Session& session, std::vector<AdminUserRow>* out, std::string* error) {
  // solved / attempted 的口径（§6.4 + §6.5 的口径一致性契约）：
  //   solved    = 该用户有过至少一条 verdict='AC' 的题数（COUNT DISTINCT problem_id）
  //   attempted = 该用户有过至少一条【任意状态】提交的题数
  // 两者【不是互斥的】：一道 AC 过的题同时计入两者。这不是笔误 —— §6.5 给出的
  // byDifficulty / byTag 聚合用的正是这一对写法（at 子查询只按 user_id 过滤，
  // 不排除 AC），管理页若另立一套互斥算法，同一个用户会在两个页面上显示不同的进度。
  // 列表页的【单题】进度仍走 §6.4 的 CASE（AC 优先，否则 attempted）—— 那是另一个粒度。
  static const char* kSql = R"SQL(
SELECT u.id, u.username, u.role, u.created_at,
       COALESCE(s.n, 0)          AS submission_count,
       COALESCE(s.ac, 0)         AS ac_count,
       COALESCE(s.solved, 0)     AS solved,
       COALESCE(s.attempted, 0)  AS attempted,
       COALESCE(f.favorites, 0)  AS favorites,
       s.last_submit_at,
       -- 见文件头：不能用 GROUP_CONCAT，它会跳过 NULL 的 verdict
        (SELECT s2.verdict FROM submissions s2
          WHERE s2.user_id = u.id ORDER BY s2.id DESC LIMIT 1) AS last_verdict,
        u.disabled
FROM users u
LEFT JOIN (
  SELECT user_id,
         COUNT(*) AS n,
         SUM(verdict = 'AC') AS ac,
         COUNT(DISTINCT CASE WHEN verdict = 'AC' THEN problem_id END) AS solved,
         COUNT(DISTINCT problem_id) AS attempted,
         MAX(created_at) AS last_submit_at
  FROM submissions GROUP BY user_id
) s ON s.user_id = u.id
LEFT JOIN (
  SELECT user_id, COUNT(*) AS favorites
  FROM user_problems WHERE favorite = 1 GROUP BY user_id
) f ON f.user_id = u.id
ORDER BY u.id
)SQL";

  const db::ResultSet result = session.run(kSql);
  if (result.failed()) return fail(error, "统计 users[] 失败", result.error());

  std::vector<AdminUserRow> rows;
  rows.reserve(result.size());
  for (const db::Row& row : result.rows()) {
    AdminUserRow entry;
    entry.username = row.str(1);
    entry.role = row.str(2);
    entry.created_at = row.str(3);
    entry.submission_count = as_count(row, 4);
    entry.ac_count = as_count(row, 5);
    entry.solved = as_count(row, 6);
    entry.attempted = as_count(row, 7);
    entry.favorites = as_count(row, 8);
    entry.last_submit_at = row.str(9);
    // verdict 可为 NULL（提交未出结论），此时 str() 返回空串 → to_json 里序列化为 null
    entry.last_verdict = row.str(10);
    entry.disabled = row.boolean(11);
    rows.push_back(std::move(entry));
  }
  *out = std::move(rows);
  return true;
}

bool build_overview(db::Session& session, const JudgeRuntimeStatus& status,
                    Overview* out, std::string* error) {
  Overview overview;
  if (!load_system_counts(session, &overview.system, error)) return false;
  if (!load_users(session, &overview.users, error)) return false;
  // 运行态三项在 DB 之后写入：此刻 provider 的锁早已释放（调用方的快照是值拷贝），
  // 不存在"持 DB 锁去读调度器"的可能。
  overview.system.queue_length = status.queue_length;
  overview.system.active_judges = status.active_judges;
  overview.system.judge_alive = status.judge_alive;
  *out = std::move(overview);
  return true;
}

bool build_overview(const StatusProvider& provider, Overview* out, std::string* error) {
  // 锁顺序落地处（§6.3.3 实现约束 2）：【先快照，后租约】。
  // 顺序若反过来，多出一条 DB mutex → scheduler lock 的嵌套边。
  const JudgeRuntimeStatus status = provider ? provider() : unwired_status();
  // 快照此刻已是值，不再持有调度器的锁。
  db::Lease lease = db::Pool::instance().acquire();
  if (!lease.valid()) {
    const std::string detail = "读取管理端点数据失败：连接池未就绪，取不到租约（见 app.log）";
    common::log_error(common::LogChannel::kApp, "管理端点 " + detail);
    if (error != nullptr) *error = detail;
    return false;
  }
  return build_overview(lease.session(), status, out, error);
}

// ---- 序列化 -------------------------------------------------------------------

std::string to_json(const Overview& overview) {
  nlohmann::json system = {
      {"userCount", overview.system.user_count},
      {"adminCount", overview.system.admin_count},
      {"problemCount", overview.system.problem_count},
      {"caseCount", overview.system.case_count},
      {"submissionCount", overview.system.submission_count},
      {"acCount", overview.system.ac_count},
      {"failedCount", overview.system.failed_count},
      {"judgingCount", overview.system.judging_count},
      {"queueLength", overview.system.queue_length},
      {"activeJudges", overview.system.active_judges},
      {"judgeAlive", overview.system.judge_alive},
  };

  nlohmann::json users = nlohmann::json::array();
  for (const AdminUserRow& row : overview.users) {
    users.push_back({
        {"username", row.username},
        {"role", row.role},
        {"createdAt", row.created_at},
        {"submissionCount", row.submission_count},
        {"acCount", row.ac_count},
        {"solved", row.solved},
        {"attempted", row.attempted},
        {"favorites", row.favorites},
        {"lastSubmitAt", nullable_text(row.last_submit_at)},
        {"lastVerdict", nullable_text(row.last_verdict)},
        {"disabled", row.disabled},
    });
  }

  // 只序列化 data 块：{code, message, data} 包络由 common::response.h（T0.4）负责，
  // 在那里写第二遍就成了 §9 T3.5 明确禁止的"两套包络"。
  return nlohmann::json{{"system", std::move(system)}, {"users", std::move(users)}}.dump();
}

// =============================================================================
// 阶段 2 · 管理端写操作（D-37 变更，见 admin.h 对应段落的说明）
// =============================================================================

namespace {

// ---- 题目 JSON 文件（D-30：文件是唯一真源）------------------------------------

std::string problem_path(const std::string& slug) {
  return common::config().problems_dir + "/" + slug + ".json";
}

// slug 的路径安全校验。json_schema 也会校验 slug 的值域，但那一层的职责是"题库格式对不对"，
// 这一层的职责是"这个字符串能不能当文件名拼进路径" —— 少了它，slug=../../etc/passwd
// 会先于任何校验被拼进路径。宁可两层各管一件事。
bool path_safe_slug(const std::string& slug) {
  if (slug.empty() || slug.size() > 64) return false;
  if (slug.front() == '.') return false;   // 以 . 开头会与隐藏临时文件规则混淆
  for (const char c : slug) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!ok) return false;                 // '/'、'.'、空格、'..' 全部在此被挡下
  }
  return true;
}

bool read_text_file(const std::string& path, std::string* out, std::string* error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    if (error != nullptr) *error = "读文件失败：" + path;
    return false;
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  *out = buffer.str();
  return true;
}

bool write_text_file(const std::string& path, const std::string& text, std::string* error) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    if (error != nullptr) *error = "写文件失败：" + path;
    return false;
  }
  file << text;
  file.close();
  if (!file) {
    if (error != nullptr) *error = "写文件未完整落盘：" + path;
    return false;
  }
  return true;
}

// 把 DB 行的"是否归档"与文件内容拼成编辑器回显的 JSON：
// 文件是真源，archived 是 DB 侧标记（文件里没有这个字段），两者拼在一起回给前端。
// 编辑提交时前端只回传 json 部分，标记不受影响 —— 这正是"归档不属于题面内容"的落法。

}  // namespace

bool load_admin_problems(db::Session& session, std::vector<AdminProblemRow>* out,
                         std::string* error) {
  // 归档题【也列出来】：管理台要能把题恢复上架，藏起来等于把归档做成了删除。
  static const char* kSql =
      "SELECT id, slug, title, difficulty, case_count, is_featured, archived, sort_no,"
      " updated_at FROM problems ORDER BY sort_no ASC, id ASC";
  const db::ResultSet result = session.run(kSql);
  if (result.failed()) return fail(error, "题目列表查询失败", result.error());

  std::vector<AdminProblemRow> rows;
  rows.reserve(result.size());
  for (const db::Row& row : result.rows()) {
    AdminProblemRow entry;
    entry.id = static_cast<int>(row.i64(0));
    entry.slug = row.str(1);
    entry.title = row.str(2);
    entry.difficulty = row.str(3);
    entry.case_count = row.i64(4);
    entry.is_featured = row.i64(5) != 0;
    entry.archived = row.i64(6) != 0;
    entry.sort_no = static_cast<int>(row.i64(7));
    entry.updated_at = row.str(8);
    rows.push_back(std::move(entry));
  }
  *out = std::move(rows);
  return true;
}

ProblemWriteStatus read_problem_json(const std::string& slug, std::string* out,
                                     std::string* error) {
  if (!path_safe_slug(slug)) return ProblemWriteStatus::kMissing;   // 形状非法 ⇒ 按不存在处理
  const std::string path = problem_path(slug);
  if (::access(path.c_str(), F_OK) != 0) return ProblemWriteStatus::kMissing;
  if (!read_text_file(path, out, error)) {
    common::log_error(common::LogChannel::kApp, "管理端点 " + *error);
    return ProblemWriteStatus::kIo;
  }
  return ProblemWriteStatus::kOk;
}

ProblemWriteStatus write_problem_json(const std::string& slug, const std::string& text,
                                      bool require_existing, std::string* error) {
  if (!path_safe_slug(slug)) {
    if (error != nullptr) *error = "slug 不合法：只允许字母、数字、- 和 _（≤ 64 字符）";
    return ProblemWriteStatus::kInvalid;
  }
  const std::string path = problem_path(slug);
  const bool exists = ::access(path.c_str(), F_OK) == 0;
  if (require_existing && !exists) return ProblemWriteStatus::kMissing;
  if (!require_existing && exists) return ProblemWriteStatus::kConflict;

  // ① 先校验、后落盘：校验不过时磁盘上的旧题【一个字节都没动】。
  //    expect_slug = 路径 slug：编辑时 JSON 里的 slug 与路径不一致会被当场点名。
  problem::ParseOptions options;
  options.source = path;
  options.expect_slug = slug;
  const problem::ParseResult parsed = problem::parse_problem_json(text, options);
  if (!parsed.ok) {
    if (error != nullptr) {
      *error = problem::format_issues(parsed.issues, path);
    }
    return ProblemWriteStatus::kInvalid;
  }

  // ② 写入前留一份旧内容用于回滚：写库失败时文件必须回到写入前的状态，
  //    否则"文件是真源"就出现了一个 DB 与文件都半新半旧的中间态。
  std::string previous;
  if (exists && !read_text_file(path, &previous, error)) {
    common::log_error(common::LogChannel::kApp, "管理端点 " + *error);
    return ProblemWriteStatus::kIo;
  }

  // ③ 原子替换：先写隐藏临时文件（不以 .json 结尾 → 题库加载器的枚举不会扫到它），
  //    再 rename。写到一半的文件被启动进程读走，比"写失败"更难查。
  const std::string temp = common::config().problems_dir + "/." + slug + ".json.tmp";
  std::string io_error;
  if (!write_text_file(temp, text, &io_error) || ::rename(temp.c_str(), path.c_str()) != 0) {
    if (io_error.empty()) io_error = std::string("原子替换失败：") + path + "（" +
                                     std::strerror(errno) + "）";
    ::remove(temp.c_str());
    if (error != nullptr) *error = io_error;
    common::log_error(common::LogChannel::kApp, "管理端点 写回题库失败：" + io_error);
    return ProblemWriteStatus::kIo;
  }

  // ④ 文件已就位 → 同步入库 + 就地刷新只读缓存（不重启）。失败则回滚文件。
  std::string sync_error;
  if (!problem::sync_one(parsed.problem, &sync_error)) {
    const char* action = exists ? "回滚" : "删除";
    if (exists) {
      std::string rollback_error;
      if (!write_text_file(path, previous, &rollback_error)) {
        common::log_error(common::LogChannel::kApp,
                          "管理端点 写库失败后回滚也失败：" + rollback_error +
                              "（文件是新内容、DB 是旧内容，重启后 load_all 会重放文件）");
      }
    } else {
      ::remove(path.c_str());
    }
    common::log_error(common::LogChannel::kApp,
                      std::string("管理端点 写库失败，已") + action + "文件：" + sync_error);
    if (error != nullptr) *error = sync_error;
    return ProblemWriteStatus::kIo;
  }
  return ProblemWriteStatus::kOk;
}

bool set_problem_archived(db::Session& session, const std::string& slug, bool archived,
                          bool* found, std::string* error) {
  *found = false;
  const db::ResultSet existing =
      session.run("SELECT id FROM problems WHERE slug = ?", {db::Value::from_text(slug)});
  if (existing.failed()) return fail(error, "归档：查询题目失败", existing.error());
  if (existing.empty()) return true;   // found 保持 false → 1004
  *found = true;
  // 只动 archived 一个标记列：题面业务列一律经文件写回（D-30），这条 UPDATE 是唯一例外，
  // 理由见 admin.h【阶段 2】段落。affected_rows 为 0 可能是"值没变"，故不作存在性判据。
  const db::ResultSet updated = session.run(
      "UPDATE problems SET archived = ?, updated_at = NOW() WHERE slug = ?",
      {db::Value::from_bool(archived), db::Value::from_text(slug)});
  if (updated.failed()) return fail(error, "归档：更新标记失败", updated.error());
  return true;
}

bool load_admin_submissions(db::Session& session, int page, int size,
                            AdminSubmissionPage* out, std::string* error) {
  if (page < 1 || size < 1) return fail_text(error, "提交管理：分页参数不合法");
  if (size > 100) size = 100;   // 与 §1.5 的列表上限同值：超过夹到上限而不是报错

  const db::ResultSet counted = session.run("SELECT COUNT(*) FROM submissions");
  if (counted.failed()) return fail(error, "提交管理：统计总数失败", counted.error());

  AdminSubmissionPage result;
  result.total = counted.empty() ? 0 : counted.at(0).i64(0);
  result.size = size;
  // 服务端夹页（与 P1-5 同一条处置）：删完最后一页最后一条后 page 可能越界
  {
    const long long pages = (result.total + result.size - 1) / result.size;
    const long long max_page = pages < 1 ? 1 : pages;
    result.page = (page > max_page) ? static_cast<int>(max_page) : page;
  }

  const db::ResultSet rows = session.run(
      "SELECT s.id, u.username, p.slug, p.title, s.language, s.status, s.verdict, s.created_at"
      " FROM submissions s"
      " JOIN users u ON u.id = s.user_id"
      " JOIN problems p ON p.id = s.problem_id"
      " ORDER BY s.id DESC LIMIT ? OFFSET ?",
      {db::Value::from_i64(result.size),
       db::Value::from_i64(static_cast<long long>(result.page - 1) * result.size)});
  if (rows.failed()) return fail(error, "提交管理：取页失败", rows.error());

  for (const db::Row& row : rows.rows()) {
    AdminSubmissionRow entry;
    entry.id = row.u64(0);
    entry.username = row.str(1);
    entry.problem_slug = row.str(2);
    entry.problem_title = row.str(3);
    entry.language = row.str(4);
    entry.status = row.str(5);
    entry.verdict = row.str(6);   // NULL → 空串 → 序列化为 null
    entry.created_at = row.str(7);
    result.list.push_back(std::move(entry));
  }
  *out = std::move(result);
  return true;
}

namespace {

// 删/重判共用的前置检查：存在吗？在评测中吗？（kNotFound / kBusy / kOk）
AdminOpStatus check_submission_state(db::Session& session, unsigned long long id,
                                     std::string* error, unsigned long long* out_user_id) {
  const db::ResultSet row = session.run(
      "SELECT user_id, status FROM submissions WHERE id = ?", {db::Value::from_u64(id)});
  if (row.failed()) {
    fail(error, "提交管理：查询提交失败", row.error());
    return AdminOpStatus::kError;
  }
  if (row.empty()) return AdminOpStatus::kNotFound;
  if (out_user_id != nullptr) *out_user_id = row.at(0).u64(0);
  const std::string status = row.at(0).str(1);
  // 正在评测时删/重判会与 worker 的写回撞车（写回 UPDATE 0 行 + 明细 FK 报错），
  // 一律 1011 让调用方"等它结束再来"，而不是放行后产生一条 JE。
  return (status == "pending" || status == "judging") ? AdminOpStatus::kBusy : AdminOpStatus::kOk;
}

}  // namespace

AdminOpStatus admin_delete_submission(db::Session& session, unsigned long long id,
                                      std::string* error) {
  const AdminOpStatus state = check_submission_state(session, id, error, nullptr);
  if (state != AdminOpStatus::kOk) return state;
  const db::ResultSet removed =
      session.run("DELETE FROM submissions WHERE id = ?", {db::Value::from_u64(id)});
  if (removed.failed()) {
    fail(error, "提交管理：删除提交失败", removed.error());
    return AdminOpStatus::kError;
  }
  return AdminOpStatus::kOk;
}

AdminOpStatus admin_rejudge_submission(db::Session& session, unsigned long long id,
                                       unsigned long long* out_user_id, std::string* error) {
  unsigned long long owner = 0;
  const AdminOpStatus state = check_submission_state(session, id, error, &owner);
  if (state != AdminOpStatus::kOk) return state;

  db::Transaction txn(session);
  if (!txn.ok()) {
    if (error != nullptr) *error = "重判：开启事务失败：" + session.error().format();
    return AdminOpStatus::kError;
  }
  // 明细与编译诊断不 CASCADE 到 submissions（只有反方向的外键），故显式删除 ——
  // 否则新结论会与旧行并存，详情页会看到两套逐用例结果。
  for (const char* table : {"submission_case_results", "submission_compile_errors"}) {
    const db::ResultSet cleared =
        session.run(std::string("DELETE FROM ") + table + " WHERE submission_id = ?",
                    {db::Value::from_u64(id)});
    if (cleared.failed()) {
      if (error != nullptr) {
        *error = std::string("重判：清空 ") + table + " 失败：" + cleared.error().format();
      }
      return AdminOpStatus::kError;
    }
  }
  const db::ResultSet reset = session.run(
      "UPDATE submissions SET status = 'pending', verdict = NULL, message = NULL,"
      " compiler_output = NULL, case_passed = NULL, case_total = NULL, total_ms = NULL,"
      " max_mem_kb = NULL, finished_at = NULL WHERE id = ?",
      {db::Value::from_u64(id)});
  if (reset.failed()) {
    if (error != nullptr) *error = "重判：重置提交失败：" + reset.error().format();
    return AdminOpStatus::kError;
  }
  if (!txn.commit()) {
    if (error != nullptr) *error = "重判：提交事务失败：" + session.error().format();
    return AdminOpStatus::kError;
  }
  if (out_user_id != nullptr) *out_user_id = owner;
  return AdminOpStatus::kOk;
}

AdminOpStatus admin_set_user_role(db::Session& session, const std::string& username,
                                  const std::string& role, std::string* error) {
  if (role != "admin" && role != "user") {
    if (error != nullptr) *error = "role 只能是 admin 或 user";
    return AdminOpStatus::kError;
  }
  const db::ResultSet existing =
      session.run("SELECT role FROM users WHERE username = ?", {db::Value::from_text(username)});
  if (existing.failed()) {
    fail(error, "用户管理：查询用户失败", existing.error());
    return AdminOpStatus::kError;
  }
  if (existing.empty()) return AdminOpStatus::kNotFound;
  if (existing.at(0).str(0) == role) return AdminOpStatus::kOk;   // 幂等（affected_rows=0 不可作判据）

  const db::ResultSet updated =
      session.run("UPDATE users SET role = ? WHERE username = ?",
                  {db::Value::from_text(role), db::Value::from_text(username)});
  if (updated.failed()) {
    fail(error, "用户管理：更新角色失败", updated.error());
    return AdminOpStatus::kError;
  }
  common::log_warn(common::LogChannel::kApp,
                   "管理端点：角色变更 " + username + " → " + role +
                       "（注意 .env 的 OJ_ADMIN_USERNAMES 在下次启动时会重新同步，D-37）");
  return AdminOpStatus::kOk;
}

AdminOpStatus admin_set_user_disabled(db::Session& session, const std::string& username,
                                      bool disabled, std::string* error) {
  const db::ResultSet existing =
      session.run("SELECT id, disabled FROM users WHERE username = ?",
                  {db::Value::from_text(username)});
  if (existing.failed()) {
    fail(error, "用户管理：查询用户失败", existing.error());
    return AdminOpStatus::kError;
  }
  if (existing.empty()) return AdminOpStatus::kNotFound;
  const unsigned long long user_id = existing.at(0).u64(0);
  if ((existing.at(0).i64(1) != 0) == disabled) return AdminOpStatus::kOk;   // 幂等

  db::Transaction txn(session);
  if (!txn.ok()) {
    if (error != nullptr) *error = "用户管理：开启事务失败：" + session.error().format();
    return AdminOpStatus::kError;
  }
  const db::ResultSet updated =
      session.run("UPDATE users SET disabled = ? WHERE id = ?",
                  {db::Value::from_bool(disabled), db::Value::from_u64(user_id)});
  if (updated.failed()) {
    if (error != nullptr) *error = "用户管理：更新停用标记失败：" + updated.error().format();
    return AdminOpStatus::kError;
  }
  if (disabled) {
    // 立即下线：只改标记的话，已登录的会话还能继续用到过期（§6.3.2 的 7 天）。
    const db::ResultSet sessions_killed =
        session.run("DELETE FROM sessions WHERE user_id = ?", {db::Value::from_u64(user_id)});
    if (sessions_killed.failed()) {
      if (error != nullptr) {
        *error = "用户管理：清空会话失败：" + sessions_killed.error().format();
      }
      return AdminOpStatus::kError;
    }
  }
  if (!txn.commit()) {
    if (error != nullptr) *error = "用户管理：提交事务失败：" + session.error().format();
    return AdminOpStatus::kError;
  }
  common::log_warn(common::LogChannel::kApp,
                   "管理端点：" + username + (disabled ? " 已停用（会话已清空）" : " 已恢复"));
  return AdminOpStatus::kOk;
}

// ---- 序列化（阶段 2 新增结构）-------------------------------------------------

std::string to_json(const std::vector<AdminProblemRow>& problems) {
  nlohmann::json items = nlohmann::json::array();
  for (const AdminProblemRow& row : problems) {
    items.push_back({{"id", row.id},
                     {"slug", row.slug},
                     {"title", row.title},
                     {"difficulty", row.difficulty},
                     {"caseCount", row.case_count},
                     {"isFeatured", row.is_featured},
                     {"archived", row.archived},
                     {"sortNo", row.sort_no},
                     {"updatedAt", row.updated_at}});
  }
  return nlohmann::json{{"problems", std::move(items)}}.dump();
}

std::string to_json(const AdminSubmissionPage& page) {
  nlohmann::json items = nlohmann::json::array();
  for (const AdminSubmissionRow& row : page.list) {
    items.push_back({{"id", row.id},
                     {"username", row.username},
                     {"problemSlug", row.problem_slug},
                     {"problemTitle", row.problem_title},
                     {"language", row.language},
                     {"status", row.status},
                     {"verdict", nullable_text(row.verdict)},
                     {"createdAt", row.created_at}});
  }
  return nlohmann::json{{"list", std::move(items)},
                        {"total", page.total},
                        {"page", page.page},
                        {"size", page.size}}
      .dump();
}

}  // namespace web
}  // namespace oj