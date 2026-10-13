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

#include <string>
#include <utility>

#include "common/log.h"
#include "db/mysql_pool.h"

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
         WHERE s2.user_id = u.id ORDER BY s2.id DESC LIMIT 1) AS last_verdict
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
    });
  }

  // 只序列化 data 块：{code, message, data} 包络由 common::response.h（T0.4）负责，
  // 在那里写第二遍就成了 §9 T3.5 明确禁止的"两套包络"。
  return nlohmann::json{{"system", std::move(system)}, {"users", std::move(users)}}.dump();
}

}  // namespace web
}  // namespace oj