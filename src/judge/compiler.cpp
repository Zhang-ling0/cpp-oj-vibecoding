// =============================================================================
// 编译模块实现：两阶段 spawn + 超时回收 + g++ 输出解析 + 归属分流
// 被测对象：include/judge/compiler.h（SPEC.md §9 M2 T2.1 / T2.2）
//
// 【实现要点】
//   1. spawn 与回收只有一个函数 run_command()。两阶段编译的差异只有"argv 不同"，
//      超时/截断/进程组 kill 的逻辑一旦写两遍，两处就会在不同时期各自演化 ——
//      而这正是 §8.1.1 说的"两处各自合理的设计放在一起就矛盾"的形状。
//   2. 输出解析是纯函数（parse_compile_output），与 spawn 完全分离。因此"输出长什么样"
//      这类问题可以在单测里穷举字符串解决，不需要真的跑 g++（真跑的用例只留 4 条，
//      因为本机 2 核上每条真跑都要几百毫秒，见【测试为什么这么分】）。
//   3. 归属判定只有一个函数 owner_of()，判据是【文件名】而不是路径：
//      用户代码在工作区里只有 user.cpp 一个文件，其余一切路径（生成文件、系统头、
//      库）都不是用户的。这个判据不需要看路径前缀，因此 g++ 用相对路径还是绝对
//      路径输出都得出同一结论。
//   4. 子进程环境只改三样，且都写明理由（见 spawn_child）：LC_ALL=C、MALLOC_ARENA_MAX、
//      stdin 接 /dev/null。默认不设的项（如 TMPDIR）保持继承，避免造出第二套环境。
// =============================================================================
#include "judge/compiler.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <regex>
#include <utility>

#include "common/log.h"

namespace oj {
namespace judge {
namespace {

using Clock = std::chrono::steady_clock;

// ---- SIGALRM：handler 只置标志，不做任何系统调用 ---------------------------------
// 理由见本文件头部【超时机制】。标志是 volatile sig_atomic_t 而不是 std::atomic<bool>：
// 信号处理函数里能安全碰的类型只有 lock-free 的原子与 sig_atomic_t，后者还额外保证
// 不会被优化成一次缓存的读。
volatile std::sig_atomic_t g_alarm_fired = 0;

extern "C" void on_alarm_signal(int) { g_alarm_fired = 1; }

// 装 handler 并 disarm 上一次遗留的 alarm，返回旧的 action 用于还原。
// 刻意【还原】而不是一装了之：本模块是库代码，进程里若已有别的模块用 SIGALRM
// （比如 runner 的兜底计时），覆盖后不还回去就会让它静默失效 —— 这种 bug 表现为
// "另一个功能偶尔不触发"，定位成本极高。
struct ScopedAlarm {
  ScopedAlarm(unsigned int timeout_ms) {
    g_alarm_fired = 0;  // 上一轮遗留的标志必须先清：不清的话下一次编译会【立刻】被判超时
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alarm_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // 不设 SA_RESTART：让 poll 以 EINTR 返回，循环立刻看到标志
    ::sigaction(SIGALRM, &sa, &previous_);
    ::alarm((timeout_ms + 999u) / 1000u);  // alarm 粒度是秒，向上取整
    armed_ = true;
  }
  ~ScopedAlarm() { disarm(); }

  // 提前解除计时：回收循环之后的阻塞 waitpid 不该再被 SIGALRM 打断。
  void disarm() {
    if (!armed_) return;
    ::alarm(0);
    ::sigaction(SIGALRM, &previous_, nullptr);
    armed_ = false;
  }

  bool fired() const { return g_alarm_fired != 0; }

  ScopedAlarm(const ScopedAlarm&) = delete;
  ScopedAlarm& operator=(const ScopedAlarm&) = delete;

