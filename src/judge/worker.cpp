// worker：取题 → 生成 → 编译 → 逐用例 → verdict → 单事务写回
#include "judge/worker.h"

#include <sys/stat.h>

#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "common/config.h"
#include "common/log.h"
#include "db/dao.h"
#include "db/mysql_pool.h"
#include "judge/compiler.h"
#include "judge/generator.h"
#include "judge/runner.h"
#include "judge/scheduler.h"
#include "judge/workspace.h"
#include "model/problem.h"
#include "problem/loader.h"

namespace oj {
namespace judge {
namespace {

// verdict 优先级：JE > TLE > MLE > RE > WA > AC（§5.6.2）
int verdict_rank(const std::string& v) {
  if (v == "JE") return 6;
  if (v == "TLE") return 5;
  if (v == "MLE") return 4;
  if (v == "RE") return 3;
  if (v == "WA") return 2;
  if (v == "AC") return 1;
  return 0;
}

std::string pick_verdict(const std::vector<std::string>& per_case) {
  std::string best = "AC";
  for (const std::string& v : per_case) {
    if (verdict_rank(v) > verdict_rank(best)) best = v;
  }
  return best;
}

// 从 stderr 里解析 @@OJ@@JSON 行（§5.3）。返回解析出的条数。
struct OjLine {
  int idx = -1;
  std::string brief, args, output, expected, verdict, error;
  long long time_ms = 0;
  long long mem_kb = 0;
  bool passed = false;
};

bool parse_oj_line(const std::string& text, OjLine* out) {
  // nlohmann 解析
  try {
    const auto j = nlohmann::json::parse(text);
    out->idx = j.value("idx", -1);
    out->brief = j.value("brief", "");
    out->args = j.value("args", "");
    out->output = j.value("output", "");
    out->expected = j.value("expected", "");
    out->verdict = j.value("verdict", "AC");
    out->error = j.value("error", "");
    out->time_ms = j.value("time_ms", 0);
    out->mem_kb = j.value("mem_kb", 0);
    out->passed = j.value("passed", false);
    return out->idx >= 0;
  } catch (...) {
    return false;
  }
}

std::vector<OjLine> extract_oj_lines(const std::string& stderr_text) {
  std::vector<OjLine> lines;
  std::istringstream is(stderr_text);
  std::string row;
  while (std::getline(is, row)) {
    const auto pos = row.find("@@OJ@@");
    if (pos == std::string::npos) continue;
    OjLine line;
    if (parse_oj_line(row.substr(pos + 6), &line)) lines.push_back(std::move(line));
  }
  return lines;
}

bool write_text_file(const std::string& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  return static_cast<bool>(out);
}

void fail_je(unsigned long long id, const std::string& message) {
  common::log_error(common::LogChannel::kJudge,
                    "提交 " + std::to_string(id) + " JE：" + message);
  db::Lease lease = db::Pool::instance().acquire();
  if (!lease.valid()) return;
  dao::mark_failed_je(lease.session(), id, message);
}

}  // namespace

void judge_one(unsigned long long submission_id, unsigned long long user_id) {
  (void)user_id;
  common::log_info(common::LogChannel::kJudge,
                   "开始评测 submission=" + std::to_string(submission_id));

  dao::PendingSubmission sub;
  {
    // 租约只覆盖读取 + pending→judging：编译/跑用例期间绝不能持有 DB 锁（§2.3）
    db::Lease lease = db::Pool::instance().acquire();
    if (!lease.valid()) {
      fail_je(submission_id, "判题：连接池不可用");
      return;
    }
    if (!dao::load_submission_for_judge(lease.session(), submission_id, &sub)) {
      return;  // 可能已被删除
    }
    if (!dao::mark_judging(lease.session(), submission_id)) {
      return;  // 已不是 pending
    }
  }

  const auto problem = problem::find_by_id(sub.problem_id);
  if (!problem) {
    fail_je(submission_id, "题库缓存缺少该题");
    return;
  }

  const std::string root = workspace_root();
  const CreateResult created = create_workspace(root, submission_id);
  if (!created.ok) {
    fail_je(submission_id, "创建工作区失败：" + created.error);
    return;
  }

  dao::WritebackInput wb;
  wb.submission_id = submission_id;
  wb.problem_id = sub.problem_id;

  const WorkspacePaths paths = workspace_paths(root, submission_id);
  bool ok_pipeline = true;
  do {
    // 1) 生成 harness
    const GenerateResult gen = generate_harness(*problem);
    if (!gen.ok) {
      wb.status = "failed";
      wb.verdict = "JE";
      wb.message = "题库配置有误：" + gen.error;
      break;
    }
    if (!write_text_file(paths.user_source, sub.code) ||
        !write_text_file(paths.solution_decl, gen.solution_decl) ||
        !write_text_file(paths.tester_util, gen.tester_util) ||
        !write_text_file(paths.tester_source, gen.tester_cpp)) {
      wb.status = "failed";
      wb.verdict = "JE";
      wb.message = "写入工作区文件失败";
      break;
    }
    common::log_text(common::LogChannel::kJudge, common::LogLevel::kInfo,
                     "submission=" + std::to_string(submission_id) + " 生成 harness 完成");

    // 2) 编译（内部有编译闸门=1，2 核 2G 安全）
    CompileOptions copt;
    copt.write_judge_log = true;
    const CompileResult comp = compile_submission(paths.dir, copt);
    if (!comp.ok) {
      wb.status = "done";
      wb.verdict = "CE";
      wb.compiler_output = comp.output;
      if (comp.timed_out) {
        wb.message = "编译超时";
      } else if (comp.stage == CompileStage::kHarnessLink) {
        wb.message = "平台测试代码编译失败（题库配置有误）";
      } else if (comp.stage == CompileStage::kUserObject) {
        wb.message = "编译错误";
      } else {
        // stage=kNone 且未成功：工作区缺文件等基础设施问题 → JE
        wb.status = "failed";
        wb.verdict = "JE";
        wb.message = "编译环境异常（平台错误）";
        break;
      }
      for (const CompileDiagnostic& d : comp.diagnostics) {
        dao::DiagnosticRow row;
        row.seq = d.seq;
        row.file_name = d.file_name;
        row.line = d.line;
        row.col = d.col;
        row.severity = to_string(d.severity);
        row.message = d.message;
        wb.diagnostics.push_back(std::move(row));
      }
      break;  // CE 路径结束
    }
    if (!comp.binary_created) {
      wb.status = "failed";
      wb.verdict = "JE";
      wb.message = "编译未产出可执行文件（平台错误）";
      break;
    }

    // 3) 逐用例执行
    const int case_count = static_cast<int>(problem->cases.size());
    std::vector<dao::CaseRow> rows(static_cast<std::size_t>(case_count));
    std::vector<std::string> verdicts(static_cast<std::size_t>(case_count), "AC");
    std::vector<int> first_fail_case;  // 1-based 提示用

    RunCaseOptions ropt;
    ropt.workspace_dir = paths.dir;
    ropt.time_limit_ms = problem->time_limit_ms;
    ropt.mem_limit_kb = problem->mem_limit_kb;
    ropt.fence_wall_ms = common::config().fence_wall_ms(static_cast<long>(problem->time_limit_ms));

    for (int i = 0; i < case_count; ++i) {
      ropt.case_idx = i;
      dao::CaseRow& row = rows[static_cast<std::size_t>(i)];
      row.case_idx = i;
      row.args_text = problem->cases[static_cast<std::size_t>(i)].args_json();
      row.expected_text = problem->cases[static_cast<std::size_t>(i)].expected_json();

      Scheduler::instance().acquire_global_slot();
      const RunCaseResult run = run_case(ropt);
      Scheduler::instance().release_global_slot();

      const std::vector<OjLine> lines = extract_oj_lines(run.stderr_text);
      const OjLine* hit = nullptr;
      for (const OjLine& line : lines) {
        if (line.idx == i) {
          hit = &line;
          break;
        }
      }

      if (hit != nullptr) {
        row.output_text = hit->output;
        if (!hit->args.empty()) row.args_text = hit->args;
        if (!hit->expected.empty()) row.expected_text = hit->expected;
        row.has_time_ms = true;
        row.time_ms = hit->time_ms;
        row.has_mem_kb = true;
        row.mem_kb = hit->mem_kb;
        row.passed = hit->passed;
        row.verdict = hit->passed ? "AC" : "WA";
        row.error_msg = hit->error;
        verdicts[static_cast<std::size_t>(i)] = row.verdict;
        if (!row.passed && first_fail_case.empty()) first_fail_case.push_back(i + 1);
      } else {
        // 无 @@OJ@@：进程被杀或崩溃
        row.passed = false;
        row.has_time_ms = false;
        row.has_mem_kb = false;
        if (run.kill == CaseKillReason::kTle) {
          row.verdict = "TLE";
          row.error_msg = "超出时间限制 (" + std::to_string(problem->time_limit_ms) + "ms)";
          verdicts[static_cast<std::size_t>(i)] = "TLE";
          if (first_fail_case.empty()) first_fail_case.push_back(i + 1);
        } else if (run.kill == CaseKillReason::kMle) {
          row.verdict = "MLE";
          row.error_msg = "超出内存限制 (地址空间 " + std::to_string(problem->mem_limit_kb) + "KB)";
          verdicts[static_cast<std::size_t>(i)] = "MLE";
          if (first_fail_case.empty()) first_fail_case.push_back(i + 1);
        } else {
          // RE 或 spawn 失败
          row.verdict = "RE";
          row.output_text = run.stdout_text.substr(0, 4096);
          row.error_msg = run.term_signal != 0
                              ? "信号终止 sig=" + std::to_string(run.term_signal)
                              : "非零退出 code=" + std::to_string(run.exit_code);
          verdicts[static_cast<std::size_t>(i)] = "RE";
          if (first_fail_case.empty()) first_fail_case.push_back(i + 1);
        }
      }
    }

    // 0 条 @@OJ@@ 且全部用例都无输出 → harness 从未运行 → JE（§5.6.2 第 5 行）
    // 逐用例跑时，每个进程独立；若每个都是 spawn 失败则 JE
    bool any_ran = false;
    for (int i = 0; i < case_count; ++i) {
      if (rows[static_cast<std::size_t>(i)].verdict != "RE" ||
          rows[static_cast<std::size_t>(i)].error_msg.rfind("信号", 0) != 0) {
        // RE 也可能真跑了但崩了；用"是否产出过 @@OJ@@"更准：hit 路径 verdict 是 AC/WA
      }
      if (rows[static_cast<std::size_t>(i)].verdict == "AC" ||
          rows[static_cast<std::size_t>(i)].verdict == "WA") {
        any_ran = true;
      }
    }
    // 全部用例都是 RE 且 exit 127 → JE
    bool all_spawn_fail = case_count > 0;
    for (int i = 0; i < case_count; ++i) {
      const dao::CaseRow& row = rows[static_cast<std::size_t>(i)];
      if (!(row.verdict == "RE" && row.error_msg.rfind("非零退出 code=127", 0) == 0)) {
        all_spawn_fail = false;
        break;
      }
    }
    if (!any_ran && all_spawn_fail) {
      wb.status = "failed";
      wb.verdict = "JE";
      wb.message = "测试程序未能运行（平台错误），请重试或联系管理员";
      // D-67：JE 零明细行
      break;
    }

    wb.status = "done";
    wb.verdict = pick_verdict(verdicts);
    wb.cases = std::move(rows);
    if (wb.verdict == "TLE" && !first_fail_case.empty()) {
      wb.message = "第 " + std::to_string(first_fail_case.front()) + " 个用例超时";
    } else if (wb.verdict == "MLE" && !first_fail_case.empty()) {
      wb.message = "第 " + std::to_string(first_fail_case.front()) + " 个用例超出内存限制";
    } else if (wb.verdict == "RE" && !first_fail_case.empty()) {
      wb.message = "第 " + std::to_string(first_fail_case.front()) + " 个用例运行错误";
    } else if (wb.verdict == "WA" && !first_fail_case.empty()) {
      wb.message = "第 " + std::to_string(first_fail_case.front()) + " 个用例答案错误";
    } else if (wb.verdict == "AC") {
      wb.message = "";
    }
    (void)ok_pipeline;
  } while (false);

  std::string write_error;
  if (!dao::write_result(wb, &write_error)) {
    common::log_error(common::LogChannel::kJudge,
                      "写回失败 submission=" + std::to_string(submission_id) + "：" + write_error);
    db::Lease l2 = db::Pool::instance().acquire();
    if (l2.valid()) {
      dao::mark_failed_je(l2.session(), submission_id, "写回失败（平台错误）");
    }
  } else {
    common::log_info(common::LogChannel::kJudge,
                     "完成 submission=" + std::to_string(submission_id) +
                         " status=" + wb.status + " verdict=" +
                         (wb.verdict.empty() ? "NULL" : wb.verdict));
  }

  std::string rm_error;
  remove_workspace(root, paths.dir, &rm_error);
}

}  // namespace judge
}  // namespace oj
