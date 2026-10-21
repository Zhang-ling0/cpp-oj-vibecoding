// 用例沙箱：fork + 五档 rlimit + CPU stall 检测 + 兜底墙钟 + 进程组 kill（§5.4）
#include "judge/runner.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/log.h"
#include "judge/compiler.h"  // kRunnerBinary 文件名常量

namespace oj {
namespace judge {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kMaxCapture = 256u * 1024u;  // 输出上限，防内存打爆
constexpr int kStallPolls = 3;
constexpr int kStallIntervalMs = 500;

long long wall_ms_since(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

// 子进程 CPU 时间（utime+stime，单位 ms）。读 /proc/<pid>/stat。
bool read_child_cpu_ms(pid_t pid, long long* out_ms) {
  char path[64];
  std::snprintf(path, sizeof path, "/proc/%d/stat", static_cast<int>(pid));
  std::FILE* f = std::fopen(path, "r");
  if (!f) return false;
  int c = std::fgetc(f);
  if (c != '(') { std::fclose(f); return false; }
  while (c != EOF && c != ')') c = std::fgetc(f);
  char state = 0;
  long ppid = 0, pgrp = 0, session = 0, tty = 0, tpgid = 0, flags = 0;
  long minflt = 0, cminflt = 0, majflt = 0, cmajflt = 0, utime = 0, stime = 0;
  const int n = std::fscanf(
      f, " %c %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld", &state, &ppid, &pgrp,
      &session, &tty, &tpgid, &flags, &minflt, &cminflt, &majflt, &cmajflt, &utime, &stime);
  std::fclose(f);
  if (n < 13) return false;
  const long hz = ::sysconf(_SC_CLK_TCK);
  if (hz <= 0) return false;
  *out_ms = (static_cast<long long>(utime) + static_cast<long long>(stime)) * 1000 / hz;
  return true;
}

void kill_group(pid_t pgid) {
  if (::kill(-pgid, SIGKILL) != 0 && errno == ESRCH) {
    ::kill(pgid, SIGKILL);
  }
}

// 读管道直到 EOF 或达上限；非阻塞轮询以便做 stall / 墙钟。
struct Capture {
  std::string out;
  std::string err;
  bool truncated = false;
};

bool drain_fd(int fd, std::string* buf, bool* truncated) {
  char tmp[4096];
  for (;;) {
    const ssize_t n = ::read(fd, tmp, sizeof tmp);
    if (n > 0) {
      if (buf->size() + static_cast<std::size_t>(n) > kMaxCapture) {
        buf->append(tmp, kMaxCapture - buf->size());
        *truncated = true;
        return true;  // 停止读，由调用方杀进程
      }
      buf->append(tmp, static_cast<std::size_t>(n));
      continue;
    }
    if (n == 0) return true;  // EOF
    if (errno == EINTR) continue;
    return true;
  }
}

}  // namespace

RunCaseResult run_case(const RunCaseOptions& options) {
  RunCaseResult result;

  int out_pipe[2] = {-1, -1};
  int err_pipe[2] = {-1, -1};
  if (::pipe(out_pipe) != 0 || ::pipe(err_pipe) != 0) {
    result.kill = CaseKillReason::kSpawnFailed;
    return result;
  }

  const std::string binary = options.workspace_dir + "/" + kRunnerBinary;
  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(out_pipe[0]); ::close(out_pipe[1]);
    ::close(err_pipe[0]); ::close(err_pipe[1]);
    result.kill = CaseKillReason::kSpawnFailed;
    return result;
  }

  if (pid == 0) {
    // ---- 子进程 ----
    ::setpgid(0, 0);
    ::dup2(out_pipe[1], STDOUT_FILENO);
    ::dup2(err_pipe[1], STDERR_FILENO);
    ::close(out_pipe[0]); ::close(out_pipe[1]);
    ::close(err_pipe[0]); ::close(err_pipe[1]);

    const int devnull = ::open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
      ::dup2(devnull, STDIN_FILENO);
      if (devnull > STDERR_FILENO) ::close(devnull);
    }

    if (::chdir(options.workspace_dir.c_str()) != 0) ::_exit(127);

    // 五档 rlimit（§5.4）
    struct rlimit rl;
    rl.rlim_cur = rl.rlim_max = options.time_limit_ms / 1000ull + 1ull;
    ::setrlimit(RLIMIT_CPU, &rl);
    rl.rlim_cur = rl.rlim_max = options.mem_limit_kb * 1024ull;
    ::setrlimit(RLIMIT_AS, &rl);
    rl.rlim_cur = rl.rlim_max = 1ull * 1024ull * 1024ull;  // 1MB
    ::setrlimit(RLIMIT_FSIZE, &rl);
    rl.rlim_cur = rl.rlim_max = 64;
    ::setrlimit(RLIMIT_NOFILE, &rl);
    rl.rlim_cur = rl.rlim_max = 0;
    ::setrlimit(RLIMIT_CORE, &rl);