 private:
  struct sigaction previous_ {};
  bool armed_ = false;
};

// 判据里用到的数字转换：正则已保证是纯数字，因此唯一可能出问题的是溢出。
// 用手写转换而不是 std::stoul：后者溢出时抛 std::out_of_range，而这里是 worker
// 的判定路径 —— 一个诊断行号不该有能力把评测线程抛下去。
unsigned int to_unsigned(const std::string& digits) {
  unsigned long long value = 0;
  for (const char c : digits) {
    if (value > 0xFFFFFFFFull) return 0xFFFFFFFFu;
    value = value * 10ull + static_cast<unsigned long long>(c - '0');
  }
  return static_cast<unsigned int>(value);
}

// ---- 编译闸门（默认 1，见头文件【开发机实测约束】）--------------------------------
std::mutex g_gate_mutex;
std::condition_variable g_gate_cv;
unsigned int g_gate_slots = 1;
unsigned int g_gate_in_flight = 0;

class CompileSlot {
 public:
  CompileSlot() {
    std::unique_lock<std::mutex> lock(g_gate_mutex);
    g_gate_cv.wait(lock, [] { return g_gate_in_flight < g_gate_slots; });
    ++g_gate_in_flight;
  }
  ~CompileSlot() {
    {
      std::lock_guard<std::mutex> lock(g_gate_mutex);
      --g_gate_in_flight;
    }
    g_gate_cv.notify_one();
  }
  CompileSlot(const CompileSlot&) = delete;
  CompileSlot& operator=(const CompileSlot&) = delete;
};

// ---- spawn 子进程：把 argv 变成一次带管道捕获的执行 -------------------------------
struct CommandResult {
  bool spawn_failed = false;
  bool timed_out = false;
  bool truncated = false;
  int exit_code = -1;
  int term_signal = 0;
  std::string output;
};

bool is_directory(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool file_exists(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0;
}

// 子进程里做：脱离进程组 → 重定向三条流 → chdir → 环境 → exec。
// 放在独立函数里是为了让主函数读起来只剩"fork 之后等它"，因为这一段里每一步的顺序
// 都有原因（见各处注释），混在一起会变成一堆搬移代码。
void spawn_child(const std::vector<std::string>& argv, const std::string& cwd, int pipe_fd) {
  // 独立进程组：超时/截断时 kill(-pgid) 才能连 cc1plus 与 collect2 一起收掉，
  // 只杀 g++ 本身会留下继续吃 CPU 的孤儿（§5.4.3 的同一条要求）。
  ::setpgid(0, 0);

  // ⚠️ 这里【不能】先 close(pipe_fd)：dup2 的目标 fd 必须仍然是打开的。
  //   先关再 dup2 会让两次 dup2 都以 EBADF 失败，子进程就继续用继承来的 stdout/stderr ——
  //   表现为"编译器输出直接打到终端、result.output 永远是空的"，而且不报任何错。
  const int devnull = ::open("/dev/null", O_RDONLY);
  if (devnull >= 0) {
    ::dup2(devnull, STDIN_FILENO);  // 用户代码不该读到任何 stdin
    if (devnull > STDERR_FILENO) ::close(devnull);
  }
  ::dup2(pipe_fd, STDOUT_FILENO);
  ::dup2(pipe_fd, STDERR_FILENO);  // 合并：顺序即原序，理由见头文件【输出捕获】

  if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) {
    const std::string message = std::string("judge: chdir 到工作目录失败: ") + cwd +
                                " (errno=" + std::strerror(errno) + ")\n";
    ssize_t ignored = ::write(STDERR_FILENO, message.data(), message.size());
    (void)ignored;
    ::_exit(127);
  }

  // LC_ALL=C：正则的关键词（error:/warning:/note:/undefined reference）是英文的。
  // 万一某天工具链按 locale 输出本地化关键词，解析会静默变成"零条诊断"——
  // 那是最坏的一种失败：看起来判了 CE，却什么诊断都没有。锁死 C locale 让这种漂移
  // 在本模块就被消掉，而不是留给用户去发现。
  ::setenv("LC_ALL", "C", 1);
  ::setenv("LANG", "C", 1);
  // glibc 的每个 malloc arena 在 64 位上会预留 64MB 地址空间，且线程数一多就成倍增加。
  // 1967MB 的机器上这一步能把 cc1plus 的虚拟占用压掉一大截（本文件的开发机约束）。
  ::setenv("MALLOC_ARENA_MAX", "1", 1);

  std::vector<char*> raw;
  raw.reserve(argv.size() + 1);
  for (const std::string& arg : argv) raw.push_back(const_cast<char*>(arg.c_str()));
  raw.push_back(nullptr);

  ::execvp(raw[0], raw.data());
  const std::string message = std::string("judge: 无法执行 ") + argv[0] +
                              " (errno=" + std::strerror(errno) + ")\n";
  ssize_t ignored = ::write(STDERR_FILENO, message.data(), message.size());
  (void)ignored;
  ::_exit(127);
}

std::string join_command(const std::vector<std::string>& argv) {
  std::string text;
  for (const std::string& arg : argv) {
    if (!text.empty()) text += ' ';
    text += arg;  // argv 全部由本模块拼出（路径与固定 flag），不含用户输入，无需转义
  }
  return text;
}

// 杀掉进程组。ESRCH（子进程还没来得及 setpgid 就被判超时）时退回杀直接子进程 ——
// 否则会漏下一个真正的孤儿进程，表现为"评测偶尔卡住"，且极难复现。
void kill_group(pid_t pid) {
  if (::kill(-pid, SIGKILL) != 0 && errno == ESRCH) ::kill(pid, SIGKILL);
}

CommandResult run_command(const std::vector<std::string>& argv, const std::string& cwd,
                          unsigned int timeout_ms, std::size_t max_output_bytes) {
  CommandResult result;
  int fds[2] = {-1, -1};
  if (::pipe(fds) != 0) {
    result.spawn_failed = true;
    result.output = std::string("judge: pipe() 失败: ") + std::strerror(errno) + "\n";
    return result;
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    result.spawn_failed = true;
    result.output = std::string("judge: fork() 失败: ") + std::strerror(errno) + "\n";
    ::close(fds[0]);
    ::close(fds[1]);
    return result;
  }
  if (pid == 0) {
    ::close(fds[0]);
    spawn_child(argv, cwd, fds[1]);
    ::_exit(127);  // 不可达；spawn_child 的每条路径都以 _exit 收尾
  }
  ::close(fds[1]);  // 父进程只读；留着写端会让 EOF 永远不来

  const auto start = Clock::now();
  bool eof = false;
  bool exited = false;
  int status = 0;
  ScopedAlarm timer(timeout_ms);

  char buffer[4096];
  while (!eof) {
    struct pollfd pfd;
    pfd.fd = fds[0];
    pfd.events = POLLIN;
    pfd.revents = 0;
    const int polled = ::poll(&pfd, 1, 50);

    if (polled > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      const ssize_t got = ::read(fds[0], buffer, sizeof(buffer));
      if (got > 0) {
        if (result.output.size() < max_output_bytes) {
          const std::size_t room = max_output_bytes - result.output.size();
          result.output.append(buffer, std::min(room, static_cast<std::size_t>(got)));
        }
        if (result.output.size() >= max_output_bytes) {
          result.truncated = true;
          kill_group(pid);
        }
      } else if (got == 0) {
        eof = true;
      } else if (errno != EINTR && errno != EAGAIN) {
        // 读端出错后【必须】先杀掉子进程再退出循环：一旦停止读管道，写满管道的子进程
        // 会永远阻塞在 write 上，而下面那句阻塞 waitpid 就再也等不回来了（死锁）。
        kill_group(pid);
        eof = true;
      }
    }

    if (!exited) {
      const pid_t waited = ::waitpid(pid, &status, WNOHANG);
      if (waited == pid) exited = true;
      else if (waited < 0 && errno != EINTR) exited = true;
    }

    if (eof) break;

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                Clock::now() - start)
                                .count();
    // 两道判据指向同一个处置（头文件【超时机制】：alarm 是规格要求的机制，墙钟是它的兜底）
    if (timer.fired() || elapsed_ms >= static_cast<long long>(timeout_ms)) {
      result.timed_out = true;
      kill_group(pid);
    }
  }

