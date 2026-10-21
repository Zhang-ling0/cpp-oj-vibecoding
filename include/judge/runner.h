// 用例沙箱：fork + setrlimit + stall 检测 + 兜底墙钟 + 进程组 kill
#pragma once

#include <string>

namespace oj {
namespace judge {

enum class CaseKillReason {
  kNone,        // 正常退出
  kTle,         // SIGXCPU / stall / 兜底墙钟
  kMle,         // bad_alloc（exit 未到 harness）或启动失败
  kRe,          // 非零退出 / 信号
  kSpawnFailed, // fork/exec 失败（JE 由调用方判）
};

struct RunCaseOptions {
  std::string workspace_dir;
  int case_idx = 0;  // 传给 ./run 的 argv[1]，tester.cpp 按它 switch（§5.3）
  unsigned long long time_limit_ms = 3000;
  unsigned long long mem_limit_kb = 131072;
  long fence_wall_ms = 35000;  // time_limit × fence_multiplier + 5000
};

struct RunCaseResult {
  CaseKillReason kill = CaseKillReason::kNone;
  int exit_code = -1;
  int term_signal = 0;
  bool timed_out = false;       // kill == kTle 时为 true
  bool oom_like = false;        // kill == kMle 时为 true
  std::string stdout_text;      // 用户输出（有上限截断）
  std::string stderr_text;      // 平台 stderr（含 @@OJ@@ 行）
  bool truncated = false;
};

// 执行一次 `./run`（工作区已编译产物）。阻塞到结束或被杀。
RunCaseResult run_case(const RunCaseOptions& options);

}  // namespace judge
}  // namespace oj