    ::setenv("MALLOC_ARENA_MAX", "1", 1);
    ::setenv("LC_ALL", "C", 1);

    char idx_buf[16];
    std::snprintf(idx_buf, sizeof idx_buf, "%d", options.case_idx);
    // argv[1] = 用例序号：tester.cpp 的 switch(idx) 靠它选用例，
    // 缺了它 tester 直接 return 2 → 每个用例都误判 RE
    char* const argv[] = {const_cast<char*>(binary.c_str()), idx_buf, nullptr};
    ::execv(binary.c_str(), argv);
    ::_exit(127);
  }

  // ---- 父进程 ----
  ::setpgid(pid, pid);  // 双保险：子进程 setpgid 之前也能 kill 到组
  ::close(out_pipe[1]);
  ::close(err_pipe[1]);
  const int flags_out = ::fcntl(out_pipe[0], F_GETFL, 0);
  const int flags_err = ::fcntl(err_pipe[0], F_GETFL, 0);
  ::fcntl(out_pipe[0], F_SETFL, flags_out | O_NONBLOCK);
  ::fcntl(err_pipe[0], F_SETFL, flags_err | O_NONBLOCK);

  Capture cap;
  const Clock::time_point start = Clock::now();
  const long long fence = options.fence_wall_ms > 0 ? options.fence_wall_ms : 35000;
  int zero_streak = 0;
  long long last_cpu = 0;
  bool killed = false;
  int status = 0;
  bool exited = false;

  while (!killed) {
    struct pollfd pfds[2];
    pfds[0].fd = out_pipe[0]; pfds[0].events = POLLIN;
    pfds[1].fd = err_pipe[0]; pfds[1].events = POLLIN;
    const int pr = ::poll(pfds, 2, kStallIntervalMs);
    if (pr > 0) {
      if (pfds[0].revents & (POLLIN | POLLHUP)) drain_fd(out_pipe[0], &cap.out, &cap.truncated);
      if (pfds[1].revents & (POLLIN | POLLHUP)) drain_fd(err_pipe[0], &cap.err, &cap.truncated);
    }
    if (cap.truncated) {
      kill_group(pid);
      killed = true;
      break;
    }

    const pid_t done = ::waitpid(pid, &status, WNOHANG);
    if (done == pid) {
      exited = true;
      break;
    }

    // CPU stall：连续 3 次 500ms CPU 增量为 0 → TLE（§5.4.2）
    long long cpu = 0;
    if (read_child_cpu_ms(pid, &cpu)) {
      if (cpu - last_cpu <= 0) {
        if (++zero_streak >= kStallPolls) {
          kill_group(pid);
          result.timed_out = true;
          result.kill = CaseKillReason::kTle;
          killed = true;
          break;
        }
      } else {
        zero_streak = 0;
      }
      last_cpu = cpu;
    }

    if (wall_ms_since(start) > fence) {
      kill_group(pid);
      result.timed_out = true;
      result.kill = CaseKillReason::kTle;
      killed = true;
      break;
    }
  }

  if (!exited) {
    // 被我们杀掉：回收
    ::waitpid(pid, &status, 0);
  }
  // 收尾读剩余
  fcntl(out_pipe[0], F_SETFL, flags_out);
  fcntl(err_pipe[0], F_SETFL, flags_err);
  drain_fd(out_pipe[0], &cap.out, &cap.truncated);
  drain_fd(err_pipe[0], &cap.err, &cap.truncated);
  ::close(out_pipe[0]);
  ::close(err_pipe[0]);

  result.stdout_text = std::move(cap.out);
  result.stderr_text = std::move(cap.err);
  result.truncated = cap.truncated;

  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
    result.term_signal = 0;
    if (result.kill == CaseKillReason::kNone) {
      if (result.exit_code == 0) {
        result.kill = CaseKillReason::kNone;
      } else if (result.exit_code == 127) {
        result.kill = CaseKillReason::kSpawnFailed;
      } else {
        // harness 自身失败：可能是 bad_alloc（134/abort）或用户代码 throw 未捕获
        result.kill = CaseKillReason::kRe;
      }
    }
  } else if (WIFSIGNALED(status)) {
    result.term_signal = WTERMSIG(status);
    result.exit_code = -1;
    if (result.kill == CaseKillReason::kNone) {
      if (result.term_signal == SIGXCPU || result.term_signal == SIGKILL ||
          result.term_signal == SIGXFSZ) {
        // SIGKILL 可能来自 stall/墙钟（已标记）或 OOM killer；未标记时按 TLE 处理保守
        // RLIMIT_AS 超限通常是 bad_alloc → exit，不是信号
        if (result.term_signal == SIGXFSZ) {
          result.kill = CaseKillReason::kMle;  // 写超 1MB → 视作资源超限
        } else {
          result.kill = CaseKillReason::kTle;
          result.timed_out = true;
        }
      } else {
        result.kill = CaseKillReason::kRe;
      }
    }
  }

  return result;
}

}  // namespace judge
}  // namespace oj