  ::close(fds[0]);
  timer.disarm();  // 下面的阻塞 waitpid 不该再被 SIGALRM 打断

  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }

  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.term_signal = WTERMSIG(status);
    result.exit_code = 128 + result.term_signal;  // 与 shell 口径一致，便于日志对照
  }

  if (result.truncated) {
    result.output +=
        "\n[oj] 编译器输出超过上限，已截断（其余内容未采集）；"
        "可用 -fmax-errors 限制诊断条数后重试。\n";
  }
  return result;
}

// ---- 输出解析（纯函数）----------------------------------------------------------

// §5.6 给出的正则，扩展了两处：
//   · 文件名允许不含 ':'（即 [^:\n]+）—— g++ 的文件名不会含冒号，而加了 '\n' 边界后
//     整个匹配被钉在单行内，避免一条诊断吃掉多行
//   · severity 多收一个 "fatal error"：它通常没有 line:col 因而落不到这里，但
//     带位置时（如 -fpermissive 下的某些 GCC 版本）会出现，漏掉就会静默丢诊断
const std::regex& diagnostic_regex() {
  static const std::regex re(
      R"(^([^:\n]+):(\d+):(\d+):[ \t]*(fatal error|error|warning|note):[ \t]*(.+)$)");
  return re;
}

std::string basename_of(const std::string& path) {
  const std::size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// 归属判定：唯一的判据是【文件名】。理由见本文件【实现要点】3。
//   user.cpp → 用户代码
//   tester.cpp / solution_decl.h / tester_util.h → 平台生成的 harness（题库配置有误）
//   其余（系统头 / 库 / 任何未知路径）→ 同样归 harness：它们同样不是用户写的，
//   对用户说"这不是你的代码出错"仍然成立（这句话对 -I 路径上的头文件一样准确）。
DiagnosticOwner owner_of(const std::string& base) {
  return base == kUserSource ? DiagnosticOwner::kUserCode : DiagnosticOwner::kHarness;
}

bool is_link_error(const std::string& output) {
  // 只认这两个关键词（§5.6 原文）。刻意【不】把 collect2 的 "ld returned 1 exit status"
  // 算进来：它只是摘要，真正的因果在前一行；而把它算进来会让"链接器因为别的原因
  // 失败"也被贴上"你的签名不一致"这张标签 —— 一个不准确的归因比没有归因更糟。
  static const std::regex re(R"((undefined reference|undefined symbol))");
  return std::regex_search(output, re);
}

}  // namespace

