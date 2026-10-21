// 配套 include/web/submission_api.h（口径与取舍写在那里）。本文件只放 SQL 与序列化。

#include "web/submission_api.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/log.h"
#include "db/mysql_pool.h"
#include "judge/scheduler.h"

namespace oj {
namespace web {
namespace {

// ---- 失败处理（与 user_api.cpp / problem_api.cpp 同一约定）------------------------
bool fail(std::string* error, const std::string& what, const db::DbError& db_error) {
  const std::string detail = what + "：" + db_error.format();
  common::log_error(common::LogChannel::kApp, "提交组 API " + detail);
  if (error != nullptr) *error = detail;
  return false;
}

bool fail_text(std::string* error, const std::string& what) {
  common::log_error(common::LogChannel::kApp, "提交组 API " + what);
  if (error != nullptr) *error = what;
  return false;
}

// 空串 ⇒ JSON null（§1.4：库里没有这个东西，不是"有但是空的"）。
// 返回类型必须是 nlohmann::json：返回 std::string 会走隐式转换，
// 而 json→string 的转换对 null 抛 type_error（"type must be string, but is null"）。
nlohmann::json nullable_text(const std::string& value) {
  if (value.empty()) return nlohmann::json(nullptr);
  return nlohmann::json(value);
}

std::string trimmed(const std::string& text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
  return text.substr(begin, end - begin);
}

// 枚举值一律比【大写】（AC/WA/…）。§1.5 只要求大小写不敏感，不要求下发时归一化，
// 故库里存的就是大写，这里也只是为了比较而统一大小写。
std::string uppered(const std::string& text) {
  std::string copy = text;
  std::transform(copy.begin(), copy.end(), copy.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return copy;
}

// 纯数字（§1.5：page=0 / -1 / 1abc / 1.5 一律 1001，不做静默夹取）。
bool parse_positive_int(const std::string& text, long long* out) {
  if (text.empty()) return false;
  long long value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + (c - '0');
    if (value > 1000000000LL) return false;   // 溢出护栏；实际远小于此
  }
  if (value < 1) return false;
  *out = value;
  return true;
}

bool reject(QueryParseError* error, const std::string& field, const std::string& reason) {
  if (error != nullptr) {
    error->field = field;
    error->reason = reason;
  }
  return false;
}

bool valid_verdict(const std::string& upper) {
  return upper == "AC" || upper == "WA" || upper == "TLE" || upper == "MLE" ||
         upper == "RE" || upper == "CE" || upper == "JE";
}

// 把 'a=1&b=2' 摊平成 [(key, value)]。同名参数【最后一个生效】是本函数自带的语义
// （遍历时覆盖前一次），未知的 key 由调用方忽略（§1.5 的两条通用规则）。
std::vector<std::pair<std::string, std::string>> split_query(const std::string& raw_query) {
  std::vector<std::pair<std::string, std::string>> pairs;
  std::size_t cursor = 0;
  while (cursor <= raw_query.size()) {
    const std::size_t amp = raw_query.find('&', cursor);
    const std::string item =
        raw_query.substr(cursor, amp == std::string::npos ? std::string::npos : amp - cursor);
    if (!item.empty()) {
      const std::size_t eq = item.find('=');
      std::string key = (eq == std::string::npos) ? item : item.substr(0, eq);
      std::string value = (eq == std::string::npos) ? std::string() : item.substr(eq + 1);
      // 解码失败（%ZZ 之类）一律丢弃这一项：调用方看到的仍是"没传这个参数"，
      // 而不是把一个被半解码的值当成合法值使出去（problem_api::url_decode 的同一取舍）。
      if (!url_decode(key, &key) || !url_decode(value, &value)) continue;
      pairs.emplace_back(std::move(key), std::move(value));
    }
    if (amp == std::string::npos) break;
    cursor = amp + 1;
  }
  return pairs;
}

// 列表与详情的共用的 SELECT 列（§5.3 的 list[] 与 §5.2 的外层字段同源）。
constexpr const char* kSubmissionColumns =
    "s.id, p.slug, p.title, s.status, s.verdict, s.message,"
    " s.case_passed, s.case_total, s.total_ms, s.max_mem_kb, s.created_at";

void fill_common(const db::Row& row, unsigned long long* id, std::string* slug, std::string* title,
                 std::string* status, std::string* verdict, std::string* message,
                 bool* has_case_passed, long long* case_passed, bool* has_case_total,
                 long long* case_total, bool* has_total_ms, long long* total_ms,
                 bool* has_max_mem_kb, long long* max_mem_kb, std::string* created_at) {
  *id = row.u64(0);
  *slug = row.str(1);
  *title = row.str(2);
  *status = row.str(3);
  *verdict = row.str(4);
  *message = row.str(5);
  // 数值列可为 NULL（还在排队时一条都没有）⇒ 下发 null 而不是 0（§1.4 的空值语义）
  *has_case_passed = !row.is_null(6);
  *case_passed = row.i64(6);
  *has_case_total = !row.is_null(7);
  *case_total = row.i64(7);
  *has_total_ms = !row.is_null(8);
  *total_ms = row.i64(8);
  *has_max_mem_kb = !row.is_null(9);
  *max_mem_kb = row.i64(9);
  *created_at = row.str(10);
}

// 归属判定用的一条极窄查询：这条 id 存在吗？（用于把 1003 与 1004 分开）
bool exists_anywhere(db::Session& session, unsigned long long id, std::string* error) {
  const db::ResultSet result =
      session.run("SELECT id FROM submissions WHERE id = ?", {db::Value::from_u64(id)});
  if (result.failed()) {
    fail(error, "查询提交是否存在失败", result.error());
    return false;
  }
  return !result.empty();
}

}  // namespace

// ---- judge 闸门 ---------------------------------------------------------------------

bool judge_available() {
  // 阶段 1 接线：读调度器存活状态。POST /api/submissions 与 .../retry 都只看这个函数。
  return judge::scheduler().alive();
}

// ---- 查询串解析（§5.3）---------------------------------------------------------------

bool parse_submission_query(const std::string& raw_query, SubmissionQuery* out,
                            QueryParseError* error) {
  SubmissionQuery query;
  const auto pairs = split_query(raw_query);
  for (const auto& pair : pairs) {
    if (pair.first == "problemSlug") {
      query.problem_slug = trimmed(pair.second);
    } else if (pair.first == "verdict") {
      const std::string upper = uppered(trimmed(pair.second));
      if (upper.empty()) continue;                       // 空 = 不按结论过滤
      if (!valid_verdict(upper)) {
        return reject(error, "verdict", "verdict 只能是 AC/WA/TLE/MLE/RE/CE/JE 之一");
      }
      query.verdict = upper;
    } else if (pair.first == "page") {
      long long value = 0;
      if (!parse_positive_int(pair.second, &value)) {
        return reject(error, "page", "page 必须是 ≥ 1 的纯数字");
      }
      query.page = static_cast<int>(value);
    } else if (pair.first == "size") {
      long long value = 0;
      if (!parse_positive_int(pair.second, &value)) {
        return reject(error, "size", "size 必须是 ≥ 1 的纯数字");
      }
      // 超过上限【夹到上限而不是报错】（§1.5），响应的 size 回显实际生效值
      query.size = (value > kMaxPageSize) ? kMaxPageSize : static_cast<int>(value);
    }
    // 未知参数名忽略（§1.5：前端新增筛选不该让旧后端整体 400）
  }
  *out = query;
  return true;
}

// ---- 列表（§5.3）---------------------------------------------------------------------

bool load_submissions(db::Session& session, unsigned long long user_id,
                      const SubmissionQuery& query, SubmissionList* out, std::string* error) {
  if (query.page < 1 || query.size < 1) return fail_text(error, "提交列表：分页参数不合法");

  std::string where = " WHERE s.user_id = ?";
  std::vector<db::Value> params{db::Value::from_u64(user_id)};
  if (!query.problem_slug.empty()) {
    where += " AND p.slug = ?";
    params.push_back(db::Value::from_text(query.problem_slug));
  }
  if (!query.verdict.empty()) {
    where += " AND s.verdict = ?";
    params.push_back(db::Value::from_text(query.verdict));
  }

  // ① 总数：与取页共用 where，保证"总页数"与"翻页结果"同口径
  const db::ResultSet counted =
      session.run("SELECT COUNT(*) FROM submissions s JOIN problems p ON p.id = s.problem_id" + where,
                  params);
  if (counted.failed()) return fail(error, "提交列表：统计总数失败", counted.error());

  SubmissionList list;
  list.total = counted.empty() ? 0 : counted.at(0).i64(0);
  list.size = query.size;
  // 服务端夹页（P1-5）：删完最后一页最后一条后 page 可能 > 真实最大页，
  // 夹回最大页（至少 1），前端跟着回显走，避免空页死局。
  {
    const long long pages = (list.total + list.size - 1) / list.size;
    const long long max_page = pages < 1 ? 1 : pages;
    list.page = (query.page > max_page) ? static_cast<int>(max_page) : query.page;
  }
  std::vector<db::Value> page_params = params;
  page_params.push_back(db::Value::from_i64(list.size));
  page_params.push_back(db::Value::from_i64(static_cast<long long>(list.page - 1) * list.size));
  const db::ResultSet rows = session.run(
      std::string("SELECT ") + kSubmissionColumns +
          " FROM submissions s JOIN problems p ON p.id = s.problem_id" + where +
          " ORDER BY s.id DESC LIMIT ? OFFSET ?",
      page_params);
  if (rows.failed()) return fail(error, "提交列表：取页失败", rows.error());

  for (const db::Row& row : rows.rows()) {
    SubmissionListItem item;
    fill_common(row, &item.id, &item.problem_slug, &item.problem_title, &item.status, &item.verdict,
                &item.message, &item.has_case_passed, &item.case_passed, &item.has_case_total,
                &item.case_total, &item.has_total_ms, &item.total_ms, &item.has_max_mem_kb,
                &item.max_mem_kb, &item.created_at);
    list.list.push_back(std::move(item));
  }
  *out = std::move(list);
  return true;
}

// ---- 详情（§5.2）---------------------------------------------------------------------

bool load_submission_detail(db::Session& session, unsigned long long user_id,
                            unsigned long long id, SubmissionDetail* out, LookupStatus* status,
                            std::string* error) {
  const auto report = [&](LookupStatus value) {
    if (status != nullptr) *status = value;
    return value == LookupStatus::kFound;
  };

  // 归属写进 WHERE（成功路径只有一条查询）
  const db::ResultSet rows = session.run(
      std::string("SELECT ") + kSubmissionColumns + ", s.code, s.compiler_output, p.case_count"
      " FROM submissions s JOIN problems p ON p.id = s.problem_id"
      " WHERE s.id = ? AND s.user_id = ?",
      {db::Value::from_u64(id), db::Value::from_u64(user_id)});
  if (rows.failed()) {
    fail(error, "提交详情查询失败", rows.error());
    return report(LookupStatus::kNotFound);
  }
  if (rows.empty()) {
    // 无行有两种原因（同一句 SQL 的两种结果），失败路径上再查一次区分：
    // 存在于他人名下 → 1003；压根不存在 → 1004。admin 同样走这条（不放开，§5.2）。
    // exists_anywhere 内部已把 DB 报错打进 app.log 并写进 error，这里不再记一条
    std::string inner_error;
    if (!exists_anywhere(session, id, &inner_error)) return report(LookupStatus::kNotFound);
    return report(LookupStatus::kNotOwned);
  }

  const db::Row& row = rows.at(0);
  SubmissionDetail detail;
  fill_common(row, &detail.id, &detail.problem_slug, &detail.problem_title, &detail.status,
              &detail.verdict, &detail.message, &detail.has_case_passed, &detail.case_passed,
              &detail.has_case_total, &detail.case_total, &detail.has_total_ms, &detail.total_ms,
              &detail.has_max_mem_kb, &detail.max_mem_kb, &detail.created_at);
  detail.code = row.str(11);
  detail.compiler_output = row.str(12);

  // caseTotal = GREATEST(实跑明细行数, 题库当前用例数)（[D-65]）：因此 CE 时是题库的用例数
  // 而不是 0 —— 状态行显示「编译错误 · 12 个用例未执行」。库里已有的汇总列在 CE 时是 0，
  // 故这里以题库用例数兜底，而不是原样下发 case_passed/case_total。
  const long long case_total = row.i64(13);
  if (!detail.has_case_total || detail.case_total < case_total) {
    detail.has_case_total = true;
    detail.case_total = case_total;
  }
  detail.can_retry = (detail.status == "failed" && detail.verdict == "JE");

  const db::ResultSet cases = session.run(
      "SELECT case_idx, verdict, passed, args_text, output_text, expected_text,"
      " time_ms, mem_kb, error_msg FROM submission_case_results"
      " WHERE submission_id = ? ORDER BY case_idx ASC",
      {db::Value::from_u64(id)});
  if (cases.failed()) return fail(error, "提交详情：读取逐用例结果失败", cases.error());
  for (const db::Row& item : cases.rows()) {
    CaseResult entry;
    entry.case_idx = static_cast<int>(item.i64(0));
    entry.verdict = item.str(1);
    entry.passed = item.i64(2) != 0;
    entry.args_text = item.str(3);
    entry.output_text = item.str(4);
    entry.expected_text = item.str(5);
    // TLE/MLE 用例的 time_ms / mem_kb 是 NULL（子进程被 SIGKILL，harness 来不及输出度量），
    // 下发 null 而不是 0（§5.2 明写）
    entry.has_time_ms = !item.is_null(6);
    entry.time_ms = item.i64(6);
    entry.has_mem_kb = !item.is_null(7);
    entry.mem_kb = item.i64(7);
    entry.error_msg = item.str(8);
    detail.case_results.push_back(std::move(entry));
  }

  const db::ResultSet diagnostics = session.run(
      "SELECT file_name, line, col, severity, message FROM submission_compile_errors"
      " WHERE submission_id = ? ORDER BY seq ASC",
      {db::Value::from_u64(id)});
  if (diagnostics.failed()) {
    return fail(error, "提交详情：读取编译诊断失败", diagnostics.error());
  }
  for (const db::Row& item : diagnostics.rows()) {
    CompileDiagnostic entry;
    entry.file = item.str(0);
    entry.line = static_cast<int>(item.i64(1));
    entry.col = static_cast<int>(item.i64(2));
    entry.severity = item.str(3);
    entry.message = item.str(4);
    detail.compile_errors.push_back(std::move(entry));
  }

  *out = std::move(detail);
  return report(LookupStatus::kFound);
}

// ---- diff（§5.4）---------------------------------------------------------------------

bool load_submission_diff(db::Session& session, unsigned long long user_id, unsigned long long from_id,
                          unsigned long long to_id, DiffSide* out_from, DiffSide* out_to,
                          LookupStatus* status, std::string* error) {
  const auto report = [&](LookupStatus value) {
    if (status != nullptr) *status = value;
    return value == LookupStatus::kFound;
  };

  // 两次取（from / to 各自一次）：两次都要过归属校验，任一不属本人 → 1003。
  const auto load_one = [&](unsigned long long id, DiffSide* side) -> LookupStatus {
    const db::ResultSet rows =
        session.run("SELECT id, created_at, code FROM submissions WHERE id = ? AND user_id = ?",
                    {db::Value::from_u64(id), db::Value::from_u64(user_id)});
    if (rows.failed()) {
      fail(error, "diff 查询失败", rows.error());
      return LookupStatus::kNotFound;
    }
    if (rows.empty()) {
      std::string inner_error;
      if (!exists_anywhere(session, id, &inner_error)) {
        fail_text(error, inner_error);
        return LookupStatus::kNotFound;
      }
      return LookupStatus::kNotOwned;
    }
    side->id = rows.at(0).u64(0);
    side->created_at = rows.at(0).str(1);
    side->code = rows.at(0).str(2);
    return LookupStatus::kFound;
  };

  const LookupStatus from = load_one(from_id, out_from);
  if (from != LookupStatus::kFound) return report(from);
  const LookupStatus to = load_one(to_id, out_to);
  if (to != LookupStatus::kFound) return report(to);
  return report(LookupStatus::kFound);
}

// ---- 删除（§5.5）-----------------------------------------------------------------------

bool delete_submission(db::Session& session, unsigned long long user_id, unsigned long long id,
                       LookupStatus* status, std::string* error) {
  const auto report = [&](LookupStatus value) {
    if (status != nullptr) *status = value;
    return value == LookupStatus::kFound;
  };

  // 归属写进 WHERE；明细与编译诊断由外键 CASCADE 连带删除（§5.5）
  const db::ResultSet removed = session.run(
      "DELETE FROM submissions WHERE id = ? AND user_id = ?",
      {db::Value::from_u64(id), db::Value::from_u64(user_id)});
  if (removed.failed()) return fail(error, "删除提交失败", removed.error());
  if (removed.affected_rows() > 0) return report(LookupStatus::kFound);

  std::string inner_error;   // DB 报错已由 exists_anywhere 记过日志
  if (!exists_anywhere(session, id, &inner_error)) return report(LookupStatus::kNotFound);
  return report(LookupStatus::kNotOwned);
}

// ---- 新建（§5.1）------------------------------------------------------------------------

bool create_submission(db::Session& session, unsigned long long user_id, const std::string& slug,
                       const std::string& language, const std::string& code, bool create,
                       CreatedSubmission* out, LookupStatus* status, std::string* error) {
  const auto report = [&](LookupStatus value) {
    if (status != nullptr) *status = value;
    return value == LookupStatus::kFound;
  };

  // 归档题不接新提交（阶段 2 下线归档）：与"题不存在"同为 1004 ——
  // 对提交者而言两者的处置一样（回列表），而"这题暂时下线了"不该由提交接口来解释。
  const db::ResultSet problem = session.run("SELECT id FROM problems WHERE slug = ? AND archived = 0",
                                            {db::Value::from_text(slug)});
  if (problem.failed()) {
    fail(error, "提交：查询题目失败", problem.error());
    return report(LookupStatus::kNotFound);
  }
  if (problem.empty()) return report(LookupStatus::kNotFound);   // → 1004
  const int problem_id = static_cast<int>(problem.at(0).i64(0));

  // judge 未接线时只校验 slug 不建行（见头文件 create 参数的说明）
  if (!create) return report(LookupStatus::kFound);

  const db::ResultSet inserted = session.run(
      "INSERT INTO submissions (user_id, problem_id, language, code, status, created_at)"
      " VALUES (?, ?, ?, ?, 'pending', UTC_TIMESTAMP())",
      {db::Value::from_u64(user_id), db::Value::from_i64(problem_id),
       db::Value::from_text(language), db::Value::from_text(code)});
  if (inserted.failed()) return fail(error, "提交：写入 submissions 失败", inserted.error());

  const unsigned long long id = inserted.insert_id();

  // queueAhead：你前面还有几个 pending（§5.1）。⚠️ 它等价于"队列 FIFO 且按 id 升序出队"
  // 这条隐含契约 —— 队列若改成优先级队列，必须改成从调度器读真实位置。
  // 它【只回答自己的问题】，绝不返回全局队列长度（那会泄露其他用户的并发负载，D-60）。
  const db::ResultSet ahead =
      session.run("SELECT COUNT(*) FROM submissions WHERE status = 'pending' AND id < ?",
                  {db::Value::from_u64(id)});
  if (ahead.failed()) return fail(error, "提交：统计 queueAhead 失败", ahead.error());

  out->submission_id = id;
  out->queue_ahead = ahead.empty() ? 0 : ahead.at(0).i64(0);
  return report(LookupStatus::kFound);
}

// ---- 重跑（§5.6）---------------------------------------------------------------------------

bool retry_submission(db::Session& session, unsigned long long user_id, unsigned long long id,
                      bool create, unsigned long long* out_new_id, LookupStatus* status,
                      bool* retryable, std::string* error) {
  const auto report = [&](LookupStatus value) {
    if (status != nullptr) *status = value;
    return value == LookupStatus::kFound;
  };
  *retryable = false;

  const db::ResultSet source = session.run(
      "SELECT problem_id, language, code, status, verdict FROM submissions"
      " WHERE id = ? AND user_id = ?",
      {db::Value::from_u64(id), db::Value::from_u64(user_id)});
  if (source.failed()) {
    fail(error, "重跑：读取原提交失败", source.error());
    return report(LookupStatus::kNotFound);
  }
  if (source.empty()) {
    // exists_anywhere 内部已把 DB 报错打进 app.log 并写进 error，这里不再记一条
    std::string inner_error;
    if (!exists_anywhere(session, id, &inner_error)) return report(LookupStatus::kNotFound);
    return report(LookupStatus::kNotOwned);
  }
  const db::Row& row = source.at(0);
  // 前置条件：status='failed' 且 verdict='JE'（§5.6 表）。其余一律 1011，
  // 错误文案由路由层给（"只有失败的提交可以重跑"）。
  if (row.str(3) != "failed" || row.str(4) != "JE") return report(LookupStatus::kFound);

  // judge 未接线时只校验不建行（见头文件 create 参数的说明）
  *retryable = true;
  if (!create) return report(LookupStatus::kFound);

  // 重跑是【新增一条记录】，不覆盖原记录（§5.6）：覆盖会破坏"提交历史不可变"，
  // 还得先删明细再重写，凭空多一个失败点。与普通提交同一条代码路径，判题侧零改动。
  const db::ResultSet inserted = session.run(
      "INSERT INTO submissions (user_id, problem_id, language, code, status, created_at)"
      " VALUES (?, ?, ?, ?, 'pending', UTC_TIMESTAMP())",
      {db::Value::from_u64(user_id), db::Value::from_i64(row.i64(0)),
       db::Value::from_text(row.str(1)), db::Value::from_text(row.str(2))});
  if (inserted.failed()) return fail(error, "重跑：写入新提交失败", inserted.error());

  *out_new_id = inserted.insert_id();
  *retryable = true;
  return report(LookupStatus::kFound);
}

// ---- 序列化 -----------------------------------------------------------------------------

std::string to_json(const SubmissionList& list) {
  nlohmann::json items = nlohmann::json::array();
  for (const SubmissionListItem& item : list.list) {
    items.push_back({{"id", item.id},
                     {"problemSlug", item.problem_slug},
                     {"problemTitle", item.problem_title},
                     {"status", item.status},
                     {"verdict", nullable_text(item.verdict)},
                     {"casePassed",
                      item.has_case_passed ? nlohmann::json(item.case_passed) : nlohmann::json(nullptr)},
                     {"caseTotal",
                      item.has_case_total ? nlohmann::json(item.case_total) : nlohmann::json(nullptr)},
                     {"totalMs",
                      item.has_total_ms ? nlohmann::json(item.total_ms) : nlohmann::json(nullptr)},
                     {"maxMemKb",
                      item.has_max_mem_kb ? nlohmann::json(item.max_mem_kb) : nlohmann::json(nullptr)},
                     {"createdAt", item.created_at}});
  }
  return nlohmann::json{{"list", std::move(items)},
                        {"total", list.total},
                        {"page", list.page},
                        {"size", list.size}}
      .dump();
}

std::string to_json(const SubmissionDetail& detail) {
  nlohmann::json cases = nlohmann::json::array();
  for (const CaseResult& entry : detail.case_results) {
    cases.push_back({{"caseIdx", entry.case_idx},
                     {"verdict", entry.verdict},
                     {"passed", entry.passed},
                     {"argsText", nullable_text(entry.args_text)},
                     {"outputText", nullable_text(entry.output_text)},
                     {"expectedText", nullable_text(entry.expected_text)},
                     {"timeMs", entry.has_time_ms ? nlohmann::json(entry.time_ms) : nlohmann::json(nullptr)},
                     {"memKb", entry.has_mem_kb ? nlohmann::json(entry.mem_kb) : nlohmann::json(nullptr)},
                     {"errorMsg", nullable_text(entry.error_msg)}});
  }

  nlohmann::json diagnostics = nlohmann::json::array();
  for (const CompileDiagnostic& entry : detail.compile_errors) {
    diagnostics.push_back({{"file", entry.file},
                           {"line", entry.line},
                           {"col", entry.col},
                           {"severity", entry.severity},
                           {"message", entry.message}});
  }

  return nlohmann::json{
             {"id", detail.id},
             {"problemSlug", detail.problem_slug},
             {"problemTitle", detail.problem_title},
             {"status", detail.status},
             {"verdict", nullable_text(detail.verdict)},
             {"message", nullable_text(detail.message)},
             {"casePassed",
              detail.has_case_passed ? nlohmann::json(detail.case_passed) : nlohmann::json(nullptr)},
             {"caseTotal",
              detail.has_case_total ? nlohmann::json(detail.case_total) : nlohmann::json(nullptr)},
             {"totalMs",
              detail.has_total_ms ? nlohmann::json(detail.total_ms) : nlohmann::json(nullptr)},
             {"maxMemKb",
              detail.has_max_mem_kb ? nlohmann::json(detail.max_mem_kb) : nlohmann::json(nullptr)},
             {"code", detail.code},
             {"compilerOutput", nullable_text(detail.compiler_output)},
             {"compileErrors", std::move(diagnostics)},
             {"caseResults", std::move(cases)},
             {"createdAt", detail.created_at},
             {"canRetry", detail.can_retry},
         }
      .dump();
}

std::string to_json(const DiffSide& from, const DiffSide& to) {
  const auto side = [](const DiffSide& value) {
    return nlohmann::json{{"id", value.id}, {"createdAt", value.created_at}, {"code", value.code}};
  };
  // ⚠️ 服务端只返回两段原文，hunks 是前端算出来的（§5.4：为什么不服务端算 diff）
  return nlohmann::json{{"from", side(from)}, {"to", side(to)}}.dump();
}

std::string to_json(const CreatedSubmission& created) {
  return nlohmann::json{{"submissionId", created.submission_id},
                        {"status", "pending"},
                        {"queueAhead", created.queue_ahead}}
      .dump();
}

}  // namespace web
}  // namespace oj