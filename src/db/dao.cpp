// DAO 实现：启动恢复 + 单事务写回（T2.11 / §5.7 / §3.4）
#include "db/dao.h"

#include <algorithm>

#include "common/log.h"
#include "db/mysql_pool.h"

namespace oj {
namespace judge {
namespace dao {

long recover_stale_judging() {
  db::Lease lease = db::Pool::instance().acquire();
  if (!lease.valid()) return -1;
  const db::ResultSet r = lease->run(
      "UPDATE submissions SET status='failed', verdict='JE',"
      " message='服务重启导致评测中断，请重新提交', finished_at=UTC_TIMESTAMP()"
      " WHERE status='judging'");
  if (r.failed()) {
    common::log_error(common::LogChannel::kJudge, "启动恢复 UPDATE 失败：" + r.error().format());
    return -1;
  }
  return static_cast<long>(r.affected_rows());
}

std::vector<unsigned long long> load_pending_ids(bool* ok) {
  if (ok) *ok = false;
  std::vector<unsigned long long> ids;
  db::Lease lease = db::Pool::instance().acquire();
  if (!lease.valid()) return ids;
  const db::ResultSet r = lease->run(
      "SELECT id FROM submissions WHERE status='pending' ORDER BY id ASC");
  if (r.failed()) {
    common::log_error(common::LogChannel::kJudge, "启动恢复读 pending 失败：" + r.error().format());
    return ids;
  }
  ids.reserve(r.size());
  for (const db::Row& row : r.rows()) ids.push_back(row.u64(0));
  if (ok) *ok = true;
  return ids;
}

bool load_submission_for_judge(db::Session& session, unsigned long long id, PendingSubmission* out) {
  const db::ResultSet r = session.run(
      "SELECT id, user_id, problem_id, language, code FROM submissions WHERE id = ?",
      {db::Value::from_u64(id)});
  if (r.failed() || r.empty()) return false;
  const db::Row& row = r.at(0);
  out->id = row.u64(0);
  out->user_id = row.u64(1);
  out->problem_id = static_cast<int>(row.i64(2));
  out->language = row.str(3);
  out->code = row.str(4);
  return true;
}

bool mark_judging(db::Session& session, unsigned long long id) {
  const db::ResultSet r = session.run(
      "UPDATE submissions SET status='judging' WHERE id = ? AND status='pending'",
      {db::Value::from_u64(id)});
  if (r.failed()) return false;
  return r.affected_rows() > 0;
}

bool write_result(const WritebackInput& in, std::string* error) {
  db::Lease lease = db::Pool::instance().acquire();
  if (!lease.valid()) {
    if (error) *error = "连接池不可用";
    return false;
  }
  db::Session& s = lease.session();

  // 题库当前用例数（GREATEST 的第二元，§5.6.3 [D-65]）
  long long problem_cases = 0;
  {
    const db::ResultSet pc = s.run("SELECT case_count FROM problems WHERE id = ?",
                                   {db::Value::from_i64(in.problem_id)});
    if (pc.failed()) {
      if (error) *error = "读取 problem.case_count 失败：" + pc.error().format();
      return false;
    }
    if (!pc.empty()) problem_cases = pc.at(0).i64(0);
  }

  long long passed = 0;
  long long total_ms = 0;
  long long max_mem = 0;
  for (const CaseRow& c : in.cases) {
    if (c.passed) ++passed;
    if (c.has_time_ms) total_ms += c.time_ms;
    if (c.has_mem_kb) max_mem = std::max(max_mem, c.mem_kb);
  }
  const long long total = std::max(static_cast<long long>(in.cases.size()), problem_cases);

  db::Transaction txn(s);
  if (!txn.ok()) {
    if (error) *error = "BEGIN 失败";
    return false;
  }

  const db::ResultSet upd = s.run(
      "UPDATE submissions SET status=?, verdict=?, message=?, compiler_output=?,"
      " case_passed=?, case_total=?, total_ms=?, max_mem_kb=?, finished_at=UTC_TIMESTAMP()"
      " WHERE id = ?",
      {db::Value::from_text(in.status),
       in.verdict.empty() ? db::Value::null() : db::Value::from_text(in.verdict),
       in.message.empty() ? db::Value::null() : db::Value::from_text(in.message),
       in.compiler_output.empty() ? db::Value::null() : db::Value::from_text(in.compiler_output),
       db::Value::from_i64(passed), db::Value::from_i64(total),
       db::Value::from_i64(total_ms), db::Value::from_i64(max_mem),
       db::Value::from_u64(in.submission_id)});
  if (upd.failed()) {
    txn.rollback();
    if (error) *error = "UPDATE submissions 失败：" + upd.error().format();
    return false;
  }

  for (const CaseRow& c : in.cases) {
    const db::ResultSet ir = s.run(
        "INSERT INTO submission_case_results"
        " (submission_id, case_idx, verdict, passed, args_text, output_text, expected_text,"
        "  time_ms, mem_kb, error_msg)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        {db::Value::from_u64(in.submission_id), db::Value::from_i64(c.case_idx),
         db::Value::from_text(c.verdict), db::Value::from_bool(c.passed),
         c.args_text.empty() ? db::Value::null() : db::Value::from_text(c.args_text),
         c.output_text.empty() ? db::Value::null() : db::Value::from_text(c.output_text),
         c.expected_text.empty() ? db::Value::null() : db::Value::from_text(c.expected_text),
         c.has_time_ms ? db::Value::from_i64(c.time_ms) : db::Value::null(),
         c.has_mem_kb ? db::Value::from_i64(c.mem_kb) : db::Value::null(),
         c.error_msg.empty() ? db::Value::null() : db::Value::from_text(c.error_msg)});
    if (ir.failed()) {
      txn.rollback();
      if (error) *error = "INSERT case_results 失败：" + ir.error().format();
      return false;
    }
  }

  for (const DiagnosticRow& d : in.diagnostics) {
    const db::ResultSet ir = s.run(
        "INSERT INTO submission_compile_errors"
        " (submission_id, seq, file_name, line, col, severity, message)"
        " VALUES (?, ?, ?, ?, ?, ?, ?)",
        {db::Value::from_u64(in.submission_id), db::Value::from_u64(d.seq),
         db::Value::from_text(d.file_name), db::Value::from_u64(d.line),
         db::Value::from_u64(d.col), db::Value::from_text(d.severity),
         db::Value::from_text(d.message)});
    if (ir.failed()) {
      txn.rollback();
      if (error) *error = "INSERT compile_errors 失败：" + ir.error().format();
      return false;
    }
  }

  if (!txn.commit()) {
    if (error) *error = "COMMIT 失败";
    return false;
  }
  return true;
}

bool mark_failed_je(db::Session& session, unsigned long long id, const std::string& message) {
  const db::ResultSet r = session.run(
      "UPDATE submissions SET status='failed', verdict='JE', message=?,"
      " finished_at=UTC_TIMESTAMP() WHERE id = ?",
      {db::Value::from_text(message), db::Value::from_u64(id)});
  return !r.failed();
}

}  // namespace dao
}  // namespace judge
}  // namespace oj