const char* to_string(CompileStage stage) {
  switch (stage) {
    case CompileStage::kNone: return "none";
    case CompileStage::kUserObject: return "user-object";
    case CompileStage::kHarnessLink: return "harness-link";
  }
  return "unknown";
}

const char* to_string(DiagnosticOwner owner) {
  switch (owner) {
    case DiagnosticOwner::kUserCode: return kUserSource;
    case DiagnosticOwner::kHarness: return "harness";
    case DiagnosticOwner::kLink: return "link";
  }
  return "unknown";
}

const char* to_string(DiagnosticSeverity severity) {
  switch (severity) {
    case DiagnosticSeverity::kError: return "error";
    case DiagnosticSeverity::kWarning: return "warning";
    case DiagnosticSeverity::kNote: return "note";
  }
  return "error";
}

const char* harness_ownership_message() {
  return "这不是你的代码出错。平台为本题自动生成的测试代码编译失败，"
         "通常说明该题的配置有误。你的代码无需修改；若确认代码正确，请联系管理员检查题库。";
}

const char* link_signature_message() {
  return "你的 Solution 类的方法名或函数签名与题目要求不一致。"
         "请核对题面「函数签名」一节，确保类名、方法名、参数类型、返回类型完全一致。"
         "常见原因：漏写 public、少了 vector<int>& 的引用符号、用了 int 而非 long long。";
}

unsigned int compile_max_concurrent() {
  std::lock_guard<std::mutex> lock(g_gate_mutex);
  return g_gate_slots;
}

