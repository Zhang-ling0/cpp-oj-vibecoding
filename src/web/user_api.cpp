// 配套 include/web/user_api.h（口径与取舍写在那里）。本文件只放 SQL 与序列化。

#include "web/user_api.h"

#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/log.h"
#include "db/mysql_pool.h"
#include "web/bcrypt.h"
#include "web/session.h"   // destroy_others：踢掉其他设备的会话（与本模块同事务）

namespace oj {
namespace web {
namespace {

// ---- 失败处理（与 problem_api.cpp / admin.cpp 同一约定）-------------------------
bool fail(std::string* error, const std::string& what, const db::DbError& db_error) {
  const std::string detail = what + "：" + db_error.format();
  common::log_error(common::LogChannel::kApp, "账号/个人数据 API " + detail);
  if (error != nullptr) *error = detail;
  return false;
}

bool fail_text(std::string* error, const std::string& what) {
  common::log_error(common::LogChannel::kApp, "账号/个人数据 API " + what);
  if (error != nullptr) *error = what;
  return false;
}

// 1001 的字段级失败（field 是前端把红字挂到哪个输入框下的依据，§6.3）。
bool fail_field(AuthFailure* out_failure, const char* field, const std::string& reason,
                std::string* error) {
  if (out_failure != nullptr) {
    out_failure->error = common::ApiError::kInvalidArgument;
    out_failure->field.field = field;
    out_failure->field.reason = reason;
    out_failure->server_side = false;
  }
  if (error != nullptr) *error = std::string(field) + " " + reason;
  return false;
}

// 读出该用户的 bcrypt 哈希。查无此行不该发生（会话行 FK 到 users），仍按失败处理。
bool load_password_hash(db::Session& session, unsigned long long user_id, std::string* out_hash,
                        std::string* error) {
  const db::ResultSet result =
      session.run("SELECT password_hash FROM users WHERE id = ?",
                  {db::Value::from_u64(user_id)});
  if (result.failed()) return fail(error, "读取用户口令哈希失败", result.error());
  if (result.empty()) return fail_text(error, "会话对应的用户不存在（sessions 有孤儿行）");
  *out_hash = result.at(0).str(0);
  return true;
}

// 空串 ⇒ JSON null（§1.4：库里没有这个东西，不是"有但是空的"）。
// 返回类型必须是 nlohmann::json：返回 std::string 会走隐式转换，
// 而 json→string 的转换对 null 抛 type_error（"type must be string, but is null"）。
nlohmann::json nullable_text(const std::string& value) {
  if (value.empty()) return nlohmann::json(nullptr);
  return nlohmann::json(value);
}

}  // namespace

// ---- POST /api/auth/password ----------------------------------------------------

bool change_password(db::Session& session, unsigned long long user_id,
                     const std::string& keep_token, const std::string& old_password,
                     const std::string& new_password, AuthFailure* out_failure,
                     std::string* error) {
  // ① 新口令先校验：它是纯格式问题，先答掉省掉一次 bcrypt。
  //    复用 auth_api::validate_password（§5.8 的 6–64 字节与非全空白），
  //    字段名改成 §6.3.5 的 newPassword，避免本模块与注册端点各留一份长度规则。
  const std::optional<FieldError> invalid = validate_password(new_password);
  if (invalid.has_value()) {
    return fail_field(out_failure, "newPassword", invalid->reason, error);
  }

  // ② 验旧口令。bcrypt 只在这里跑一次（cost=10，本机 60–100ms）。
  std::string hash;
  if (!load_password_hash(session, user_id, &hash, error)) return false;
  if (!verify_password(old_password, hash)) {
    return fail_field(out_failure, "oldPassword", "原密码不正确", error);
  }

  // ③ 同事务：改哈希 + 踢掉其他设备。顺序无关（都是同一 user_id 的行），
  //    但 commit 前的任何失败都由 Transaction 的析构回滚，不会出现"只改了一半"。
  db::Transaction transaction(session);
  if (!transaction.ok()) return fail_text(error, "改密失败：事务无法开启");

  const std::string fresh = hash_password(new_password);
  const db::ResultSet updated = session.run(
      "UPDATE users SET password_hash = ? WHERE id = ?",
      {db::Value::from_text(fresh), db::Value::from_u64(user_id)});
  if (updated.failed()) return fail(error, "更新口令哈希失败", updated.error());

  std::string kick_error;
  if (!destroy_others(session, user_id, keep_token, nullptr, &kick_error)) {
    return fail_text(error, "踢掉其他设备会话失败：" + kick_error);
  }
  if (!transaction.commit()) return fail_text(error, "改密事务提交失败");
  return true;
}

// ---- DELETE /api/auth/account ----------------------------------------------------

bool delete_account(db::Session& session, unsigned long long user_id, const std::string& password,
                    AuthFailure* out_failure, std::string* error) {
  std::string hash;
  if (!load_password_hash(session, user_id, &hash, error)) return false;
  if (!verify_password(password, hash)) {
    return fail_field(out_failure, "password", "密码不正确", error);
  }

  // 一条 DELETE 完成整条 CASCADE 链（§6.3.7 约束 1）：sessions / submissions /
  // user_problems → submission_case_results / submission_compile_errors 全部连带删除。
  // 题库表不动（全局资源，约束 2）。
  const db::ResultSet deleted =
      session.run("DELETE FROM users WHERE id = ?", {db::Value::from_u64(user_id)});
  if (deleted.failed()) return fail(error, "注销账号失败", deleted.error());
  if (deleted.affected_rows() == 0) return fail_text(error, "注销账号失败：用户行已不存在");
  return true;
}

// ---- POST /api/problems/{slug}/favorite -------------------------------------------

bool toggle_favorite(db::Session& session, unsigned long long user_id, const std::string& slug,
                     bool* out_is_favorite, FavoriteStatus* out_status, std::string* error) {
  const auto report = [&](FavoriteStatus value) {
    if (out_status != nullptr) *out_status = value;
    return value == FavoriteStatus::kOk;
  };

  const db::ResultSet problem = session.run("SELECT id FROM problems WHERE slug = ?",
                                            {db::Value::from_text(slug)});
  if (problem.failed()) {
    fail(error, "收藏：查询题目失败", problem.error());
    return report(FavoriteStatus::kError);
  }
  if (problem.empty()) return report(FavoriteStatus::kNotFound);   // → 1004
  const int problem_id = static_cast<int>(problem.at(0).i64(0));

  db::Transaction transaction(session);
  if (!transaction.ok()) {
    fail_text(error, "收藏：事务无法开启");
    return report(FavoriteStatus::kError);
  }

  const db::ResultSet existing = session.run(
      "SELECT favorite FROM user_problems WHERE user_id = ? AND problem_id = ?",
      {db::Value::from_u64(user_id), db::Value::from_i64(problem_id)});
  if (existing.failed()) {
    fail(error, "收藏：读取现有收藏状态失败", existing.error());
    return report(FavoriteStatus::kError);
  }

  bool is_favorite = true;   // 三种行状态的默认目标：无行 / favorite=0 → 收藏
  if (existing.empty()) {
    const db::ResultSet inserted = session.run(
        "INSERT INTO user_problems (user_id, problem_id, favorite, updated_at)"
        " VALUES (?, ?, 1, UTC_TIMESTAMP())",
        {db::Value::from_u64(user_id), db::Value::from_i64(problem_id)});
    if (inserted.failed()) {
      fail(error, "收藏：写入 user_problems 失败", inserted.error());
      return report(FavoriteStatus::kError);
    }
  } else if (existing.at(0).i64(0) != 0) {
    is_favorite = false;     // 已收藏 → 本次取消
    const db::ResultSet removed = session.run(
        "DELETE FROM user_problems WHERE user_id = ? AND problem_id = ?",
        {db::Value::from_u64(user_id), db::Value::from_i64(problem_id)});
    if (removed.failed()) {
      fail(error, "收藏：删除 user_problems 行失败", removed.error());
      return report(FavoriteStatus::kError);
    }
  } else {
    const db::ResultSet updated = session.run(
        "UPDATE user_problems SET favorite = 1, updated_at = UTC_TIMESTAMP()"
        " WHERE user_id = ? AND problem_id = ?",
        {db::Value::from_u64(user_id), db::Value::from_i64(problem_id)});
    if (updated.failed()) {
      fail(error, "收藏：更新 favorite 失败", updated.error());
      return report(FavoriteStatus::kError);
    }
  }

  if (!transaction.commit()) {
    fail_text(error, "收藏：事务提交失败");
    return report(FavoriteStatus::kError);
  }
  *out_is_favorite = is_favorite;
  return report(FavoriteStatus::kOk);
}

// ---- GET /api/stats ---------------------------------------------------------------

bool load_user_stats(db::Session& session, unsigned long long user_id, UserStats* out,
                     std::string* error) {
  // 四个标量合成【一条】SQL 的标量子查询：[D-21] 串行化 DB 下四次往返就是四次停顿，
  // 而这个端点的存在意义就是"只要一个数字"（§6.1）。
  // solved/attempted 都是 COUNT(DISTINCT problem_id)：题数，不是提交条数（§6.1 字段表）。
  static const char* kSql =
      "SELECT"
      " (SELECT COUNT(*) FROM submissions WHERE user_id = ?) AS total,"
      " (SELECT COUNT(DISTINCT problem_id) FROM submissions"
      "   WHERE user_id = ? AND verdict = 'AC') AS solved,"
      " (SELECT COUNT(DISTINCT problem_id) FROM submissions WHERE user_id = ?) AS attempted,"
      " (SELECT COUNT(*) FROM user_problems WHERE user_id = ? AND favorite = 1) AS favorites";
  const db::ResultSet result =
      session.run(kSql, {db::Value::from_u64(user_id), db::Value::from_u64(user_id),
                         db::Value::from_u64(user_id), db::Value::from_u64(user_id)});
  if (result.failed()) return fail(error, "读取个人统计失败", result.error());
  if (result.empty()) return fail_text(error, "读取个人统计失败：结果集为空");

  const db::Row& row = result.at(0);
  out->total = row.i64(0);
  out->solved = row.i64(1);
  out->attempted = row.i64(2);
  out->favorites = row.i64(3);
  return true;
}

// ---- GET /api/me/overview ----------------------------------------------------------

bool load_user_overview(db::Session& session, unsigned long long user_id, UserOverview* out,
                        std::string* error) {
  UserOverview result;

  // ① totals：题目总数与用户无关，另两条按 §6.5 的"存在至少一条"口径。
  {
    static const char* kSql =
        "SELECT"
        " (SELECT COUNT(*) FROM problems) AS problem_total,"
        " (SELECT COUNT(*) FROM submissions WHERE user_id = ?) AS submissions,"
        " (SELECT COUNT(DISTINCT problem_id) FROM submissions"
        "   WHERE user_id = ? AND verdict = 'AC') AS solved,"
        " (SELECT COUNT(DISTINCT problem_id) FROM submissions WHERE user_id = ?) AS attempted,"
        " (SELECT COUNT(*) FROM user_problems WHERE user_id = ? AND favorite = 1) AS favorites";
    const db::ResultSet rows =
        session.run(kSql, {db::Value::from_u64(user_id), db::Value::from_u64(user_id),
                           db::Value::from_u64(user_id), db::Value::from_u64(user_id)});
    if (rows.failed()) return fail(error, "概览：读取 totals 失败", rows.error());
    if (rows.empty()) return fail_text(error, "概览：读取 totals 失败：结果集为空");
    const db::Row& row = rows.at(0);
    result.total = row.i64(0);
    result.submissions = row.i64(1);
    result.solved = row.i64(2);
    result.attempted = row.i64(3);
    result.favorites = row.i64(4);
  }

  // ② byDifficulty：基表是 problems，【不 JOIN 标签表】故 COUNT(*) 即可。
  //    三条难度固定按 easy → medium → hard 输出（前端概览条按这个顺序画），
  //    库里没有的难度补 0 条，保证前端不必处理"少一条"的情况。
  {
    static const char* kSql =
        "SELECT p.difficulty AS difficulty, COUNT(*) AS total,"
        " SUM(EXISTS(SELECT 1 FROM submissions s"
        "              WHERE s.user_id = ? AND s.problem_id = p.id AND s.verdict = 'AC')) AS solved,"
        " SUM(EXISTS(SELECT 1 FROM submissions s"
        "              WHERE s.user_id = ? AND s.problem_id = p.id)) AS attempted"
        " FROM problems p GROUP BY p.difficulty"
        " ORDER BY FIELD(p.difficulty, 'easy', 'medium', 'hard')";
    const db::ResultSet rows =
        session.run(kSql, {db::Value::from_u64(user_id), db::Value::from_u64(user_id)});
    if (rows.failed()) return fail(error, "概览：读取 byDifficulty 失败", rows.error());
    // 固定三条、顺序固定：按索引落位再按序输出，避免"库里缺 medium 时把 hard 顶到中间"
    // 这种顺序漂移（前端概览条的列序直接由这个数组决定）。
    static const char* kOrder[3] = {"easy", "medium", "hard"};
    DifficultyStat slot[3];
    for (int i = 0; i < 3; ++i) slot[i].difficulty = kOrder[i];
    for (const db::Row& row : rows.rows()) {
      const std::string name = row.str(0);
      int index = (name == "easy") ? 0 : (name == "medium" ? 1 : (name == "hard" ? 2 : -1));
      if (index < 0) continue;        // ENUM 之外的脏数据不影响其它两条
      slot[index].total = row.i64(1);
      slot[index].solved = row.i64(2);
      slot[index].attempted = row.i64(3);
    }
    for (const DifficultyStat& stat : slot) result.by_difficulty.push_back(stat);
  }

  // ③ byTag：【头号陷阱】problem_tags 是多对多，直接 JOIN + COUNT(*) 会把一道挂 3 个
  //    标签的题数三次。这里用 IN 子查询先 DISTINCT 收敛成集合再参与 SUM，行数不变。
  {
    static const char* kSql =
        "SELECT t.name AS name, t.category AS category, COUNT(DISTINCT pt.problem_id) AS total,"
        " SUM(pt.problem_id IN (SELECT DISTINCT problem_id FROM submissions"
        "                      WHERE user_id = ? AND verdict = 'AC')) AS solved,"
        " SUM(pt.problem_id IN (SELECT DISTINCT problem_id FROM submissions"
        "                      WHERE user_id = ?)) AS attempted"
        " FROM problem_tags pt JOIN tags t ON t.id = pt.tag_id"
        " GROUP BY t.id, t.name, t.category"
        " ORDER BY total DESC, t.name ASC";
    const db::ResultSet rows =
        session.run(kSql, {db::Value::from_u64(user_id), db::Value::from_u64(user_id)});
    if (rows.failed()) return fail(error, "概览：读取 byTag 失败", rows.error());
    for (const db::Row& row : rows.rows()) {
      TagStatRow stat;
      stat.name = row.str(0);
      stat.category = row.str(1);
      stat.total = row.i64(2);
      stat.solved = row.i64(3);
      stat.attempted = row.i64(4);
      result.by_tag.push_back(std::move(stat));
    }
  }

  // ④ recent：最近 3 条（§6.2），按 id 倒序 —— 与 §6.3 提交列表的默认排序一致。
  {
    static const char* kSql =
        "SELECT s.id, p.slug, p.title, s.verdict, s.created_at"
        " FROM submissions s JOIN problems p ON p.id = s.problem_id"
        " WHERE s.user_id = ? ORDER BY s.id DESC LIMIT 3";
    const db::ResultSet rows = session.run(kSql, {db::Value::from_u64(user_id)});
    if (rows.failed()) return fail(error, "概览：读取 recent 失败", rows.error());
    for (const db::Row& row : rows.rows()) {
      RecentSubmissionRow entry;
      entry.id = row.u64(0);
      entry.problem_slug = row.str(1);
      entry.problem_title = row.str(2);
      entry.verdict = row.str(3);
      entry.created_at = row.str(4);
      result.recent.push_back(std::move(entry));
    }
  }

  *out = std::move(result);
  return true;
}

// ---- 序列化 -----------------------------------------------------------------------

std::string to_json(const UserStats& stats) {
  return nlohmann::json{
             {"total", stats.total},
             {"solved", stats.solved},
             {"attempted", stats.attempted},
             {"favorites", stats.favorites},
         }
      .dump();
}

std::string to_json(const UserOverview& overview) {
  nlohmann::json by_difficulty = nlohmann::json::array();
  for (const DifficultyStat& stat : overview.by_difficulty) {
    by_difficulty.push_back({{"difficulty", stat.difficulty},
                             {"total", stat.total},
                             {"solved", stat.solved},
                             {"attempted", stat.attempted}});
  }

  nlohmann::json by_tag = nlohmann::json::array();
  for (const TagStatRow& stat : overview.by_tag) {
    by_tag.push_back({{"name", stat.name},
                      {"category", stat.category},
                      {"total", stat.total},
                      {"solved", stat.solved},
                      {"attempted", stat.attempted}});
  }

  nlohmann::json recent = nlohmann::json::array();
  for (const RecentSubmissionRow& entry : overview.recent) {
    recent.push_back({{"id", entry.id},
                      {"problemSlug", entry.problem_slug},
                      {"problemTitle", entry.problem_title},
                      {"verdict", nullable_text(entry.verdict)},
                      {"createdAt", entry.created_at}});
  }

  return nlohmann::json{
             {"totals",
              {{"solved", overview.solved},
               {"total", overview.total},
               {"attempted", overview.attempted},
               {"submissions", overview.submissions},
               {"favorites", overview.favorites}}},
             {"byDifficulty", std::move(by_difficulty)},
             {"byTag", std::move(by_tag)},
             {"recent", std::move(recent)},
         }
      .dump();
}

}  // namespace web
}  // namespace oj