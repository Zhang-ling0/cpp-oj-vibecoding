// Judge DAO：启动恢复 + 单事务写回（submissions / case_results / compile_errors）
#pragma once

#include <string>
#include <vector>

namespace oj {
namespace db {
class Session;
}
namespace judge {
namespace dao {

// 启动恢复 §5.7 [D-20]：judging → failed/JE。返回受影响行数；失败返回 -1。
long recover_stale_judging();

// 全部 pending 的 id（id 升序）。失败返回空且 *ok=false。
std::vector<unsigned long long> load_pending_ids(bool* ok);

struct PendingSubmission {
  unsigned long long id = 0;
  unsigned long long user_id = 0;
  int problem_id = 0;
  std::string language;
  std::string code;
};

// 读一条待判提交；不存在或失败返回 false。
bool load_submission_for_judge(db::Session& session, unsigned long long id, PendingSubmission* out);

// pending → judging。已不是 pending 时返回 false（幂等防护）。
bool mark_judging(db::Session& session, unsigned long long id);

struct CaseRow {
  int case_idx = 0;
  std::string verdict;  // AC/WA/TLE/MLE/RE
  bool passed = false;
  std::string args_text;
  std::string output_text;
  std::string expected_text;
  bool has_time_ms = false;
  long long time_ms = 0;
  bool has_mem_kb = false;
  long long mem_kb = 0;
  std::string error_msg;
};

struct DiagnosticRow {
  unsigned int seq = 0;
  std::string file_name;
  unsigned int line = 0;
  unsigned int col = 0;
  std::string severity;
  std::string message;
};

struct WritebackInput {
  unsigned long long submission_id = 0;
  int problem_id = 0;
  std::string status;      // done | failed
  std::string verdict;     // 空 = NULL
  std::string message;     // 空 = NULL
  std::string compiler_output;  // 空 = NULL
  std::vector<CaseRow> cases;
  std::vector<DiagnosticRow> diagnostics;
};

// 单事务写回。聚合列按 §3.4：case_passed / case_total=GREATEST / total_ms / max_mem_kb。
bool write_result(const WritebackInput& in, std::string* error);

// 失败兜底：status='failed', verdict='JE'（不写明细，D-67）。
bool mark_failed_je(db::Session& session, unsigned long long id, const std::string& message);

}  // namespace dao
}  // namespace judge
}  // namespace oj