void set_compile_max_concurrent(unsigned int slots) {
{
      std::lock_guard<std::mutex> lock(g_gate_mutex);
      // 0 不是"无限并发"而是无效配置：闸门为 0 时 CompileSlot 会永远等下去，
      // 表现为"评测全部卡住"。按 1 处理，让它退化成最保守的行为而不是死锁。
      g_gate_slots = slots == 0 ? 1u : slots;
    }
  g_gate_cv.notify_all();
}

std::vector<CompileDiagnostic> parse_compile_output(const std::string& output) {
  std::vector<CompileDiagnostic> diagnostics;

  std::size_t begin = 0;
  while (begin <= output.size()) {
    std::size_t end = output.find('\n', begin);
    if (end == std::string::npos) end = output.size();
    const std::string line = output.substr(begin, end - begin);
    begin = end + 1;
    if (line.empty()) continue;

    std::smatch match;
    if (!std::regex_match(line, match, diagnostic_regex())) continue;

    // 分组编号：1=文件 2=行 3=列 4=等级 5=正文。
    // 写成注释而不是靠记：错位时 severity 会静默变成 error、message 会错位一格，
    // 而这两种都不会崩，只会让"warning/note 被当成 error 显示"这类问题混过去。
    const std::string severity_text = match[4].str();
    DiagnosticSeverity severity = DiagnosticSeverity::kError;
    if (severity_text == "warning") {
      severity = DiagnosticSeverity::kWarning;
    } else if (severity_text == "note") {
      severity = DiagnosticSeverity::kNote;
    }

    const std::string base = basename_of(match[1].str());
    const DiagnosticOwner owner = owner_of(base);
    CompileDiagnostic diagnostic;
    diagnostic.seq = static_cast<unsigned int>(diagnostics.size() + 1);
    diagnostic.line = to_unsigned(match[2].str());
    diagnostic.col = to_unsigned(match[3].str());
    diagnostic.severity = severity;
    diagnostic.owner = owner;
    if (owner == DiagnosticOwner::kUserCode) {
      diagnostic.file_name = kUserSource;
      diagnostic.message = match[5].str();
    } else {
      // 归属分流 [D-64]：原始 message 被替换成专属文案（用户看"这是题库的问题"，
      // 而不是"tester.cpp 第 12 行"这类他既改不了也找不到的定位）。原始文本没有丢：
      // 它整段留在 CompileResult::output 里，而全文始终展示（§5.6 最后一句）。
      diagnostic.file_name = to_string(DiagnosticOwner::kHarness);
      diagnostic.message = harness_ownership_message();
    }
    diagnostics.push_back(std::move(diagnostic));
  }

  if (is_link_error(output)) {
    // 链接期错误没有 file:line:col（§5.6），因此 line/col 保持 0，前端据此不提供高亮。
    // 追加在末尾而不是插在命中位置：它与前面的诊断不是同一类（不是"某一行写错了"，
    // 而是"整体签名对不上"），混在行号序列里会让编辑器按一个不存在的行去标红。
    CompileDiagnostic diagnostic;
    diagnostic.seq = static_cast<unsigned int>(diagnostics.size() + 1);
    diagnostic.file_name = to_string(DiagnosticOwner::kLink);
    diagnostic.severity = DiagnosticSeverity::kError;
    diagnostic.owner = DiagnosticOwner::kLink;
    diagnostic.message = link_signature_message();
    diagnostics.push_back(std::move(diagnostic));
  }

  return diagnostics;
}

CompileResult compile_submission(const std::string& workspace_dir,
                                 const CompileOptions& options) {
  CompileResult result;

  // 工作区不存在 / 没有 user.cpp：这不是编译失败，是上游没准备好文件。
  // stage=kNone 让调用点能把它与"用户代码编译不过"分开（后者是 CE）—— §5.6.2 的
  // 判定表里没有这一行，因为按 T1.5/T2.5.2 的契约它不该发生；真发生了判 JE。
  if (!is_directory(workspace_dir) || !file_exists(workspace_dir + "/" + kUserSource)) {
    result.output = "judge: 工作区不可用（缺少 " + std::string(kUserSource) + "）：" + workspace_dir +
                    "\n";
    return result;
  }

  CompileSlot slot;  // 编译闸门：容量默认 1，见头文件【开发机实测约束】

  const auto log_stage = [&](const char* event, const std::string& command,
                             const CommandResult& raw) {
    if (!options.write_judge_log) return;
    common::LogFields fields{
        {"submission_dir", workspace_dir},
        {"command", command},
        {"exit_code", std::to_string(raw.exit_code)},
        {"term_signal", std::to_string(raw.term_signal)},
        {"timed_out", raw.timed_out ? "1" : "0"},
        {"truncated", raw.truncated ? "1" : "0"},
        {"output_bytes", std::to_string(raw.output.size())},
    };
    common::log_kv(common::LogChannel::kJudge, common::LogLevel::kInfo, event, fields);
  };
  const auto log_output = [&](const char* title, const std::string& output) {
    if (!options.write_judge_log || output.empty()) return;
    // log_block 而不是 log_write：编译器输出是多行，行不变量要求每行自带前缀，
    // 而这里要保留 g++ 原始的缩进与 `^~~~` 指示符（块内不缩进，便于人读，见 log.h）。
    common::log_block(common::LogChannel::kJudge, common::LogLevel::kInfo, title, output);
  };

  // ---- 阶段一：只编用户代码（§5.2）--------------------------------------------
  // -fkeep-inline-functions：类内定义的成员函数隐式 inline，-c 单独编译时若本 TU
  // 没有调用点，GCC 不会把它落进 user.o，阶段二链接必报 undefined reference
  // （题库 22/26 个 code_template 都是类内定义，不加此旗标正确代码全判 CE）。
  const std::vector<std::string> stage1{
      options.compiler,        options.language_standard, options.optimization,
      options.warnings,        "-fkeep-inline-functions", "-c",
      kUserSource,             "-o",                      kUserObject};
  result.commands.push_back(join_command(stage1));
  const CommandResult raw1 =
      run_command(stage1, workspace_dir, options.timeout_ms, options.max_output_bytes);
  log_stage("compile.stage1", result.commands.back(), raw1);

  result.output += raw1.output;
  result.exit_code = raw1.exit_code;
  result.term_signal = raw1.term_signal;
  result.timed_out = raw1.timed_out;
  result.output_truncated = raw1.truncated;
  result.user_object_created = file_exists(workspace_dir + "/" + kUserObject);

  if (raw1.spawn_failed || raw1.timed_out || raw1.exit_code != 0) {
    result.stage = CompileStage::kUserObject;
    result.diagnostics = parse_compile_output(raw1.output);
    log_output("阶段一编译器输出（user.cpp）", raw1.output);
    return result;
  }

  // ---- 阶段二：链接 harness（§5.2）--------------------------------------------
  // 刻意【不加 -Wall】：-Wall 的 warning 全来自 harness 自己（生成代码里的未用参数、
  // 有符号/无符号比较），它们会灌进用户看到的编译输出却与用户代码无关 —— 噪声会
  // 让人以为是自己的代码有问题。
  const std::vector<std::string> stage2{
      options.compiler, options.language_standard, options.optimization,
      kUserObject,       kTesterSource,             "-o",
      kRunnerBinary};
  result.commands.push_back(join_command(stage2));
  const CommandResult raw2 =
      run_command(stage2, workspace_dir, options.timeout_ms, options.max_output_bytes);
  log_stage("compile.stage2", result.commands.back(), raw2);

  result.output += raw2.output;
  result.exit_code = raw2.exit_code;
  result.term_signal = raw2.term_signal;
  result.timed_out = raw2.timed_out;
  result.output_truncated = result.output_truncated || raw2.truncated;
  result.binary_created = file_exists(workspace_dir + "/" + kRunnerBinary);

  if (raw2.spawn_failed || raw2.timed_out || raw2.exit_code != 0) {
    result.stage = CompileStage::kHarnessLink;
    result.diagnostics = parse_compile_output(raw2.output);
    log_output("阶段二编译器输出（链接）", raw2.output);
    return result;
  }

  result.ok = true;
  return result;
}

}  // namespace judge
}  // namespace oj